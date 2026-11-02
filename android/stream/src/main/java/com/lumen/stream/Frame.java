package com.lumen.stream;

/** One complete encoded video frame (an Annex-B access unit). */
public final class Frame {
    public final int frameId;
    /** The access unit is data[0, size); data is a pooled buffer (see VideoReceiver.recycle). */
    public final byte[] data;
    public final int size;
    public final boolean keyframe;
    public final boolean hevc;
    /** When the last packet arrived, System.nanoTime() / 1000. */
    public final long receivedUs;
    /** Head orientation SteamVR rendered this frame with: w, x, y, z (OpenVR frame). */
    public final float[] renderPose;
    /** Per-eye tan(half field of view): x, y. */
    public final float[] tanHalfFov;
    /** Picture area within each eye's half: uMin, vMin, uMax, vMax (0..1, v down). */
    public final float[] bounds;
    /** Encoded frame size, both eyes side by side. */
    public final int width, height;

    Frame(int frameId, byte[] data, int size, boolean keyframe, boolean hevc, long receivedUs, float[] renderPose,
          float[] tanHalfFov, float[] bounds, int width, int height) {
        this.frameId = frameId;
        this.data = data;
        this.size = size;
        this.keyframe = keyframe;
        this.hevc = hevc;
        this.receivedUs = receivedUs;
        this.renderPose = renderPose;
        this.tanHalfFov = tanHalfFov;
        this.bounds = bounds;
        this.width = width;
        this.height = height;
    }
}
