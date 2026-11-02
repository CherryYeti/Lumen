package com.lumen.stream;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Decodes frames from VideoReceiver with MediaCodec and renders each one to
 * the surface as soon as it's decoded (no frame pacing or reprojection yet).
 *
 * If the decoder falls behind, queued frames are dropped and a keyframe is
 * requested, because later frames depend on the dropped ones.
 */
public final class VideoDecoder extends Thread implements VideoReceiver.Listener {
    public interface Listener {
        void onVideoSize(int width, int height);
        void onError(String message);
        /** A decoded frame is about to be sent to the surface. Called on the decoder thread. */
        default void onFrameRendered(Frame frame) {}
    }

    private static final String TAG = "LumenDecoder";
    // Used if a frame doesn't say its size.
    private static final int DEFAULT_WIDTH = 3600, DEFAULT_HEIGHT = 2024;

    public final AtomicLong decoded = new AtomicLong();
    public final AtomicLong dropped = new AtomicLong();
    /** Sum of receive -> decoded-output times since the last read, in µs. */
    public final AtomicLong latencySumUs = new AtomicLong();
    public final AtomicLong latencyCount = new AtomicLong();

    private final Surface surface;
    private final Listener listener;
    private final ArrayBlockingQueue<Frame> queue = new ArrayBlockingQueue<>(3);
    private volatile boolean running = true;
    private VideoReceiver receiver;
    private MediaCodec codec;
    private boolean codecHevc;
    private int codecWidth, codecHeight;
    private boolean needKeyframe = true;
    private String codecName = "";
    // Frames queued in the codec, oldest first, to match outputs back to them.
    private final ArrayDeque<Frame> inFlight = new ArrayDeque<>();

    public VideoDecoder(Surface surface, Listener listener) {
        super("LumenVideoDecoder");
        this.surface = surface;
        this.listener = listener;
    }

    public void setReceiver(VideoReceiver receiver) {
        this.receiver = receiver;
    }

    public String codecName() {
        return codecName;
    }

    public void shutdown() {
        running = false;
        interrupt();
    }

    @Override
    public void onFrame(Frame frame) {
        if (!queue.offer(frame)) {
            // Behind: everything queued is now useless without a fresh keyframe.
            dropped.addAndGet(queue.size() + 1);
            Frame old;
            while ((old = queue.poll()) != null)
                recycle(old);
            recycle(frame);
            if (receiver != null)
                receiver.requestKeyframe();
        }
    }

    /** No output this long while feeding input = the codec is wedged; restart it. */
    private static final long STUCK_US = 2_000_000;
    private long lastOutputUs, lastInputUs, startedUs;
    public final AtomicLong restarts = new AtomicLong();

