package com.lumen.client;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.lumen.stream.VideoDecoder;
import com.lumen.stream.VideoReceiver;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.Locale;

/**
 * Two screens: setup (PC address, and this phone's address to give
 * lumen-capture --stream) and the fullscreen stream.
 *
 * <p>Stream screen: tap to recenter, long-press to toggle the stats overlay,
 * back to return to setup.
 */
public final class MainActivity extends Activity {
    private static final String PREFS = "lumen";
    private static final String KEY_PC_HOST = "pcHost";

    private final Handler main = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private boolean streaming;

    // Stream screen state.
    private AspectSurfaceView surfaceView;
    private TextView statsView;
    private VideoReceiver receiver;
    private VideoDecoder decoder;
    private PoseSender poseSender;
    private WifiManager.WifiLock wifiLock;
    private long lastFrames, lastBytes, lastDecoded, lastPoses, lastStatsNs;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        showSetup();
    }

    // ---- Setup screen ----

    private void showSetup() {
        streaming = false;
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(48, 48, 48, 48);
        layout.setBackgroundColor(Color.BLACK);

        String phoneIp = localIpv4();
        TextView info = new TextView(this);
        info.setTextColor(Color.WHITE);
        info.setTextSize(16);
        info.setText("Lumen\n\nThis phone: " + (phoneIp != null ? phoneIp : "no Wi-Fi address")
                     + "\nOn the PC run:\n  lumen-capture --codec hevc --stream "
                     + (phoneIp != null ? phoneIp : "PHONE_IP") + "\n\nPC address (for head tracking):");
        layout.addView(info);

        EditText pcHost = new EditText(this);
        pcHost.setTextColor(Color.WHITE);
        pcHost.setHint("192.168.1.10");
        pcHost.setHintTextColor(Color.GRAY);
        pcHost.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        pcHost.setText(prefs.getString(KEY_PC_HOST, ""));
        layout.addView(pcHost);

        Button start = new Button(this);
        start.setText("Start");
        start.setOnClickListener(v -> {
            String host = pcHost.getText().toString().trim();
            if (host.isEmpty()) {
                pcHost.setError("Needed to send head tracking");
                return;
            }
            prefs.edit().putString(KEY_PC_HOST, host).apply();
            showStream(host);
        });
        layout.addView(start);

        setContentView(layout);
        setFullscreen(false);
    }

    // ---- Stream screen ----

    private void showStream(String pcHost) {
        streaming = true;
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        surfaceView = new AspectSurfaceView(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                                                               FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        statsView = new TextView(this);
        statsView.setTextColor(Color.GREEN);
        statsView.setBackgroundColor(0x80000000);
        statsView.setTextSize(12);
        statsView.setPadding(16, 8, 16, 8);
        statsView.setText("Waiting for video on UDP " + VideoReceiver.PORT + "...");
        root.addView(statsView, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                                                             FrameLayout.LayoutParams.WRAP_CONTENT,
                                                             Gravity.TOP | Gravity.START));

        poseSender = new PoseSender(this, pcHost);
        if (!poseSender.hasSensor())
            statsView.setText("This phone has no game rotation vector sensor; head tracking is off.");

        GestureDetector gestures = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onSingleTapUp(MotionEvent e) {
                poseSender.recenter();
                return true;
            }

            @Override
            public void onLongPress(MotionEvent e) {
                statsView.setVisibility(statsView.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
            }
        });
        root.setOnTouchListener((v, event) -> gestures.onTouchEvent(event));

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                startVideo(holder);
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                stopVideo();
            }
        });

        setContentView(root);
        setFullscreen(true);
        startStreamingResources();
    }

    private void startVideo(SurfaceHolder holder) {
        decoder = new VideoDecoder(holder.getSurface(), new VideoDecoder.Listener() {
            @Override
            public void onVideoSize(int width, int height) {
                main.post(() -> {
                    if (surfaceView != null)
                        surfaceView.setVideoSize(width, height);
                });
            }

            @Override
            public void onError(String message) {
                main.post(() -> {
                    if (statsView != null)
                        statsView.setText(message);
                });
            }
        });
        receiver = new VideoReceiver(decoder);
        decoder.setReceiver(receiver);
        decoder.start();
        receiver.start();
    }

    private void stopVideo() {
        if (receiver != null) {
            receiver.shutdown();
            receiver = null;
        }
        if (decoder != null) {
            decoder.shutdown();
            try {
                decoder.join(500);
            } catch (InterruptedException ignored) {
            }
            decoder = null;
        }
    }

    private void startStreamingResources() {
        if (!streaming)
            return;
        if (poseSender != null)
            poseSender.start();
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null && wifiLock == null) {
            // Keeps Wi-Fi out of power save, which otherwise adds tens of ms.
            int mode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                                        : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wifi.createWifiLock(mode, "lumen");
            wifiLock.acquire();
        }
        lastStatsNs = System.nanoTime();
        main.postDelayed(this::updateStats, 1000);
    }

    private void stopStreamingResources() {
        main.removeCallbacksAndMessages(null);
        if (poseSender != null)
            poseSender.stop();
        if (wifiLock != null) {
            wifiLock.release();
            wifiLock = null;
        }
    }

    private void updateStats() {
        if (!streaming)
            return;
        main.postDelayed(this::updateStats, 1000);
        VideoReceiver r = receiver;
        VideoDecoder d = decoder;
        if (r == null || d == null || statsView == null)
            return;
        long now = System.nanoTime();
        double seconds = (now - lastStatsNs) / 1e9;
        lastStatsNs = now;
        long frames = r.frames.get(), bytes = r.bytes.get(), decoded = d.decoded.get();
        long poses = poseSender != null ? poseSender.sent.get() : 0;
        long latencyCount = d.latencyCount.getAndSet(0), latencySum = d.latencySumUs.getAndSet(0);
        if (frames == 0) {
            statsView.setText(String.format(Locale.US, "Waiting for video on UDP %d...  poses %.0f/s",
                                            VideoReceiver.PORT, (poses - lastPoses) / seconds));
        } else {
            String stats = String.format(
                Locale.US,
                "%s\nrecv %.1f fps  %.1f Mbit/s   decoded %.1f fps  decode %.1f ms\n"
                    + "lost %d  discarded %d  decoder drops %d  keyframe requests %d\nposes %.0f/s",
                d.codecName(), (frames - lastFrames) / seconds, (bytes - lastBytes) * 8 / seconds / 1e6,
                (decoded - lastDecoded) / seconds, latencyCount > 0 ? latencySum / 1000.0 / latencyCount : 0.0,
                r.lostFrames.get(), r.discardedFrames.get(), d.dropped.get(), r.keyframeRequests.get(),
                (poses - lastPoses) / seconds);
            statsView.setText(stats);
            // Same line in logcat (tag LumenStats) for measuring over adb.
            Log.i("LumenStats", stats.replace('\n', ' '));
        }
        lastFrames = frames;
        lastBytes = bytes;
        lastDecoded = decoded;
        lastPoses = poses;
    }

    // ---- Lifecycle ----

    @Override
    protected void onResume() {
        super.onResume();
        startStreamingResources();
    }

    @Override
    protected void onPause() {
        stopStreamingResources();
        super.onPause();
    }

    @Override
    public void onBackPressed() {
        if (streaming) {
            stopStreamingResources();
            stopVideo();
            showSetup();
        } else {
            super.onBackPressed();
        }
    }

    private void setFullscreen(boolean immersive) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller == null)
                return;
            if (immersive) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            } else {
                controller.show(WindowInsets.Type.systemBars());
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                immersive ? View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                          : 0);
        }
    }

    /** This phone's IPv4 address, preferring Wi-Fi. */
    private static String localIpv4() {
        String fallback = null;
        try {
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp() || nif.isLoopback())
                    continue;
                for (InetAddress addr : Collections.list(nif.getInetAddresses())) {
                    if (!(addr instanceof Inet4Address))
                        continue;
                    if (nif.getName().startsWith("wlan"))
                        return addr.getHostAddress();
                    if (fallback == null)
                        fallback = addr.getHostAddress();
                }
            }
        } catch (Exception ignored) {
        }
        return fallback;
    }
}
