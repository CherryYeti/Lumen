package com.lumen.stream;

import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Plays the PC's audio (driver/src/audio_stream.h): UDP 9946, "LMA1" + uint32
 * sequence + 5 ms of 16-bit stereo PCM at 48 kHz.
 *
 * <p>Packets go straight into an AudioTrack. Wi-Fi delivers them unevenly,
 * so whenever the track has run dry (at start, or after a gap) it's given
 * {@link #CUSHION_MS} of silence first; and since the PC's and phone's clocks
 * drift apart, packets are dropped while more than {@link #MAX_QUEUED_MS} (or
 * the track's buffer, if bigger) is waiting, so the delay can't grow.
 *
 * <p>The limit must never be below the track's buffer: Android only starts a
 * stream once its buffer has filled, so a lower limit means it never plays
 * (what happened with Bluetooth headphones, whose tracks get big buffers).
 */
public final class AudioPlayer extends Thread {
    public static final int PORT = 9946;
    private static final String TAG = "LumenAudio";
    private static final int RATE = 48000;
    private static final int HEADER_SIZE = 8;
    private static final int FRAME_BYTES = 4; // 16-bit stereo
    private static final int FRAMES_PER_PACKET = 240;
    private static final int CUSHION_MS = 30;
    private static final int MAX_QUEUED_MS = 120;
    private static final long RESTART_GAP_NS = 500_000_000L;

    public final AtomicLong packets = new AtomicLong();
    public final AtomicLong lostPackets = new AtomicLong();
    public final AtomicLong underruns = new AtomicLong();
    public final AtomicLong skipped = new AtomicLong(); // dropped to keep the delay down
    public final AtomicLong restarts = new AtomicLong(); // output reopened

    private volatile boolean running = true;
    private DatagramSocket socket;

    public AudioPlayer() {
        super("LumenAudio");
    }

    public void shutdown() {
        running = false;
        interrupt();
        DatagramSocket s = socket;
        if (s != null)
            s.close();
    }

    /** Where it plays, e.g. "Galaxy Buds (type 8)" (8 = Bluetooth A2DP), for the stats line. */
    public volatile String output = "none";

    private AudioTrack createTrack() {
        // Small, so it starts playing soon; Android raises it if the output needs more.
        int bufferBytes = 2 * AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO,
                                                          AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack track = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder()
                                    .setUsage(AudioAttributes.USAGE_GAME)
                                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                    .build())
            .setAudioFormat(new AudioFormat.Builder()
                                .setSampleRate(RATE)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .build())
            .setBufferSizeInBytes(bufferBytes)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build();
        // Where it plays (Bluetooth headphones, the jack, the speaker).
        track.addOnRoutingChangedListener((AudioTrack.OnRoutingChangedListener) t -> {
            AudioDeviceInfo device = t.getRoutedDevice();
            output = device == null ? "none" : device.getProductName() + " (type " + device.getType() + ")";
            Log.i(TAG, "audio output: " + output);
        }, null);
        track.play();
        Log.i(TAG, "audio track buffer " + track.getBufferSizeInFrames() * 1000 / RATE + " ms");
        return track;
    }

    @Override
    public void run() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        AudioTrack track = createTrack();
        byte[] silence = new byte[RATE * CUSHION_MS / 1000 * FRAME_BYTES];
        byte[] buffer = new byte[2048];
        DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
        int written = 0; // frames written; wraps like getPlaybackHeadPosition()
        int lastHead = 0;
        long headMovedNs = System.nanoTime();
        int lastSequence = 0;
        long lastPacketNs = 0;
        try (DatagramSocket s = new DatagramSocket(PORT)) {
            socket = s;
            s.setSoTimeout(500);
            Log.i(TAG, "listening for audio on UDP " + PORT);
            while (running) {
                try {
                    s.receive(packet);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                int length = packet.getLength();
                if (length <= HEADER_SIZE || buffer[0] != 'L' || buffer[1] != 'M' || buffer[2] != 'A' || buffer[3] != '1')
                    continue;
                int sequence = ByteBuffer.wrap(buffer, 4, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
                long now = System.nanoTime();
                // Late or reordered: already past. After a pause, accept
                // anything (a restarted sender counts from 0 again).
                boolean recent = lastPacketNs != 0 && now - lastPacketNs < RESTART_GAP_NS;
                if (recent && sequence - lastSequence <= 0)
                    continue;
                if (recent && sequence - lastSequence > 1)
                    lostPackets.addAndGet(sequence - lastSequence - 1);
                lastSequence = sequence;
                lastPacketNs = now;
                if (packets.incrementAndGet() == 1)
                    Log.i(TAG, "audio arriving from " + packet.getAddress().getHostAddress());

                // Android can kill a track's output (e.g. Bluetooth headphones
                // connecting); then writes fail or nothing more gets played.
                // Start over with a new track. Stuck means a full buffer that
                // isn't moving: until it's full, a new track waits to start.
                int head = track.getPlaybackHeadPosition();
                boolean full = written - head >= track.getBufferSizeInFrames() - FRAMES_PER_PACKET;
                if (head != lastHead || !full) {
                    lastHead = head;
                    headMovedNs = now;
                } else if (now - headMovedNs > RESTART_GAP_NS) {
                    Log.w(TAG, "audio output stopped playing; reopening it");
                    track.release();
                    track = createTrack();
                    restarts.incrementAndGet();
                    written = lastHead = head = 0;
                    headMovedNs = now;
                }
                int queued = written - head;
                if (queued <= 0) {
                    if (written != 0)
                        underruns.incrementAndGet();
                    int n = track.write(silence, 0, silence.length, AudioTrack.WRITE_NON_BLOCKING);
                    if (n > 0)
                        written += n / FRAME_BYTES;
                } else if (queued > Math.max(RATE * MAX_QUEUED_MS / 1000, track.getBufferSizeInFrames())) {
                    skipped.incrementAndGet();
                    continue; // running behind the PC's clock: skip 5 ms
                }
                int payload = (length - HEADER_SIZE) & ~(FRAME_BYTES - 1);
                int n = track.write(buffer, HEADER_SIZE, payload, AudioTrack.WRITE_NON_BLOCKING);
                if (n > 0) {
                    written += n / FRAME_BYTES;
                } else if (n < 0) {
                    Log.w(TAG, "audio write failed (" + n + "); reopening the output");
                    track.release();
                    track = createTrack();
                    restarts.incrementAndGet();
                    written = lastHead = 0;
                    headMovedNs = now;
                }
            }
        } catch (Exception e) {
            if (running)
                Log.e(TAG, "audio stopped", e);
        } finally {
            track.release();
        }
    }
}
