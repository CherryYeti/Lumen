package com.lumen.client;

import android.content.Context;
import android.view.SurfaceView;

/** A SurfaceView that keeps the video's aspect ratio, letterboxed in its parent. */
final class AspectSurfaceView extends SurfaceView {
    private int videoWidth = 16, videoHeight = 9;

    AspectSurfaceView(Context context) {
        super(context);
    }

    void setVideoSize(int width, int height) {
        if (width <= 0 || height <= 0 || (width == videoWidth && height == videoHeight))
            return;
        videoWidth = width;
        videoHeight = height;
        requestLayout();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        int height = MeasureSpec.getSize(heightMeasureSpec);
        if ((long) width * videoHeight > (long) height * videoWidth)
            width = (int) ((long) height * videoWidth / videoHeight);
        else
            height = (int) ((long) width * videoHeight / videoWidth);
        setMeasuredDimension(width, height);
    }
}
