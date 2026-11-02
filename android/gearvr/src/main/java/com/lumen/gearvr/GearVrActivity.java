package com.lumen.gearvr;

import android.app.Activity;
import android.graphics.Bitmap;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

import com.lumen.stream.AudioPlayer;
import com.lumen.stream.Frame;
import com.lumen.stream.VideoDecoder;
import com.lumen.stream.VideoReceiver;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Lumen in a Gear VR. VrApi (native, lumen_gearvr.c) shows the stream through
 * the Oculus compositor, which handles lens correction and reprojection; this
 * class wires up the video and sends head tracking.
 *
 * <p>No setup screen: on the PC run {@code lumen-capture --stream PHONE_IP}.
 * Head poses go back to whichever address the video comes from.
 *
 * <p>In the headset (driving the driver's head-following pointer): touchpad tap
 * to click, tap then touch again and hold to click-and-hold (drag by turning
 * your head, release by lifting the finger), swipe to scroll, hold a finger
 * still for a second to recenter; the back button opens/closes the SteamVR
 * dashboard, hold back to exit.
 */
public class GearVrActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("vrapi");
        System.loadLibrary("lumen_gearvr");
    }

    private static final String TAG = "LumenGearVR";
    private static final int POSE_PORT = 9944;
    private static final int BUTTON_TRIGGER = 1;
    private static final int BUTTON_TRACKPAD_TOUCH = 2;
    private static final int BUTTON_SYSTEM = 4;
    /** Holding back at least this long exits instead of toggling the dashboard. */
    private static final long BACK_HOLD_EXIT_MS = 600;
    /** How long a tap holds the trigger: spans many ~5 ms pose packets. */
    private static final long CLICK_MS = 80;
    /** Touchpad travel (px) that turns a touch into a swipe; taps wobble up to ~45 px. */
    private static final float SWIPE_PX = 40;
    /** Swipe distance (px) that moves the virtual trackpad from centre to edge (the pad is ~400 px). */
    private static final float PAD_RANGE_PX = 400;
    /** Holding still this long recenters. */
    private static final long LONG_PRESS_MS = 1000;
    /** A touch this soon after a tap holds the trigger down until lifted (drag). */
    private static final long TAP_HOLD_MS = 300;

    private static native void nativeOnCreate(Activity activity);
    private static native void nativeOnResume();
    private static native void nativeOnPause();
    private static native void nativeOnSurfaceCreated(Surface surface);
    private static native void nativeOnSurfaceDestroyed();
    private static native void nativeOnDestroy();
    private static native void nativeSetFramePose(long presentationTimeUs, float w, float x, float y, float z,
                                                  float tanX, float tanY, float uMin, float vMin, float uMax,
                                                  float vMax);
    private static native void nativeSetVideoSize(int width, int height);
    private static native boolean nativeGetHeadPose(float[] out);
    private static native void nativeRecenter();
    private static native boolean nativeGetDisplayRequest(float[] out);
    private static native void nativeSetStatus(Bitmap bitmap);

    private final Handler main = new Handler(Looper.getMainLooper());
    private VideoReceiver receiver;
    private VideoDecoder decoder;
    private AudioPlayer audio; // the PC's sound, out of the phone's headphone jack
    private Thread poseThread;
    private volatile boolean running = true;
    private volatile long posesSent;
    /** The latest stats line, for the pose thread to send to the PC (no USB in the headset). */
    private volatile String statsForPc;
    private volatile int buttons;
    private volatile float padX, padY; // virtual trackpad position, -1..1, +y up
    private float touchDownX, touchDownY, swipeStartX, swipeStartY;
    private boolean swiping, longPressed;
    private boolean holding; // trigger held by tap-then-hold
    private long lastTapUpMs;
    private final Runnable releaseClick = () -> buttons &= ~BUTTON_TRIGGER;
    private final Runnable longPress = () -> {
        longPressed = true;
        nativeRecenter();
        Log.i(TAG, "touchpad hold: recenter");
    };
    private WifiManager.WifiLock wifiLock;
    private long lastFrames, lastDecoded, lastPoses, lastGcs, lastAudio, lastStatsNs;
    private final StatusPanel statusPanel = new StatusPanel();
    private Gamepad gamepad;
    private BatteryManager batteryManager;

    /** Phone battery, 0..1. */
    private float batteryLevel() {
        return Math.max(0, Math.min(100, batteryManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)))
            / 100f;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);

        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            int mode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                                        : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wifi.createWifiLock(mode, "lumen-gearvr");
            wifiLock.acquire();
        }
        gamepad = new Gamepad(this, main);
        batteryManager = (BatteryManager) getSystemService(Context.BATTERY_SERVICE);
        nativeOnCreate(this);
        updateStatus(0, 0);
    }

    /** Called by the native render thread once VrApi's video surface exists. */
    @SuppressWarnings("unused")
    void onVideoSurface(Surface surface) {
        main.post(() -> startVideo(surface));
    }

    private void startVideo(Surface surface) {
        if (!running || decoder != null)
            return;
        decoder = new VideoDecoder(surface, new VideoDecoder.Listener() {
            @Override
            public void onVideoSize(int width, int height) {
                Log.i(TAG, "video " + width + "x" + height);
                nativeSetVideoSize(width, height);
            }

            @Override
            public void onError(String message) {
                Log.e(TAG, message);
            }

            @Override
            public void onFrameRendered(Frame frame) {
                // The decoder used receivedUs as presentationTimeUs; the native
                // side matches it to the image it latches.
                float[] p = frame.renderPose, b = frame.bounds;
                nativeSetFramePose(frame.receivedUs, p[0], p[1], p[2], p[3], frame.tanHalfFov[0],
                                   frame.tanHalfFov[1], b[0], b[1], b[2], b[3]);
            }
        });
        receiver = new VideoReceiver(decoder);
        decoder.setReceiver(receiver);
        decoder.start();
        receiver.start();
        audio = new AudioPlayer();
        audio.start();
        poseThread = new Thread(this::sendPoses, "LumenPoses");
        poseThread.start();
        lastStatsNs = System.nanoTime();
        main.postDelayed(this::logStats, 1000);
        Log.i(TAG, "waiting for video on UDP " + VideoReceiver.PORT);
    }

    /**
     * Sends the Gear VR head orientation and touchpad state to the PC at ~250 Hz
     * ("LMN4", 64 bytes: the phone app's "LMN1" plus a uint32 button mask, the
     * virtual trackpad's x, y, and the gamepad's buttons, sticks and triggers).
     * See driver/src/pose_receiver.h.
     */
    private void sendPoses() {
        float[] q = new float[4];
        ByteBuffer packet = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN);
        DatagramPacket datagram = new DatagramPacket(packet.array(), 64); // reused: no garbage per pose
        ByteBuffer battery = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer display = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        float[] want = new float[3];
        long lastBatteryNs = 0;
        int sequence = 0;
        try (DatagramSocket socket = new DatagramSocket()) {
            // The PC sends rumble back to this socket (poses' source address).
            Thread haptics = new Thread(() -> receiveHaptics(socket), "LumenHaptics");
            haptics.start();
            while (running) {
                InetAddress pc = receiver.senderAddress();
                if (pc != null && nativeGetHeadPose(q)) {
                    packet.clear();
                    packet.put((byte) 'L').put((byte) 'M').put((byte) 'N').put((byte) '4');
                    packet.putInt(sequence++);
                    packet.putFloat(q[0]).putFloat(q[1]).putFloat(q[2]).putFloat(q[3]);
                    packet.putInt(buttons);
                    packet.putFloat(padX).putFloat(padY);
                    packet.putInt(gamepad.buttons());
                    packet.putFloat(gamepad.lx).putFloat(gamepad.ly).putFloat(gamepad.rx).putFloat(gamepad.ry);
                    packet.putFloat(gamepad.lt).putFloat(gamepad.rt);
                    datagram.setAddress(pc);
                    datagram.setPort(POSE_PORT);
                    socket.send(datagram);
                    posesSent++;
                }
                // Once a second: the stats line, which the driver writes to
                // SteamVR's log ("LML1" + UTF-8 text), since adb over USB
                // isn't possible with the phone in the headset.
                String stats = statsForPc;
                if (pc != null && stats != null) {
                    statsForPc = null;
                    byte[] text = ("LML1" + stats).getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    socket.send(new DatagramPacket(text, Math.min(text.length, 1400), pc, POSE_PORT));
                }
                // Every 5 s: the phone's battery, which SteamVR shows as the
                // headset's ("LMB1": float level 0..1, uint32 bit 0 = charging),
                // and what SteamVR should render ("LMD1": eye width, height,
                // horizontal FOV), which the driver adopts.
                long now = System.nanoTime();
                if (pc != null && now - lastBatteryNs > 5_000_000_000L && nativeGetDisplayRequest(want)) {
                    display.clear();
                    display.put((byte) 'L').put((byte) 'M').put((byte) 'D').put((byte) '1');
                    display.putInt(Math.round(want[0])).putInt(Math.round(want[1])).putFloat(want[2]);
                    socket.send(new DatagramPacket(display.array(), 16, pc, POSE_PORT));
                }
                if (pc != null && now - lastBatteryNs > 5_000_000_000L) {
                    lastBatteryNs = now;
                    battery.clear();
                    battery.put((byte) 'L').put((byte) 'M').put((byte) 'B').put((byte) '1');
                    battery.putFloat(batteryLevel());
                    battery.putInt(batteryManager.isCharging() ? 1 : 0);
                    socket.send(new DatagramPacket(battery.array(), 12, pc, POSE_PORT));
                }
                Thread.sleep(4);
            }
        } catch (InterruptedException e) {
            // stopping
        } catch (Exception e) {
            Log.e(TAG, "pose sender failed", e);
        }
    }

    /**
     * Rumble from the PC ("LMH1", 20 bytes: uint32 hand, float duration s,
     * frequency Hz, amplitude 0..1; see driver/src/pose_receiver.h). Runs until
     * the pose socket closes.
     */
    private void receiveHaptics(DatagramSocket socket) {
        byte[] buffer = new byte[64];
        DatagramPacket datagram = new DatagramPacket(buffer, buffer.length);
        ByteBuffer packet = ByteBuffer.wrap(buffer).order(ByteOrder.LITTLE_ENDIAN);
        while (running && !socket.isClosed()) {
            try {
                datagram.setLength(buffer.length);
                socket.receive(datagram);
            } catch (Exception e) {
                return; // socket closed
            }
            if (datagram.getLength() != 20 || buffer[0] != 'L' || buffer[1] != 'M' || buffer[2] != 'H'
                || buffer[3] != '1')
                continue;
            float duration = packet.getFloat(8), amplitude = packet.getFloat(16);
            if (Float.isFinite(duration) && Float.isFinite(amplitude))
                gamepad.vibrate(Math.min(1f, amplitude), duration);
        }
    }

    /** One stats line per second in logcat (tag LumenStats), like the phone app. */
    private void logStats() {
        if (!running)
            return;
        main.postDelayed(this::logStats, 1000);
        long now = System.nanoTime();
        double seconds = (now - lastStatsNs) / 1e9;
        lastStatsNs = now;
        long frames = receiver.frames.get(), decoded = decoder.decoded.get(), poses = posesSent;
        // Garbage collections this second, for diagnosing stalls.
        long gcs = gcCount();
        long latencyCount = decoder.latencyCount.getAndSet(0), latencySum = decoder.latencySumUs.getAndSet(0);
        String stats = String.format(Locale.US,
            "%s recv %.1f fps decoded %.1f fps decode %.1f ms lost %d discarded %d decoder drops %d "
                + "keyframe requests %d poses %.0f/s audio %.0f packets/s lost %d underruns %d skipped %d "
                + "reopened %d to %s gc %d",
            decoder.codecName(), (frames - lastFrames) / seconds, (decoded - lastDecoded) / seconds,
            latencyCount > 0 ? latencySum / 1000.0 / latencyCount : 0.0, receiver.lostFrames.get(),
            receiver.discardedFrames.get(), decoder.dropped.get(), receiver.keyframeRequests.get(),
            (poses - lastPoses) / seconds, (audio.packets.get() - lastAudio) / seconds, audio.lostPackets.get(),
            audio.underruns.get(), audio.skipped.get(), audio.restarts.get(), audio.output, gcs - lastGcs);
        Log.i("LumenStats", stats);
        statsForPc = stats;
        lastGcs = gcs;
        lastAudio = audio.packets.get();
        updateStatus((decoded - lastDecoded) / seconds, (poses - lastPoses) / seconds);
        lastFrames = frames;
        lastDecoded = decoded;
        lastPoses = poses;
    }

    private static long gcCount() {
        try {
            return Long.parseLong(android.os.Debug.getRuntimeStat("art.gc.gc-count"));
        } catch (RuntimeException e) {
            return 0;
        }
    }

    /** Refreshes the in-headset status panel (shown while there's no video). */
    private void updateStatus(double fps, double posesPerSecond) {
        List<String> lines = new ArrayList<>();
        InetAddress pc = receiver != null ? receiver.senderAddress() : null;
        String phone = StatusPanel.localIpv4();
        if (receiver == null) {
            lines.add("Starting...");
        } else if (pc == null) {
            lines.add("Running. Waiting for video on UDP " + VideoReceiver.PORT);
            lines.add("On the PC: Lumen app > Start (phone IP " + (phone != null ? phone : "?") + ")");
        } else if (fps < 1) {
            lines.add("PC found at " + pc.getHostAddress() + ", waiting for frames");
            lines.add("Is SteamVR running? A keyframe can take a few seconds.");
        } else {
            lines.add(String.format(Locale.US, "Receiving video: %.0f fps", fps));
        }
        lines.add("This phone: " + (phone != null ? phone : "no Wi-Fi address") + String.format(Locale.US,
            "   Battery %.0f%%%s", batteryLevel() * 100, batteryManager.isCharging() ? " (charging)" : ""));
        lines.add(String.format(Locale.US, "Head tracking: %.0f poses/s%s", posesPerSecond,
                                pc == null ? " (starts once video arrives)" : ""));
        lines.add(gamepad.isConnected() ? "Gamepad: " + gamepad.name() + " (as Touch controllers)"
                                        : "Gamepad: none (pair one over Bluetooth for Touch controls)");
        lines.add("Tap: click   Tap, then hold: drag   Swipe: scroll");
        lines.add("Back: dashboard   Hold back: exit");
        nativeSetStatus(statusPanel.draw(lines));
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return gamepad.onMotion(event) || super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        float x = event.getX(), y = event.getY();
        switch (event.getActionMasked()) {
        case MotionEvent.ACTION_DOWN:
            if (event.getEventTime() - lastTapUpMs < TAP_HOLD_MS) {
                // Tap, then touch and hold: press the trigger until the finger
                // lifts. No trackpad touch, so moving the finger doesn't scroll.
                holding = true;
                main.removeCallbacks(releaseClick);
                buttons |= BUTTON_TRIGGER;
                Log.i(TAG, "touchpad tap-and-hold: trigger held");
                break;
            }
            touchDownX = x;
            touchDownY = y;
            swiping = false;
            longPressed = false;
            // Centre first, then touch: the driver applies position before touch,
            // so a new touch never starts with a jump.
            padX = 0;
            padY = 0;
            buttons |= BUTTON_TRACKPAD_TOUCH;
            main.postDelayed(longPress, LONG_PRESS_MS);
            break;
        case MotionEvent.ACTION_MOVE:
            if (holding)
                break;
            if (!swiping && !longPressed && Math.hypot(x - touchDownX, y - touchDownY) > SWIPE_PX) {
                // Measure from here, so the tap wobble below SWIPE_PX never scrolls.
                swiping = true;
                swipeStartX = x;
                swipeStartY = y;
                main.removeCallbacks(longPress);
            }
            if (swiping) {
                padX = clampPad((x - swipeStartX) / PAD_RANGE_PX);
                padY = clampPad(-(y - swipeStartY) / PAD_RANGE_PX);
            }
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_CANCEL:
            if (holding) {
                holding = false;
                lastTapUpMs = 0; // a quick touch after a drag is a new tap, not another hold
                buttons &= ~BUTTON_TRIGGER;
                Log.i(TAG, "touchpad hold released");
                break;
            }
            main.removeCallbacks(longPress);
            // Release touch but keep the position: moving it back to the centre
            // while still touched would scroll back.
            buttons &= ~BUTTON_TRACKPAD_TOUCH;
            float dx = x - touchDownX, dy = y - touchDownY;
            if (swiping) {
                Log.i(TAG, String.format(Locale.US, "touchpad swipe: scroll (moved %.0f, %.0f px)", dx, dy));
            } else if (!longPressed && event.getActionMasked() == MotionEvent.ACTION_UP) {
                Log.i(TAG, String.format(Locale.US, "touchpad tap: click (moved %.0f, %.0f px)", dx, dy));
                // Press now, release shortly after: SteamVR sees a trigger click.
                buttons |= BUTTON_TRIGGER;
                main.removeCallbacks(releaseClick);
                main.postDelayed(releaseClick, CLICK_MS);
                lastTapUpMs = event.getEventTime();
            }
            break;
        }
        return true;
    }

    private static float clampPad(float v) {
        return Math.max(-1f, Math.min(1f, v));
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // Gamepad buttons go to the PC (including a pad's own Back/View key);
        // only the Gear VR's back button toggles the dashboard below.
        if (Gamepad.fromGamepad(event) && gamepad.onKey(event))
            return true;
        if (event.getKeyCode() != KeyEvent.KEYCODE_BACK)
            return super.dispatchKeyEvent(event);
        if (event.getAction() == KeyEvent.ACTION_UP) {
            long held = event.getEventTime() - event.getDownTime();
            if (held >= BACK_HOLD_EXIT_MS) {
                Log.i(TAG, "back held: exit");
                finish();
            } else {
                // SteamVR's system button: opens/closes the dashboard (the only
                // way back after clicking empty space dismissed it).
                Log.i(TAG, "back: toggle SteamVR dashboard");
                buttons |= BUTTON_SYSTEM;
                main.postDelayed(() -> buttons &= ~BUTTON_SYSTEM, CLICK_MS);
            }
        }
        return true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        nativeOnResume();
    }

    @Override
    protected void onPause() {
        nativeOnPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        running = false;
        main.removeCallbacksAndMessages(null);
        if (poseThread != null)
            poseThread.interrupt();
        if (receiver != null)
            receiver.shutdown();
        if (audio != null)
            audio.shutdown();
        if (decoder != null) {
            decoder.shutdown();
            try {
                decoder.join(500);
            } catch (InterruptedException ignored) {
            }
        }
        gamepad.close();
        nativeOnDestroy();
        if (wifiLock != null)
            wifiLock.release();
        super.onDestroy();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeOnSurfaceCreated(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeOnSurfaceDestroyed();
    }
}
