package com.lumen.stream;

import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.TreeMap;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Receives the PC's UDP video stream and reassembles frames. Same format and
 * rules as driver/src/video_stream.h. Header "LMV3", 68 bytes:
 *
 * <ul>
 *   <li>a frame missing any packet is dropped;
 *   <li>after a gap in frame ids, frames are discarded until a keyframe
 *       arrives, and "LMK1" keyframe requests go back to the sender every
 *       100 ms until one does.
 * </ul>
 *
 * <p>Allocates next to nothing per packet: each frame is reassembled straight
 * into a reused buffer, which the decoder hands back with {@link #recycle}.
 * Allocating per packet (~10 MB/s at 40 Mbit/s) made the garbage collector
 * run every few seconds, and on a phone busy with VrApi that stalled
 * receiving, decoding and pose sending for a moment each time.
 */
public final class VideoReceiver extends Thread {
    public interface Listener {
        void onFrame(Frame frame);
    }

    public static final int PORT = 9945;
    private static final String TAG = "LumenVideo";
    private static final int HEADER_SIZE = 68;
    private static final int FLAG_KEYFRAME = 1;
    private static final int FLAG_HEVC = 2;
    private static final int MAX_PARTIAL = 32;
    /** Payload of every packet but a frame's last (driver/src/video_stream.h kMaxPayload). */
    private static final int MAX_PAYLOAD = 1400 - HEADER_SIZE;
    /** Smallest buffer allocated, so most keyframes fit a reused one too. */
    private static final int MIN_BUFFER = 512 * 1024;
    private static final long REQUEST_INTERVAL_US = 100_000;
    private static final long RESTART_GAP_US = 500_000;
    private static final byte[] KEYFRAME_REQUEST = {'L', 'M', 'K', '1'};

    private static final class Partial {
        final byte[] data; // packet i at i * MAX_PAYLOAD
        final boolean[] have;
        int received, size;
        int flags;
        float[] renderPose;
        float[] tanHalfFov;
        float[] bounds;
        int width, height;

        Partial(byte[] data, int count, int flags, float[] renderPose, float[] tanHalfFov, float[] bounds,
                int width, int height) {
            this.data = data;
            this.have = new boolean[count];
            this.flags = flags;
            this.renderPose = renderPose;
            this.tanHalfFov = tanHalfFov;
            this.bounds = bounds;
            this.width = width;
            this.height = height;
        }
    }

    public final AtomicLong frames = new AtomicLong();
    public final AtomicLong bytes = new AtomicLong();
    public final AtomicLong lostFrames = new AtomicLong();
    public final AtomicLong discardedFrames = new AtomicLong();
    public final AtomicLong keyframeRequests = new AtomicLong();

    private final Listener listener;
    private volatile boolean running = true;
    private volatile boolean keyframeWanted = false;
    private DatagramSocket socket;
    private volatile InetAddress senderAddress;
    private final ArrayBlockingQueue<byte[]> pool = new ArrayBlockingQueue<>(8);

    public VideoReceiver(Listener listener) {
        super("LumenVideoReceiver");
        this.listener = listener;
    }

    /** Called by the decoder when it had to drop a frame. */
    public void requestKeyframe() {
        keyframeWanted = true;
    }

    /** Where the video is coming from (the PC), or null before the first packet. */
    /** Gives a frame's buffer back for reuse once its data has been consumed. */
    public void recycle(byte[] buffer) {
        pool.offer(buffer); // pool full: let it go
    }

    private byte[] obtain(int capacity) {
        byte[] buffer;
        while ((buffer = pool.poll()) != null)
            if (buffer.length >= capacity)
                return buffer;
        return new byte[Math.max(capacity, MIN_BUFFER)];
    }

    public InetAddress senderAddress() {
        return senderAddress;
    }

    public void shutdown() {
        running = false;
        interrupt();
        DatagramSocket s = socket;
        if (s != null)
            s.close();
    }

    @Override
    public void run() {
        try {
            socket = new DatagramSocket(null);
            socket.setReuseAddress(true);
            socket.setReceiveBufferSize(8 * 1024 * 1024);
            socket.setSoTimeout(100);
            socket.bind(new InetSocketAddress(PORT));
        } catch (Exception e) {
            Log.e(TAG, "can't open UDP " + PORT, e);
            return;
        }

        byte[] buffer = new byte[65536];
        DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
        ByteBuffer header = ByteBuffer.wrap(buffer).order(ByteOrder.LITTLE_ENDIAN);
        // Ordered by frame id; ids only wrap after 2^32 frames (~2 years at 60 Hz).
        TreeMap<Integer, Partial> partial = new TreeMap<>(Integer::compareUnsigned);
        boolean haveDelivered = false;
        int lastDelivered = 0;
        long lastDeliveredUs = 0;
        boolean needKeyframe = true; // until the decoder has a clean starting point
        SocketAddress sender = null;
        long lastRequestUs = 0;

        while (running) {
            if (keyframeWanted) {
                keyframeWanted = false;
                needKeyframe = true;
            }
            long nowUs = System.nanoTime() / 1000;
            if (needKeyframe && sender != null && nowUs - lastRequestUs >= REQUEST_INTERVAL_US) {
                try {
                    socket.send(new DatagramPacket(KEYFRAME_REQUEST, KEYFRAME_REQUEST.length, sender));
                    keyframeRequests.incrementAndGet();
                } catch (Exception e) {
                    Log.w(TAG, "keyframe request failed", e);
                }
                lastRequestUs = nowUs;
            }

            try {
                packet.setLength(buffer.length);
                socket.receive(packet);
            } catch (SocketTimeoutException e) {
                continue;
            } catch (Exception e) {
                if (running)
                    Log.e(TAG, "receive failed", e);
                break;
            }

            int length = packet.getLength();
            if (length < HEADER_SIZE || buffer[0] != 'L' || buffer[1] != 'M' || buffer[2] != 'V' || buffer[3] != '3')
                continue;
            int frameId = header.getInt(4);
            int index = header.getShort(8) & 0xffff;
            int count = header.getShort(10) & 0xffff;
            int flags = buffer[12] & 0xff;
            if (count == 0 || index >= count)
                continue;
            sender = packet.getSocketAddress();
            senderAddress = packet.getAddress();

            // Too old: something newer was already delivered. Unless nothing has
            // arrived for a while: then the sender restarted (ids back at 0).
            if (haveDelivered && frameId - lastDelivered <= 0) {
                if (System.nanoTime() / 1000 - lastDeliveredUs < RESTART_GAP_US)
                    continue;
                Log.i(TAG, "sender restarted (frame " + frameId + " after " + lastDelivered + ")");
                haveDelivered = false;
                for (Partial p : partial.values())
                    recycle(p.data);
                partial.clear();
                needKeyframe = true;
            }

            while (partial.size() > MAX_PARTIAL)
                recycle(partial.pollFirstEntry().getValue().data);
            Partial frame = partial.get(frameId);
            if (frame == null) {
                float[] pose = {header.getFloat(24), header.getFloat(28), header.getFloat(32), header.getFloat(36)};
                float[] fov = {header.getFloat(40), header.getFloat(44)};
                float[] bounds = {header.getFloat(48), header.getFloat(52), header.getFloat(56), header.getFloat(60)};
                frame = new Partial(obtain(count * MAX_PAYLOAD), count, flags, pose, fov, bounds, header.getShort(64) & 0xffff,
                                    header.getShort(66) & 0xffff);
                partial.put(frameId, frame);
            }
            int payload = length - HEADER_SIZE;
            boolean last = index == count - 1;
            if (frame.have.length != count || frame.have[index] || payload > MAX_PAYLOAD
                || (!last && payload != MAX_PAYLOAD))
                continue; // inconsistent or duplicate
            System.arraycopy(buffer, HEADER_SIZE, frame.data, index * MAX_PAYLOAD, payload);
            frame.have[index] = true;
            if (last)
                frame.size = index * MAX_PAYLOAD + payload;
            if (++frame.received < count)
                continue;

            // Complete. Anything older still pending is lost.
            int lost = haveDelivered ? frameId - lastDelivered - 1 : 0;
            boolean keyframe = (frame.flags & FLAG_KEYFRAME) != 0;
            if (lost > 0) {
                lostFrames.addAndGet(lost);
                needKeyframe = true;
            }
            partial.remove(frameId);
            if (needKeyframe && !keyframe) {
                discardedFrames.incrementAndGet(); // references are broken
                recycle(frame.data);
            } else {
                needKeyframe = false;
                frames.incrementAndGet();
                bytes.addAndGet(frame.size);
                listener.onFrame(new Frame(frameId, frame.data, frame.size, keyframe, (frame.flags & FLAG_HEVC) != 0,
                                           System.nanoTime() / 1000, frame.renderPose, frame.tanHalfFov,
                                           frame.bounds, frame.width, frame.height));
            }
            haveDelivered = true;
            lastDelivered = frameId;
            lastDeliveredUs = System.nanoTime() / 1000;
            // Anything older is lost; give its buffer back.
            while (!partial.isEmpty() && Integer.compareUnsigned(partial.firstKey(), frameId) < 0)
                recycle(partial.pollFirstEntry().getValue().data);
        }
        socket.close();
    }
}
