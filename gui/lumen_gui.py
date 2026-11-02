#!/usr/bin/python3
"""Lumen: a small control panel for streaming SteamVR to a phone.

Starts and stops the streamer (driver/build/lumen-capture) and SteamVR,
applies headset presets to SteamVR's settings, and shows live status.

GTK 4 + libadwaita via PyGObject; uses the system Python (/usr/bin/python3).
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, GLib, Gtk  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURE = os.path.join(REPO, "driver", "build", "lumen-capture")
CONFIG_PATH = os.path.expanduser("~/.config/lumen/gui.json")
STEAMVR_SETTINGS = os.path.expanduser("~/.local/share/Steam/config/steamvr.vrsettings")
STEAMVR_APP_ID = "250820"
# Small PNG of the stream that lumen-capture rewrites ~4x a second (--preview).
# SteamVR's own "Display VR View" stays black with Lumen's driver.
# Full-size screenshots (the "Save screenshot" button) go here.
SCREENSHOT_DIR = os.path.join(GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_PICTURES)
                              or os.path.expanduser("~/Pictures"), "Lumen")
PREVIEW_PATH = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "lumen-preview.png")
# lumen-capture --mirror sends the encoded stream here too; the video window plays it.
MIRROR_PORT = 9947

CODECS = [("HEVC", "hevc"), ("H.264", "h264")]

# Headset presets: what the driver renders. SteamVR makes each eye's image
# 1.5x the per-eye render size.
PRESETS = [
    {
        "name": "Automatic (from the phone)",
        # Nothing written: the phone reports the eye size and FOV it wants and
        # the driver saves them, asking for a SteamVR restart when they change.
        "description": "The phone sets the resolution and FOV (Gear VR: 1536x1536 eyes; phone: its screen); laser hidden",
        "driver_lumen": {},
        "hide_laser": True,
    },
    {
        "name": "Gear VR",
        # 1024 render x SteamVR's 1.5 = 1536x1536 per eye (~1.5x VrApi's
        # suggested 1024, as Oculus apps use for sharp text). 92 degrees: the
        # Gear VR shows ~90, plus a little margin for reprojection.
        "description": "Sharp: 1536x1536 eyes (3072x1536 stream), 92° FOV; laser hidden. Use 40–50 Mbit/s",
        "driver_lumen": {"renderWidth": 1024, "renderHeight": 1024, "fovDegrees": 92.0},
        "hide_laser": True,
    },
    {
        "name": "Gear VR (light)",
        "description": "1152x1152 eyes (2304x1152 stream), 100° FOV; laser hidden. Less decode/Wi-Fi load",
        "driver_lumen": {"renderWidth": 768, "renderHeight": 768, "fovDegrees": 100.0},
        "hide_laser": True,
    },
    {
        "name": "Phone screen (Galaxy S7)",
        "description": "2560x1440 stream (the S7's native screen), 95° FOV",
        "driver_lumen": {"renderWidth": 853, "renderHeight": 960, "fovDegrees": 95.0},
        "hide_laser": False,
    },
]

DEFAULT_CONFIG = {"phone_ip": "", "codec": "hevc", "bitrate": 30, "preset": "Automatic (from the phone)",
                  "height_m": 1.75, "seated": False}

# Tracking is rotation-only, so the driver puts the eyes at a fixed height
# (driver_lumen headHeightMeters). Eyes sit ~11 cm below the top of the head
# standing; seated eye height is about 69% of standing height.
def eye_height(height_m, seated):
    return round(height_m * 0.69 if seated else height_m - 0.11, 2)


def load_config():
    try:
        with open(CONFIG_PATH) as f:
            return {**DEFAULT_CONFIG, **json.load(f)}
    except (OSError, ValueError):
        return dict(DEFAULT_CONFIG)


def save_config(config):
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        json.dump(config, f, indent=2)


def process_running(name):
    return subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0


def steamvr_running():
    return process_running("vrserver")


def apply_preset(preset, head_height):
    """Writes the preset and eye height into steamvr.vrsettings. Only safe while SteamVR is
    stopped: SteamVR rewrites the file on exit. Keeps a one-time backup."""
    with open(STEAMVR_SETTINGS) as f:
        settings = json.load(f)
    backup = STEAMVR_SETTINGS + ".before-lumen-gui"
    if not os.path.exists(backup):
        shutil.copy2(STEAMVR_SETTINGS, backup)

    settings.setdefault("steamvr", {})["forcedDriver"] = "lumen"
    lumen = settings.setdefault("driver_lumen", {})
    lumen.update(preset["driver_lumen"])
    lumen["headHeightMeters"] = head_height
    # Without this SteamVR drops to 10 fps when the headset is still.
    settings.setdefault("power", {})["pauseCompositorOnStandby"] = False
    dashboard = settings.setdefault("dashboard", {})
    if preset["hide_laser"]:
        dashboard["laserRayWidthScale"] = 0.0  # the gaze pointer's laser looks doubled
    else:
        dashboard.pop("laserRayWidthScale", None)
    with open(STEAMVR_SETTINGS, "w") as f:
        json.dump(settings, f, indent=3)


def detect_phone_ip():
    """The Wi-Fi address of the first phone connected over USB (adb), or None."""
    try:
        out = subprocess.run(["adb", "shell", "ip", "-4", "addr", "show", "wlan0"], capture_output=True,
                             text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", out)
    return match.group(1) if match else None


def stop_steamvr():
    # Closing vrmonitor shuts SteamVR down; vrserver follows.
    subprocess.run(["pkill", "-x", "vrmonitor"])
    subprocess.run(["pkill", "-x", "vrserver"])


class VideoWindow(Gtk.Window):
    """The transmitted video, live and full rate: the same encoded stream the
    phone gets (lumen-capture --mirror), decoded on the GPU by GStreamer.
    Plays only while open."""

    def __init__(self, parent, codec, on_closed):
        super().__init__(title="Lumen: headset video", default_width=1280, default_height=680,
                         application=parent.get_application())
        self.on_closed = on_closed
        self.pipeline = None
        self.picture = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, can_shrink=True)
        self.status = Gtk.Label(label="Waiting for video… (a keyframe can take a second)", margin_top=24,
                                margin_bottom=24)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(self.status)
        box.append(self.picture)
        self.picture.set_vexpand(True)
        self.set_child(box)
        self.connect("close-request", self.on_close)
        try:
            gi.require_version("Gst", "1.0")
            from gi.repository import Gst
            Gst.init(None)
            name = "h265" if codec == "hevc" else "h264"
            # nv*dec: NVIDIA GPU decode; fall back to the software decoder.
            decoder = f"nv{name}dec" if Gst.ElementFactory.find(f"nv{name}dec") else f"avdec_{name}"
            self.pipeline = Gst.parse_launch(
                f"udpsrc port={MIRROR_PORT} buffer-size=8388608 caps=video/x-{name},stream-format=byte-stream "
                f"! {name}parse ! {decoder} ! videoconvert ! gtk4paintablesink name=sink sync=false")
            paintable = self.pipeline.get_by_name("sink").get_property("paintable")
            self.picture.set_paintable(paintable)
            # Hide the waiting note once the first frame is in.
            paintable.connect("invalidate-contents", lambda *_: self.status.set_visible(False))
            bus = self.pipeline.get_bus()
            bus.add_signal_watch()
            bus.connect("message", self.on_message)
            self.pipeline.set_state(Gst.State.PLAYING)
        except (ValueError, ImportError, GLib.Error) as e:
            self.status.set_label(f"Can't play the video here: {e}\n(needs GStreamer with gtk4paintablesink)")
            self.pipeline = None

    def on_message(self, _bus, message):
        from gi.repository import Gst
        if message.type == Gst.MessageType.ERROR:
            self.status.set_label(f"Video error: {message.parse_error()[0].message}")
            self.status.set_visible(True)

    def on_close(self, _window):
        if self.pipeline:
            from gi.repository import Gst
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        self.on_closed()
        return False


class LumenWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Lumen", default_width=560, default_height=760)
        self.config = load_config()
        self.capture = None  # subprocess.Popen of lumen-capture
        self.stopping = False

        self.toasts = Adw.ToastOverlay()
        toolbar = Adw.ToolbarView()
        toolbar.add_top_bar(Adw.HeaderBar())
        toolbar.set_content(self.toasts)
        self.set_content(toolbar)

        page = Adw.PreferencesPage()
        self.toasts.set_child(page)

        # --- Status
        status = Adw.PreferencesGroup(title="Status")
        self.steamvr_row = Adw.ActionRow(title="SteamVR")
        self.streamer_row = Adw.ActionRow(title="Streamer")
        self.stream_row = Adw.ActionRow(title="Stream", subtitle="—")
        for row in (self.steamvr_row, self.streamer_row, self.stream_row):
            status.add(row)
        page.add(status)

        # --- Connection
        connection = Adw.PreferencesGroup(title="Phone",
                                          description="The phone's Wi-Fi address; head tracking comes back "
                                                      "automatically.")
        self.ip_row = Adw.EntryRow(title="Phone IP address")
        self.ip_row.set_text(self.config["phone_ip"])
        detect = Gtk.Button(label="Detect over USB", valign=Gtk.Align.CENTER)
        detect.add_css_class("flat")
        detect.connect("clicked", self.on_detect)
        self.ip_row.add_suffix(detect)
        connection.add(self.ip_row)

        self.codec_row = Adw.ComboRow(title="Codec", model=Gtk.StringList.new([name for name, _ in CODECS]))
        self.codec_row.set_selected(next((i for i, (_, c) in enumerate(CODECS) if c == self.config["codec"]), 0))
        connection.add(self.codec_row)

        self.bitrate_row = Adw.SpinRow.new_with_range(5, 100, 5)
        self.bitrate_row.set_title("Bitrate (Mbit/s)")
        self.bitrate_row.set_value(self.config["bitrate"])
        connection.add(self.bitrate_row)
        page.add(connection)

        # --- Headset
        headset = Adw.PreferencesGroup(title="Headset",
                                       description="Applied to SteamVR's settings when Start launches SteamVR.")
        self.preset_row = Adw.ComboRow(title="Preset", model=Gtk.StringList.new([p["name"] for p in PRESETS]))
        self.preset_row.set_selected(next((i for i, p in enumerate(PRESETS) if p["name"] == self.config["preset"]),
                                          0))
        self.preset_row.connect("notify::selected", lambda *_: self.update_preset_subtitle())
        headset.add(self.preset_row)
        self.height_row = Adw.SpinRow.new_with_range(1.0, 2.3, 0.01)
        self.height_row.set_title("Your height (m)")
        self.height_row.set_digits(2)
        self.height_row.set_value(self.config["height_m"])
        headset.add(self.height_row)
        self.seated_row = Adw.SwitchRow(title="Playing seated")
        self.seated_row.set_active(self.config["seated"])
        headset.add(self.seated_row)
        for row, signal in ((self.height_row, "notify::value"), (self.seated_row, "notify::active")):
            row.connect(signal, lambda *_: self.update_height_subtitle())
        self.update_height_subtitle()
        page.add(headset)
        self.update_preset_subtitle()

        # --- Start / stop
        buttons = Adw.PreferencesGroup()
        box = Gtk.Box(spacing=12, homogeneous=True)
        self.start_button = Gtk.Button(label="Start")
        self.start_button.add_css_class("suggested-action")
        self.start_button.add_css_class("pill")
        self.start_button.connect("clicked", self.on_start)
        self.stop_button = Gtk.Button(label="Stop")
        self.stop_button.add_css_class("destructive-action")
        self.stop_button.add_css_class("pill")
        self.stop_button.connect("clicked", self.on_stop)
        box.append(self.start_button)
        box.append(self.stop_button)
        buttons.add(box)
        page.add(buttons)

        # --- Preview
        preview_group = Adw.PreferencesGroup(title="Headset view",
                                             description="What's being streamed (both eyes), updated 4x a second.")
        self.preview = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, height_request=200, can_shrink=True)
        self.preview.add_css_class("card")
        preview_group.add(self.preview)
        self.screenshot_button = Gtk.Button(label="Save screenshot", halign=Gtk.Align.CENTER, margin_top=12,
                                            tooltip_text=f"Full resolution, both eyes, saved to {SCREENSHOT_DIR}")
        self.screenshot_button.add_css_class("pill")
        self.screenshot_button.connect("clicked", self.on_screenshot)
        preview_group.add(self.screenshot_button)
        self.video_button = Gtk.ToggleButton(label="Video window", halign=Gtk.Align.CENTER, margin_top=6,
                                             tooltip_text="The transmitted video, live, in its own window")
        self.video_button.add_css_class("pill")
        self.video_button.connect("toggled", self.on_video_toggled)
        preview_group.add(self.video_button)
        self.video_window = None
        page.add(preview_group)
        self.preview_mtime = 0

        # --- Log
        log_group = Adw.PreferencesGroup(title="Streamer log")
        self.log_buffer = Gtk.TextBuffer()
        log_view = Gtk.TextView(buffer=self.log_buffer, editable=False, monospace=True, cursor_visible=False,
                                wrap_mode=Gtk.WrapMode.WORD_CHAR)
        log_view.add_css_class("card")
        self.log_scroll = Gtk.ScrolledWindow(child=log_view, min_content_height=220)
        log_group.add(self.log_scroll)
        page.add(log_group)

        if not os.access(CAPTURE, os.X_OK):
            self.log(f"Streamer not built: {CAPTURE}\nBuild it with: cmake --build driver/build")

        self.connect("close-request", self.on_close)
        self.refresh_status()
        GLib.timeout_add_seconds(1, self.refresh_status)
        GLib.timeout_add(250, self.refresh_preview)

    # --- UI helpers

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast(title=text, timeout=4))

    def log(self, text):
        end = self.log_buffer.get_end_iter()
        self.log_buffer.insert(end, text.rstrip("\n") + "\n")
        # Keep the log bounded.
        if self.log_buffer.get_line_count() > 400:
            start = self.log_buffer.get_start_iter()
            cut = self.log_buffer.get_iter_at_line(self.log_buffer.get_line_count() - 400)[1]
            self.log_buffer.delete(start, cut)
        adj = self.log_scroll.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    def preset(self):
        return PRESETS[self.preset_row.get_selected()]

    def head_height(self):
        return eye_height(self.height_row.get_value(), self.seated_row.get_active())

    def update_height_subtitle(self):
        self.seated_row.set_subtitle(f"Eyes {self.head_height():.2f} m above the floor in SteamVR")

    def update_preset_subtitle(self):
        self.preset_row.set_subtitle(self.preset()["description"])

    def streamer_running(self):
        return self.capture is not None and self.capture.poll() is None

    def refresh_status(self):
        vr = steamvr_running()
        self.steamvr_row.set_subtitle("Running" if vr else "Stopped")
        if not self.streamer_running():
            self.streamer_row.set_subtitle("Stopped")
        running = self.streamer_running() or vr
        self.start_button.set_sensitive(not self.streamer_running())
        self.screenshot_button.set_sensitive(self.streamer_running())
        self.stop_button.set_sensitive(running)
        return True

    def refresh_preview(self):
        try:
            mtime = os.stat(PREVIEW_PATH).st_mtime
        except OSError:
            return True
        if mtime != self.preview_mtime and self.streamer_running():
            self.preview_mtime = mtime
            try:
                self.preview.set_paintable(Gdk.Texture.new_from_filename(PREVIEW_PATH))
            except GLib.Error:
                pass  # replaced mid-read; next tick gets it
        return True

    # --- Actions

    def on_video_toggled(self, button):
        if button.get_active() and not self.video_window:
            codec = CODECS[self.codec_row.get_selected()][1]
            self.video_window = VideoWindow(self, codec, self.on_video_closed)
            self.video_window.present()
        elif not button.get_active() and self.video_window:
            self.video_window.close()

    def on_video_closed(self):
        self.video_window = None
        self.video_button.set_active(False)

    def on_screenshot(self, _button):
        if not self.streamer_running():
            self.toast("Start streaming first")
            return
        # lumen-capture saves the next frame at full size on SIGUSR1.
        self.capture.send_signal(signal.SIGUSR1)

    def on_detect(self, _button):
        ip = detect_phone_ip()
        if ip:
            self.ip_row.set_text(ip)
            self.toast(f"Phone found: {ip}")
        else:
            self.toast("No phone on USB, or its Wi-Fi is off")

    def on_start(self, _button):
        ip = self.ip_row.get_text().strip()
        if not re.fullmatch(r"[\w.-]+", ip):
            self.toast("Enter the phone's IP address first")
            return
        if not os.access(CAPTURE, os.X_OK):
            self.toast("Streamer not built (see log)")
            return
        codec = CODECS[self.codec_row.get_selected()][1]
        bitrate = int(self.bitrate_row.get_value())
        self.config.update(phone_ip=ip, codec=codec, bitrate=bitrate, preset=self.preset()["name"],
                           height_m=round(self.height_row.get_value(), 2), seated=self.seated_row.get_active())
        save_config(self.config)

        if steamvr_running():
            self.toast("SteamVR is already running: preset and height apply next time it starts")
        else:
            try:
                apply_preset(self.preset(), self.head_height())
                self.log(f"Applied preset '{self.preset()['name']}' and eye height {self.head_height():.2f} m "
                         "to SteamVR settings")
            except (OSError, ValueError) as e:
                self.log(f"Could not apply preset: {e}")

        args = [CAPTURE, "--codec", codec, "--bitrate", str(bitrate), "--stream", ip, "--preview", PREVIEW_PATH,
                "--screenshot-dir", SCREENSHOT_DIR, "--mirror", str(MIRROR_PORT)]
        os.makedirs(SCREENSHOT_DIR, exist_ok=True)
        self.log("$ " + " ".join(args))
        self.capture = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                        bufsize=1)
        self.stopping = False
        self.streamer_row.set_subtitle("Waiting for SteamVR's driver…")
        threading.Thread(target=self.read_capture_output, args=(self.capture,), daemon=True).start()

        if not steamvr_running():
            subprocess.Popen(["steam", f"steam://rungameid/{STEAMVR_APP_ID}"], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            self.log("Starting SteamVR…")
        self.refresh_status()

    def on_stop(self, _button):
        self.stopping = True
        if self.streamer_running():
            self.capture.terminate()
        if steamvr_running():
            stop_steamvr()
            self.log("Stopping SteamVR…")
        self.stream_row.set_subtitle("—")
        self.preview.set_paintable(None)
        self.refresh_status()

    def on_close(self, _window):
        if self.streamer_running():
            self.capture.terminate()
        if self.video_window:
            self.video_window.close()
        return False

    # --- Streamer output

    def read_capture_output(self, process):
        for line in process.stdout:
            GLib.idle_add(self.on_capture_line, line)
        GLib.idle_add(self.on_capture_exit, process.wait())

    def on_capture_line(self, line):
        if "lsfg-vk" in line:  # noise from an unrelated Vulkan layer
            return False
        if "imported 0x" not in line:
            self.log(line)
        text = line.strip()
        if "driver connected" in text:
            self.streamer_row.set_subtitle("Connected to SteamVR, streaming")
        elif m := re.search(r"screenshot saved: (\S+) \((\d+x\d+)\)", text):
            self.toast(f"Screenshot saved ({m.group(2)}): {os.path.basename(m.group(1))}")
        elif "screenshot failed" in text:
            self.toast("Screenshot failed (see log)")
        elif "driver disconnected" in text:
            self.streamer_row.set_subtitle("Waiting for SteamVR's driver…")
        elif m := re.search(r"encoding frames of (\d+x\d+) (\w+)", text):
            self.stream_resolution = f"{m.group(1)} {m.group(2).upper()}"
        elif m := re.match(r"NVENC encode\s+avg\s+([\d.]+)", text):
            self.encode_ms = float(m.group(1))
        elif m := re.match(r"stream: ([\d.]+) Mbit/s, \d+ keyframes, (\d+) keyframe requests", text):
            parts = [getattr(self, "stream_resolution", ""), f"{float(m.group(1)):.1f} Mbit/s"]
            if hasattr(self, "encode_ms"):
                parts.append(f"encode {self.encode_ms:.1f} ms")
            parts.append(f"{m.group(2)} keyframe requests")
            self.stream_row.set_subtitle(" · ".join(p for p in parts if p))
        return False

    def on_capture_exit(self, code):
        if not self.stopping:
            self.log(f"Streamer exited ({code})")
        self.streamer_row.set_subtitle("Stopped")
        self.refresh_status()
        return False


class LumenApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="dev.lumen.Control")

    def do_activate(self):
        window = self.get_active_window() or LumenWindow(self)
        window.present()


if __name__ == "__main__":
    sys.exit(LumenApp().run(sys.argv))
