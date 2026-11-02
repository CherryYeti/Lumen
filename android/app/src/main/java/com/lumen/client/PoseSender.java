package com.lumen.client;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Sends head orientation to the Lumen SteamVR driver: UDP port 9944, 24-byte
 * packets ("LMN1", uint32 sequence, float w x y z), little-endian. Format in
 * driver/src/pose_receiver.h.
 *
 * <p>Source: TYPE_GAME_ROTATION_VECTOR (gyro + accelerometer, no compass, so
 * no magnetic jumps; yaw drifts slowly instead). Rotation-only.
 *
 * <p>Frames involved:
 * <ul>
 *   <li>Android world: X east-ish, Y north-ish, Z up. Device: X right, Y up,
 *       Z out of the screen, in natural portrait.
 *   <li>The phone sits in the viewer in landscape (top edge to the left), so
 *       head right = -device Y, head up = device X, head back = device Z. That
 *       is a rotation of -90° about device Z.
 *   <li>OpenVR world: Y up, -Z forward. From Android world that's -90° about X.
 * </ul>
 * So q_openvr = qx(-90°) * q_android * qz(-90°), then the yaw is re-zeroed so
 * "recenter" points the user down OpenVR's -Z.
 *
 * <p>Every 5 s it also tells the driver what to render ("LMD1": per-eye size
 * = half the screen in landscape, and the FOV), so SteamVR's resolution
 * follows the phone's screen without hand-edited settings.
 */
final class PoseSender implements SensorEventListener {
    static final int PORT = 9944;
    private static final String TAG = "LumenPose";
    private static final float S = (float) Math.sqrt(0.5);
    private static final float[] WORLD_ANDROID_TO_OPENVR = {S, -S, 0, 0}; // qx(-90°)
    private static final float[] DEVICE_TO_HEAD = {S, 0, 0, -S};          // qz(-90°)

    final AtomicLong sent = new AtomicLong();

    private final SensorManager sensorManager;
    private final Sensor sensor;
    private final String host;
    private HandlerThread thread;
    private DatagramSocket socket;
    private InetAddress address;
    private int sequence;
    private final ByteBuffer packet = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
    private final float[] raw = new float[4];
    // Yaw correction applied on the left; identity until the first recenter.
    private float[] recenter = {1, 0, 0, 0};
    private volatile boolean recenterRequested = true; // recenter on the first sample

    /** Horizontal per-eye FOV asked of SteamVR; a plain phone viewer has no lens data to go on. */
    private static final float FOV_DEGREES = 95f;
    private final ByteBuffer display = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
    private long lastDisplayNs;

    PoseSender(Context context, String host) {
        // Landscape screen, one eye per half.
        android.graphics.Point size = new android.graphics.Point();
        ((android.view.WindowManager) context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay()
            .getRealSize(size);
        display.put((byte) 'L').put((byte) 'M').put((byte) 'D').put((byte) '1');
        display.putInt(Math.max(size.x, size.y) / 2).putInt(Math.min(size.x, size.y)).putFloat(FOV_DEGREES);
        this.sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.sensor = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
        this.host = host;
    }

    boolean hasSensor() {
        return sensor != null;
    }

    void start() {
        if (sensor == null)
            return;
        thread = new HandlerThread("LumenPose");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        handler.post(() -> {
            try {
                address = InetAddress.getByName(host);
                socket = new DatagramSocket();
            } catch (Exception e) {
                Log.e(TAG, "can't set up pose socket for " + host, e);
            }
        });
        sensorManager.registerListener(this, sensor, SensorManager.SENSOR_DELAY_FASTEST, handler);
    }

    void stop() {
        sensorManager.unregisterListener(this);
        if (thread != null) {
            thread.quitSafely();
            thread = null;
        }
        if (socket != null) {
            socket.close();
            socket = null;
        }
    }

    /** Make the current head direction "forward" in SteamVR. */
    void recenter() {
        recenterRequested = true;
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (socket == null || address == null)
            return;
        SensorManager.getQuaternionFromVector(raw, event.values); // w, x, y, z
        float[] q = mul(mul(WORLD_ANDROID_TO_OPENVR, raw), DEVICE_TO_HEAD);

        if (recenterRequested) {
            recenterRequested = false;
            // Head forward (-Z) in the world; undo its heading about Y.
            float[] f = rotate(q, 0, 0, -1);
            double yaw = Math.atan2(-f[0], -f[2]);
            recenter = new float[] {(float) Math.cos(-yaw / 2), 0, (float) Math.sin(-yaw / 2), 0};
        }
        q = mul(recenter, q);

        packet.clear();
        packet.put((byte) 'L').put((byte) 'M').put((byte) 'N').put((byte) '1');
        packet.putInt(sequence++);
        packet.putFloat(q[0]).putFloat(q[1]).putFloat(q[2]).putFloat(q[3]);
        try {
            socket.send(new DatagramPacket(packet.array(), 24, address, PORT));
            sent.incrementAndGet();
            long now = System.nanoTime();
            if (now - lastDisplayNs > 5_000_000_000L) {
                lastDisplayNs = now;
                socket.send(new DatagramPacket(display.array(), 16, address, PORT));
            }
        } catch (Exception e) {
            Log.w(TAG, "pose send failed", e);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    /** Hamilton product a * b, quaternions as {w, x, y, z}. */
    static float[] mul(float[] a, float[] b) {
        return new float[] {
            a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
        };
    }

    /** Rotates vector (x, y, z) by unit quaternion q. */
    static float[] rotate(float[] q, float x, float y, float z) {
        float[] v = mul(mul(q, new float[] {0, x, y, z}), new float[] {q[0], -q[1], -q[2], -q[3]});
        return new float[] {v[1], v[2], v[3]};
    }
}