    @Override
    public void run() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            while (running) {
                Frame frame = queue.poll(2, TimeUnit.MILLISECONDS);
                try {
                    if (frame != null)
                        feed(frame);
                    if (codec != null)
                        drain(info);
                    long now = System.nanoTime() / 1000;
                    if (codec != null && now - lastInputUs < 500_000 && now - Math.max(lastOutputUs, startedUs) > STUCK_US)
                        throw new IllegalStateException("no output for 2 s (unsupported size " + codecWidth + "x"
                                                        + codecHeight + "?)");
                } catch (InterruptedException e) {
                    throw e;
                } catch (Exception e) {
                    // A wedged or failed codec: start over on the next keyframe
                    // instead of staying dead.
                    Log.e(TAG, "decoder failed, restarting", e);
                    listener.onError("Decoder restarted: " + e.getMessage());
                    release();
                    inFlight.clear();
                    restarts.incrementAndGet();
                    if (receiver != null)
                        receiver.requestKeyframe();
                }
            }
        } catch (InterruptedException e) {
            // shutting down
        } finally {
            release();
        }
    }

    private void recycle(Frame frame) {
        if (receiver != null)
            receiver.recycle(frame.data);
    }

    private void feed(Frame frame) throws Exception {
        try {
            feedCodec(frame);
        } finally {
            recycle(frame); // its data is in the codec (or unwanted); only the pose fields are used later
        }
    }

    private void feedCodec(Frame frame) throws Exception {
        // (Re)start on a codec or size change, e.g. a game taking over from the
        // SteamVR home with different-sized eye textures. The PC restarts its
        // encoder then, so the first frame at the new size is a keyframe.
        boolean resized = frame.width > 0 && (frame.width != codecWidth || frame.height != codecHeight);
        if (codec == null || frame.hevc != codecHevc || resized) {
            if (!frame.keyframe)
                return; // a decoder can only start on a keyframe
            release();
            inFlight.clear();
            start(frame.hevc, frame.width > 0 ? frame.width : DEFAULT_WIDTH,
                  frame.height > 0 ? frame.height : DEFAULT_HEIGHT);
        }
        if (needKeyframe && !frame.keyframe)
            return;
        int index = codec.dequeueInputBuffer(10_000);
        if (index < 0) {
            lastInputUs = System.nanoTime() / 1000; // trying to feed counts as input
            // No input buffer in 10 ms: drop it and resync on a keyframe.
            dropped.incrementAndGet();
            needKeyframe = true;
            if (receiver != null)
                receiver.requestKeyframe();
            return;
        }
        needKeyframe = false;
        lastInputUs = System.nanoTime() / 1000;
        ByteBuffer input = codec.getInputBuffer(index);
        input.clear();
        input.put(frame.data, 0, frame.size);
        // presentationTimeUs carries the receive time, so the output side can
        // measure receive -> decoded latency.
        codec.queueInputBuffer(index, 0, frame.size, frame.receivedUs,
                               frame.keyframe ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);
        inFlight.addLast(frame);
        while (inFlight.size() > 16)
            inFlight.removeFirst(); // decoder dropped some; don't grow forever
    }

    private void drain(MediaCodec.BufferInfo info) {
        for (;;) {
            int index = codec.dequeueOutputBuffer(info, 0);
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                reportSize(codec.getOutputFormat());
                continue;
            }
            if (index < 0)
                return; // INFO_TRY_AGAIN_LATER (or the deprecated buffers-changed)
            long latency = System.nanoTime() / 1000 - info.presentationTimeUs;
            latencySumUs.addAndGet(latency);
            latencyCount.incrementAndGet();
            decoded.incrementAndGet();
            lastOutputUs = System.nanoTime() / 1000;
            Frame rendered = null;
            while (!inFlight.isEmpty() && inFlight.peekFirst().receivedUs <= info.presentationTimeUs)
                rendered = inFlight.removeFirst();
            // Report before rendering, so a consumer latching the surface
            // already knows this frame's pose.
            if (rendered != null && rendered.receivedUs == info.presentationTimeUs)
                listener.onFrameRendered(rendered);
            codec.releaseOutputBuffer(index, true); // render now; surface timestamp = pts * 1000
        }
    }

    private void start(boolean hevc, int width, int height) throws Exception {
        String mime = hevc ? MediaFormat.MIMETYPE_VIDEO_HEVC : MediaFormat.MIMETYPE_VIDEO_AVC;
        MediaFormat format = MediaFormat.createVideoFormat(mime, width, height);
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0); // realtime
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        codec = MediaCodec.createDecoderByType(mime);
        codecName = codec.getName();
        codec.configure(format, surface, null, 0);
        codec.start();
        codecHevc = hevc;
        startedUs = System.nanoTime() / 1000;
        codecWidth = width;
        codecHeight = height;
        needKeyframe = true;
        Log.i(TAG, "started " + codecName + " for " + mime + " " + width + "x" + height);
    }

    private void reportSize(MediaFormat format) {
        int width = format.getInteger(MediaFormat.KEY_WIDTH);
        int height = format.getInteger(MediaFormat.KEY_HEIGHT);
        if (format.containsKey("crop-right") && format.containsKey("crop-left"))
            width = format.getInteger("crop-right") - format.getInteger("crop-left") + 1;
        if (format.containsKey("crop-bottom") && format.containsKey("crop-top"))
            height = format.getInteger("crop-bottom") - format.getInteger("crop-top") + 1;
        listener.onVideoSize(width, height);
    }

    private void release() {
        if (codec == null)
            return;
        try {
            codec.stop();
        } catch (Exception e) {
            Log.w(TAG, "stop failed", e);
        }
        codec.release();
        codec = null;
    }
}
