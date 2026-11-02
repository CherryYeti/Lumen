# Lumen

Play SteamVR games on Linux with a phone as the headset: in a Samsung Gear VR,
or in any phone viewer.

Lumen is a SteamVR driver that presents a virtual headset. Every frame
SteamVR renders is encoded on the PC's NVIDIA GPU and streamed over Wi-Fi to
the phone, which shows it and sends head rotation back. A Bluetooth gamepad
paired with the phone works as a pair of Oculus Touch controllers, and the
PC's audio plays on the phone.

- **Gear VR app**: uses Oculus's VrApi runtime for lens correction, head
  tracking and reprojection (TimeWarp), so the view stays steady even when a
  frame arrives late. Look at something and tap the touchpad to click.
- **Phone app**: for a plain phone viewer (Cardboard-style); shows the
  stream side by side and tracks rotation with the phone's sensors.
- **Desktop app**: starts and stops everything, applies headset settings,
  and shows a live view of the stream.

Tracking is rotation only (3DoF): the head turns but doesn't move through
space, and the gamepad's hands follow your view rather than being tracked.

## Requirements

**PC**

- Linux with SteamVR (tested with 2.17 and 2.18)
- An NVIDIA GPU with NVENC, and its proprietary driver
- Build tools: CMake 3.16+, a C++17 compiler
- Libraries: Vulkan, CUDA toolkit (driver API and NVRTC), FFmpeg's
  libavcodec/libavutil built with NVENC, zlib, libpulse (PulseAudio or
  PipeWire's PulseAudio server)
- Desktop app: Python 3 with PyGObject, GTK 4, libadwaita 1.4+. Optional:
  GStreamer with `gtk4paintablesink` and the `nvcodec` plugin, for the video
  window. Optional: `adb`, to read the phone's IP over USB.

**Phone**

- Android 8.0 (API 26) or newer; tested on a Galaxy S7
- 5 GHz Wi-Fi on the same network as the PC (wired PC recommended)
- For the Gear VR app: a Samsung Gear VR, and Oculus Mobile SDK **1.26.0**
  (the last version with Gear VR support) to build it. The SDK is under the
  Oculus SDK License and can't be redistributed, so it isn't included. Download
  `ovr_sdk_mobile_1.26.0.zip` from
  <https://developers.meta.com/vr/downloads/package/oculus-mobile-sdk/1.26.0/>
  (sign in with a Meta account and accept the license there).

## Build

**Driver and streamer**

```bash
cmake -S driver -B driver/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

```bash
cmake --build driver/build
```

This builds the SteamVR driver folder (`driver/build/lumen/`) and the streamer
(`driver/build/lumen-capture`).

**Android apps**

Create `android/local.properties` pointing at your Android SDK and, for the
Gear VR app, the unpacked Oculus SDK:

```properties
sdk.dir=/home/you/Android/Sdk
ovr.sdk.dir=/home/you/SDKs/ovr_sdk_mobile_1.26.0
```

```bash
(cd android && ./gradlew assembleDebug)
```

Gradle needs JDK 17 or newer (set `JAVA_HOME` if your default is older). The
first native build downloads the NDK and CMake into the SDK. Install with:

```bash
adb install -r android/gearvr/build/outputs/apk/debug/gearvr-debug.apk
```

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

**Release APKs**

Release builds are signed with your own key. Create it once and keep it (and
its passwords) safe: updates must be signed with the same key to install over
an earlier version.

```bash
keytool -genkeypair -keystore ~/lumen-release.jks -alias lumen -keyalg RSA -keysize 4096 -validity 10000
```

Then create `android/keystore.properties` (ignored by git):

```properties
storeFile=/home/you/lumen-release.jks
storePassword=...
keyAlias=lumen
keyPassword=...
```

```bash
(cd android && ./gradlew assembleRelease)
```

The APKs are `android/app/build/outputs/apk/release/app-release.apk` and
`android/gearvr/build/outputs/apk/release/gearvr-release.apk`. Without
`keystore.properties` they come out unsigned (`*-release-unsigned.apk`).

Both APKs carry Lumen's license in `assets/licenses/`; the Gear VR APK also
carries the Oculus SDK License, because it includes Meta's `libvrapi.so`. When
publishing the Gear VR APK, meet the Oculus SDK License's redistribution
terms (sections 2.1 to 2.4):

- Include its copyright notice: "Copyright (c) Facebook Technologies, LLC and
  its affiliates. All rights reserved."
- Give recipients a copy of the Oculus SDK License: attach
  `ovr_sdk_mobile_1.26.0/LICENSE.txt` to the release next to the APK (it is
  also inside the APK).
- The SDK code may only be used with Oculus-approved hardware, which Gear VR
  is; say in the release that the Gear VR APK is for Gear VR only.

Lumen's own license (GPLv3) requires the source to be available alongside the
APKs; a release page on the repository, with its source archive, covers that.

## Setup

1. Register the driver with SteamVR (undo with `--remove`):

   ```bash
   driver/register.sh
   ```

2. Start the desktop app, and optionally add it to your app launcher:

   ```bash
   /usr/bin/python3 gui/lumen_gui.py
   ```

   ```bash
   gui/install-desktop-entry.sh
   ```

3. In the app, enter the phone's Wi-Fi IP (or plug the phone in and use
   **Detect over USB**), pick a codec (HEVC if the phone decodes it) and a
   bitrate (30–50 Mbit/s), and set your height.

4. For the Gear VR app, the phone's Gear VR service has to allow sideloaded
   apps to use the performance API; Lumen isn't from the Oculus store, so
   without that its VR mode doesn't start. Gear VR developer mode isn't
   needed, and neither is an Oculus signature file.

When the app launches SteamVR, it also writes the settings Lumen needs into
`~/.local/share/Steam/config/steamvr.vrsettings`: Lumen as the forced driver
(so it wins over other headset drivers such as ALVR), no standby when the
headset is still (otherwise SteamVR drops to 10 fps), the chosen preset and
your eye height. SteamVR rewrites that file when it exits, so these are only
applied when SteamVR isn't already running. The original file is backed up
once as `steamvr.vrsettings.before-lumen-gui`.

## Use

1. Press **Start** in the desktop app. It starts the streamer, then SteamVR.
2. **Gear VR**: open _Lumen Gear VR_ and dock the phone (it waits for the
   dock). There's no setup screen: it shows a status panel until video
   arrives, and sends head tracking back to wherever the video comes from.
3. **Phone viewer**: open _Lumen_, enter the PC's IP, tap Start and put the
   phone in the viewer, top edge to the left.

The first time a phone connects, it tells the driver its resolution and field
of view. If that differs from what SteamVR is running, the driver saves it and
asks SteamVR to restart.

**Stop** stops the streamer and SteamVR.

### Desktop app

- **Preset**: _Automatic (from the phone)_ lets the phone choose the
  resolution. The fixed presets are _Gear VR_ (1536×1536 per eye, 92°),
  _Gear VR (light)_ (1152×1152, 100°; less decode and Wi-Fi load) and
  _Phone screen (Galaxy S7)_ (2560×1440 total, 95°).
- **Your height** / **Playing seated**: sets how high your eyes are above
  SteamVR's floor (standing: height − 11 cm; seated: 69% of your height).
- **Headset view**: a small preview, updated 4 times a second.
- **Video window**: the stream exactly as the phone receives it, live, in its
  own resizable window (decoded on the GPU while open).
- **Save screenshot**: saves the next frame at full resolution to
  `~/Pictures/Lumen/`.
- **Status and log**: SteamVR and streamer state, resolution, bitrate and
  encode time.

SteamVR's own "Display VR View" stays black with Lumen; use the preview or the
video window instead.

### Gear VR controls

| Action                      | What it does                                                             |
| --------------------------- | ------------------------------------------------------------------------ |
| Tap the touchpad            | Click where you look                                                     |
| Tap, then touch and hold    | Click-and-hold: drag by turning your head, release by lifting the finger |
| Swipe                       | Scroll                                                                   |
| Hold a finger still for 1 s | Recenter                                                                 |
| Back                        | Open/close the SteamVR dashboard                                         |
| Hold back                   | Exit                                                                     |

Clicking works through a virtual Vive wand that follows your head, so
SteamVR's own bindings apply. With every preset except _Phone screen_ its
laser is hidden (`"dashboard": { "laserRayWidthScale": 0 }`), because a laser
starting between the eyes looks doubled; this hides real controllers' lasers
too.

### Phone app controls

Tap to recenter, long-press to show or hide the stats overlay, back to return
to the setup screen.

### Gamepad

A Bluetooth gamepad paired with the phone (Xbox, or 8BitDo in X-input or
Android mode) becomes two Oculus Touch controllers. Most SteamVR games ship
Touch bindings, so no setup is needed. Supported by the Gear VR app.

| Pad                 | Touch                              |
| ------------------- | ---------------------------------- |
| Left stick / click  | Left thumbstick / click            |
| Right stick / click | Right thumbstick / click           |
| LT / RT             | Left / right trigger               |
| LB / RB             | Left / right grip                  |
| X / Y               | Left X / Y                         |
| A / B               | Right A / B                        |
| Start or Guide      | Menu (opens the SteamVR dashboard) |

The hands sit in front of your body and point where you look. **Tap
View/Select** to cycle _off → move hands → rotate hands_; while adjusting,
each side's controls move that hand instead of reaching the game:

|             | Move                   | Rotate             |
| ----------- | ---------------------- | ------------------ |
| Stick       | Sideways / up and down | Turn / tilt        |
| Trigger     | Push away              | Roll one way       |
| Grip        | Pull closer            | Roll the other way |
| Stick click | Reset position         | Reset rotation     |

In the SteamVR dashboard the gaze pointer takes over: RT or A clicks, the
right stick scrolls, Start/Guide closes the dashboard. Games that need real
hand movement (grabbing, swinging) won't play well.

When a game vibrates a controller, the pad rumbles if Android can drive its
motors; many pads' Bluetooth drivers don't expose them.

### Audio

While streaming, `lumen-capture` captures the PC's default audio output and
sends it to the phone as uncompressed PCM (about 1.5 Mbit/s). The Gear VR app
plays it through the phone's headphone jack or Bluetooth headphones; the PC
keeps playing it too. Bluetooth headphones add noticeable delay; wired ones
don't.

## Settings

Driver defaults are in `driver/lumen/resources/settings/default.vrsettings`
(section `driver_lumen`); override them in `steamvr.vrsettings` under the same
section, with SteamVR stopped.

| Key                            | Default     | Meaning                                                                 |
| ------------------------------ | ----------- | ----------------------------------------------------------------------- |
| `renderWidth` / `renderHeight` | 1200 / 1350 | Per-eye render size SteamVR is asked for; each eye's image is 1.5× this |
| `fovDegrees`                   | 95          | Horizontal field of view per eye                                        |
| `headHeightMeters`             | 1.6         | Eye height above the floor                                              |
| `ipdMeters`                    | 0.063       | Distance between the eyes                                               |
| `displayFrequency`             | 60          | Frame rate                                                              |
| `posePort`                     | 9944        | UDP port for head tracking                                              |
| `gazePointer`                  | true        | Add the head-following pointer used for clicking                        |

The phone apps set `renderWidth`, `renderHeight` and `fovDegrees`
automatically; the desktop app sets `headHeightMeters`.

`lumen-capture` options (the desktop app passes these):

```
lumen-capture --stream HOST[:PORT] [--codec h264|hevc] [--bitrate MBPS]
              [--mirror PORT] [--preview FILE] [--screenshot-dir DIR]
```

Each streamed eye is capped at the headset's own eye size, which the driver
passes along.

## How it works

```
SteamVR compositor ──textures──▶ driver (in vrserver) ──fds over a Unix socket──▶ lumen-capture
                                     ▲                                               │
                                     │ head pose, buttons (UDP 9944)                 │ video (UDP 9945)
                                     │                                               │ audio (UDP 9946)
                                     └──────────────────── phone ◀───────────────────┘
```

- **Driver** (`driver/src`): an HMD using driver direct mode
  (`IVRDriverDirectModeComponent`). The compositor renders each finished frame
  into shared Vulkan textures the driver allocates, and paces itself to the
  driver's 60 Hz. The driver receives head poses and controller input from the
  phone, and adds the gaze pointer and the gamepad's Touch controllers.
- **Getting the pixels**: the textures live in the compositor process. The
  driver obtains each one's memory as a file descriptor through SteamVR's
  internal IPC (undocumented; see `driver/src/shared_fd.cpp`) and passes it
  to `lumen-capture`. The import has to happen outside vrserver: holding the
  imported memory inside it makes the compositor exit.
- **Streamer** (`driver/helper`): imports the textures into CUDA, crops away
  any unused part of each eye's texture and scales it on the GPU, and encodes
  with NVENC (lowest-latency settings, no B-frames, constant bitrate). Frames
  never leave the GPU until they're encoded.
- **Video packets** (`driver/src/video_stream.h`): each frame is split into
  UDP datagrams carrying the head pose it was rendered with and the field of
  view, so the Gear VR app can reproject it to the current head pose. A frame
  missing a packet is dropped, and the phone asks for a keyframe to recover.
- **Head tracking** (`driver/src/pose_receiver.h`): the phone sends its
  orientation about 200 times a second, predicted ahead by the measured
  round trip so things locked to the view (the dashboard cursor) keep up.
  The same port carries buttons, the gamepad, battery level, display size and
  log lines; rumble goes back the other way.
- **Phone side** (`android/`): `stream/` holds the shared receiver, decoder
  and audio player; `gearvr/` is the VrApi client (native rendering in
  `lumen_gearvr.c`), `app/` the plain phone client.

The Gear VR app also sends its stats to the PC once a second; the driver
writes them to SteamVR's log as `lumen: phone: ...`, which is the easiest way
to see what the phone is doing while it's in the headset:

```bash
grep "lumen: phone" ~/.local/share/Steam/logs/vrserver.txt | tail
```

## Troubleshooting

- **The streamer logs "frame uses textures ... that weren't imported" for
  every frame**, and SteamVR's log has `lumen: no fd for ...`: a SteamVR
  update changed its internal IPC again. `driver/src/shared_fd.cpp` handles
  the variants known so far.
- **SteamVR doesn't pick Lumen**: another headset driver (such as ALVR) is
  winning. Start SteamVR from the desktop app once so it writes
  `"forcedDriver": "lumen"`.
- **The driver stopped loading after a crash**: SteamVR may have set
  `"blocked_by_safe_mode": true` for it in `steamvr.vrsettings`. Remove that
  (or unblock the driver in SteamVR's settings).
- **The image drops to 10 fps when the headset is still**:
  `"power": { "pauseCompositorOnStandby": false }` is missing from
  `steamvr.vrsettings`.
- **Choppy video**: lower the bitrate or use a lighter preset; keep the
  phone on 5 GHz Wi-Fi. Bluetooth audio on the phone can also slow its Wi-Fi.

## Limitations

- Rotation-only tracking.
- NVIDIA GPUs only (NVENC and CUDA). [I don't have an AMD card]
- While a game runs, SteamVR overlays (the dashboard over a game,
  notifications) aren't shown in the headset: only the game's own image is
  streamed.
- The texture handover relies on undocumented SteamVR internals, which
  updates can change.

## Third-party code

`driver/third_party/openvr/openvr_driver.h` is from Valve's OpenVR SDK (BSD
3-Clause, license alongside it). The Oculus Mobile SDK is not included; see
[Requirements](#requirements) for where to get it. Gear VR APK builds include
the SDK's `libvrapi.so`: Copyright (c) Facebook Technologies, LLC and its
affiliates. All rights reserved. It is used under the Oculus SDK License,
which is included in that APK.

## License

Lumen is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).

Additional permission under GNU GPL version 3 section 7: if you modify this
program, or any covered work, by linking or combining it with the Oculus
Mobile SDK (VrApi) or the SteamVR runtime, or a modified version of either,
containing parts covered by the terms of their respective licenses, the
licensors of this program grant you additional permission to convey the
resulting work.
