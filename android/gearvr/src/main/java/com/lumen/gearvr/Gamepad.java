package com.lumen.gearvr;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * State of a Bluetooth gamepad (Xbox / 8BitDo in X-input or Android mode)
 * paired with the phone, sent to the PC in the pose packets ("LMN4"), where
 * the driver turns it into two virtual Oculus Touch controllers.
 *
 * <p>Bit layout matches GamepadButtons in driver/src/pose_receiver.h.
 */
final class Gamepad implements InputManager.InputDeviceListener {
    static final int A = 1, B = 1 << 1, X = 1 << 2, Y = 1 << 3, LB = 1 << 4, RB = 1 << 5, LS = 1 << 6,
                     RS = 1 << 7, START = 1 << 8, SELECT = 1 << 9, GUIDE = 1 << 10, CONNECTED = 1 << 31;

    private final InputManager inputManager;
    private volatile int buttons;
    private volatile boolean connected;
    volatile float lx, ly, rx, ry, lt, rt; // sticks -1..1 (+y up), triggers 0..1
    private volatile String name = "";
    private volatile int deviceId = -1;
    private boolean reportedNoRumble;

    Gamepad(Context context, Handler handler) {
        inputManager = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        inputManager.registerInputDeviceListener(this, handler);
        refreshConnected();
    }

    void close() {
        inputManager.unregisterInputDeviceListener(this);
    }

    /** Buttons for the packet, including the CONNECTED bit. */
    int buttons() {
        return connected ? (buttons | CONNECTED) : 0;
    }

    boolean isConnected() {
        return connected;
    }

    String name() {
        return name;
    }

    static boolean isGamepad(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /**
     * Whether a key came from a gamepad. Checks the device, not just the
     * event: an 8BitDo pad sends Select as a keyboard-type Back key, which
     * would otherwise look like the Gear VR's own back button.
     */
    static boolean fromGamepad(KeyEvent event) {
        if (isGamepad(event.getSource()))
            return true;
        InputDevice device = event.getDevice();
        return device != null && !device.isVirtual() && isGamepad(device.getSources());
    }

    /** Handles a gamepad button. Returns false for keys that aren't gamepad buttons. */
    boolean onKey(KeyEvent event) {
        int bit;
        switch (event.getKeyCode()) {
        case KeyEvent.KEYCODE_BUTTON_A: bit = A; break;
        case KeyEvent.KEYCODE_BUTTON_B: bit = B; break;
        case KeyEvent.KEYCODE_BUTTON_X: bit = X; break;
        case KeyEvent.KEYCODE_BUTTON_Y: bit = Y; break;
        case KeyEvent.KEYCODE_BUTTON_L1: bit = LB; break;
        case KeyEvent.KEYCODE_BUTTON_R1: bit = RB; break;
        case KeyEvent.KEYCODE_BUTTON_THUMBL: bit = LS; break;
        case KeyEvent.KEYCODE_BUTTON_THUMBR: bit = RS; break;
        case KeyEvent.KEYCODE_BUTTON_START: bit = START; break;
        case KeyEvent.KEYCODE_BUTTON_SELECT:
        case KeyEvent.KEYCODE_BACK: bit = SELECT; break; // some pads send BACK for View/Select
        case KeyEvent.KEYCODE_BUTTON_MODE: bit = GUIDE; break;
        // Digital triggers (some pads): treat as fully pressed analog.
        case KeyEvent.KEYCODE_BUTTON_L2: lt = event.getAction() == KeyEvent.ACTION_DOWN ? 1f : 0f; return true;
        case KeyEvent.KEYCODE_BUTTON_R2: rt = event.getAction() == KeyEvent.ACTION_DOWN ? 1f : 0f; return true;
        default: return false;
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN)
            buttons |= bit;
        else if (event.getAction() == KeyEvent.ACTION_UP)
            buttons &= ~bit;
        connected = true;
        return true;
    }

    /** Handles stick and trigger motion. */
    boolean onMotion(MotionEvent event) {
        if (!isGamepad(event.getSource()) || event.getAction() != MotionEvent.ACTION_MOVE)
            return false;
        InputDevice device = event.getDevice();
        lx = axis(event, device, MotionEvent.AXIS_X);
        ly = -axis(event, device, MotionEvent.AXIS_Y); // Android: down is +
        rx = axis(event, device, MotionEvent.AXIS_Z);
        ry = -axis(event, device, MotionEvent.AXIS_RZ);
        lt = Math.max(axis(event, device, MotionEvent.AXIS_LTRIGGER), axis(event, device, MotionEvent.AXIS_BRAKE));
        rt = Math.max(axis(event, device, MotionEvent.AXIS_RTRIGGER), axis(event, device, MotionEvent.AXIS_GAS));
        connected = true;
        return true;
    }

    /**
     * Rumbles the pad (a game's haptic event, from the PC). Android 8 has one
     * vibrator per input device, so both hands share it. Many pads' Bluetooth
     * drivers expose no motor at all (an 8BitDo on the Galaxy S7 doesn't);
     * then this does nothing, and says so once in the log.
     */
    void vibrate(float amplitude, float seconds) {
        InputDevice device = deviceId >= 0 ? InputDevice.getDevice(deviceId) : null;
        Vibrator vibrator = device != null ? device.getVibrator() : null;
        if (vibrator == null || !vibrator.hasVibrator()) {
            if (device != null && !reportedNoRumble)
                Log.i("LumenGearVR", "gamepad " + device.getName() + " has no rumble motor Android can drive");
            reportedNoRumble = device != null;
            return;
        }
        if (amplitude <= 0f) {
            vibrator.cancel();
            return;
        }
        // Games often send 0 for "one short pulse"; under ~20 ms a motor
        // barely spins up.
        long ms = Math.max(20, Math.min(5000, Math.round(seconds * 1000)));
        int strength = vibrator.hasAmplitudeControl() ? Math.max(1, Math.min(255, Math.round(amplitude * 255)))
                                                      : VibrationEffect.DEFAULT_AMPLITUDE;
        vibrator.vibrate(VibrationEffect.createOneShot(ms, strength));
    }

    private static float axis(MotionEvent event, InputDevice device, int axis) {
        if (device == null || device.getMotionRange(axis, event.getSource()) == null)
            return 0f;
        return event.getAxisValue(axis);
    }

    private void refreshConnected() {
        boolean found = false;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (device != null && !device.isVirtual() && isGamepad(device.getSources())) {
                found = true;
                name = device.getName();
                deviceId = id;
            }
        }
        connected = found;
        if (!found) {
            deviceId = -1;
            buttons = 0;
            lx = ly = rx = ry = lt = rt = 0f;
            name = "";
        }
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        refreshConnected();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        refreshConnected();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        refreshConnected();
    }
}
