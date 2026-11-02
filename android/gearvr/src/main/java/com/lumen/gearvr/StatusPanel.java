package com.lumen.gearvr;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.List;

/**
 * Draws the status panel shown in the headset while there's no video, so you
 * can tell the app is running and what it's waiting for. The native side
 * shows the bitmap as a head-locked panel (see lumen_gearvr.c).
 */
final class StatusPanel {
    static final int WIDTH = 1024, HEIGHT = 512;

    private final Bitmap bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
    private final Canvas canvas = new Canvas(bitmap);
    private final Paint title = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint frame = new Paint(Paint.ANTI_ALIAS_FLAG);

    StatusPanel() {
        title.setColor(Color.WHITE);
        title.setTextSize(64);
        title.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        text.setColor(Color.rgb(210, 210, 210));
        text.setTextSize(36);
        frame.setColor(Color.rgb(90, 140, 255));
        frame.setStyle(Paint.Style.STROKE);
        frame.setStrokeWidth(4);
    }

    /** Redraws the panel. The first line is shown highlighted. */
    Bitmap draw(List<String> lines) {
        // Fully black edges: outside the panel the compositor clamps to them.
        canvas.drawColor(Color.BLACK);
        canvas.drawRoundRect(12, 12, WIDTH - 12, HEIGHT - 12, 24, 24, frame);
        canvas.drawText("Lumen", 48, 100, title);
        float y = 170;
        for (int i = 0; i < lines.size(); i++) {
            text.setColor(i == 0 ? Color.rgb(120, 200, 255) : Color.rgb(210, 210, 210));
            canvas.drawText(lines.get(i), 48, y, text);
            y += 54;
        }
        return bitmap;
    }

    /** This phone's IPv4 address, preferring Wi-Fi, or null. */
    static String localIpv4() {
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
