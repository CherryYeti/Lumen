#include "touch_device.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace lumen {

namespace {

constexpr float kHandSide = 0.18f;    // meters left/right of the body's centre line
constexpr float kHandDown = 0.45f;    // below the eyes
constexpr float kHandForward = 0.30f; // in front of the eyes
constexpr float kDeadZone = 0.08f;    // stick / trigger noise

float Dead(float v) { return std::fabs(v) < kDeadZone ? 0.f : v; }

// The CV1 Touch model's aim ("tip") points 39.4 degrees down from the
// controller body (rendermodels/oculus_cv1_controller_*: tip rotate_xyz).
constexpr float kTipPitchDeg = 39.4f;
// The hands are below and beside the eyes; angle them so their rays meet the
// gaze line this far ahead, putting the dashboard cursor where you look.
constexpr float kConvergeMeters = 2.0f;

Orientation Mul(const Orientation &a, const Orientation &b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Orientation AxisAngle(float x, float y, float z, float radians) {
    float s = std::sin(radians / 2.f);
    return {std::cos(radians / 2.f), x * s, y * s, z * s};
}

} // namespace

vr::EVRInitError TouchDevice::Activate(uint32_t objectId) {
    objectId_ = objectId;
    auto *props = vr::VRProperties();
    vr::PropertyContainerHandle_t c = props->TrackedDeviceToPropertyContainer(objectId_);

    props->SetStringProperty(c, vr::Prop_ControllerType_String, "oculus_touch");
    props->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
    props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                            left_ ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    props->SetStringProperty(c, vr::Prop_RenderModelName_String,
                             left_ ? "oculus_cv1_controller_left" : "oculus_cv1_controller_right");
    props->SetStringProperty(c, vr::Prop_ModelNumber_String,
                             left_ ? "Oculus Rift CV1 (Left Controller)" : "Oculus Rift CV1 (Right Controller)");
    props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Oculus");
    props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "lumen");

    auto *input = vr::VRDriverInput();
    auto boolean = [&](const char *path, vr::VRInputComponentHandle_t *h) { input->CreateBooleanComponent(c, path, h); };
    auto scalar = [&](const char *path, vr::VRInputComponentHandle_t *h, bool twoSided) {
        input->CreateScalarComponent(c, path, h, vr::VRScalarType_Absolute,
                                     twoSided ? vr::VRScalarUnits_NormalizedTwoSided
                                              : vr::VRScalarUnits_NormalizedOneSided);
    };
    boolean("/input/joystick/click", &in_.joystickClick);
    boolean("/input/joystick/touch", &in_.joystickTouch);
    scalar("/input/joystick/x", &in_.joystickX, true);
    scalar("/input/joystick/y", &in_.joystickY, true);
    scalar("/input/trigger/value", &in_.triggerValue, false);
    boolean("/input/trigger/touch", &in_.triggerTouch);
    boolean("/input/trigger/click", &in_.triggerClick);
    scalar("/input/grip/value", &in_.gripValue, false);
    boolean("/input/grip/touch", &in_.gripTouch);
    boolean("/input/grip/click", &in_.gripClick);
    boolean(left_ ? "/input/x/click" : "/input/a/click", &in_.button1Click);
    boolean(left_ ? "/input/x/touch" : "/input/a/touch", &in_.button1Touch);
    boolean(left_ ? "/input/y/click" : "/input/b/click", &in_.button2Click);
    boolean(left_ ? "/input/y/touch" : "/input/b/touch", &in_.button2Touch);
    if (left_) {
        boolean("/input/system/click", &in_.systemClick);
        boolean("/input/system/touch", &in_.systemTouch);
    }
    boolean("/input/thumbrest/touch", &in_.thumbrestTouch);
    input->CreateHapticComponent(c, "/output/haptic", &haptic_);

    vr::VRDriverLog()->Log(left_ ? "lumen: gamepad left Touch activated" : "lumen: gamepad right Touch activated");
    return vr::VRInitError_None;
}

void TouchDevice::DebugRequest(const char *request, char *responseBuffer, uint32_t responseBufferSize) {
    if (responseBufferSize > 0)
        responseBuffer[0] = '\0';
}

vr::DriverPose_t TouchDevice::GetPose() {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;

    std::lock_guard<std::mutex> lock(mutex_);
    const Orientation &q = head_;
    // This hand's spot relative to the eyes, in the body's (yaw-only) frame:
    // the default plus whatever the user moved it by (View + stick).
    float side = (left_ ? -kHandSide : kHandSide) + offset_[0];
    float down = kHandDown - offset_[1];
    float forward = kHandForward + offset_[2];
    // Aim where the head looks: the head's orientation, turned so the ray from
    // the hand meets the gaze kConvergeMeters ahead, then tilted up to cancel
    // the model's downward-pointing tip.
    constexpr float kDeg = 3.14159265f / 180.f;
    float reach = std::fmax(0.3f, kConvergeMeters + forward);
    // rot_ is the user's own twist (rotate mode): yaw, pitch, roll.
    Orientation aim = Mul(q, AxisAngle(0, 1, 0, std::atan2(side, reach) + rot_[0]));
    aim = Mul(aim, AxisAngle(1, 0, 0, std::atan2(down, reach) + rot_[1]));
    aim = Mul(aim, AxisAngle(0, 0, 1, rot_[2]));
    aim = Mul(aim, AxisAngle(1, 0, 0, kTipPitchDeg * kDeg));
    pose.qRotation.w = aim.w;
    pose.qRotation.x = aim.x;
    pose.qRotation.y = aim.y;
    pose.qRotation.z = aim.z;
    // Position: turning with the head's yaw only, so the hands don't swing up
    // and down when looking up and down.
    // Forward direction of the head (-Z rotated), flattened onto the floor.
    float fx = -2.f * (q.x * q.z + q.w * q.y);
    float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-3f) {
        fx = 0.f;
        fz = -1.f;
    } else {
        fx /= len;
        fz /= len;
    }
    // Right of forward on the floor: (-fz, 0, fx).
    pose.vecPosition[0] = fx * forward + (-fz) * side;
    pose.vecPosition[1] = headHeight_ - down;
    pose.vecPosition[2] = fz * forward + fx * side;

    // Off while the SteamVR dashboard is open: the gaze pointer takes over
    // there, aiming exactly where you look.
    bool active = connected_ && !dashboardOpen_;
    pose.poseIsValid = active;
    pose.result = active ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.deviceIsConnected = active;
    return pose;
}

void TouchDevice::Update(const Orientation &head, const GamepadState &rawPad) {
    // Tap View/Select to cycle: off, move hands, rotate hands, off. (Holding
    // it doesn't work: the pad sends Android's Back key, and a long Back
    // opens the Oculus menu.) In move or rotate mode, this side's controls
    // adjust the hand instead of reaching the game:
    //   move:   stick = sideways / up-down, trigger = push away, grip = pull closer
    //   rotate: stick = turn left-right / tilt up-down, trigger / grip = roll
    //   stick click resets that hand's position (move) or rotation (rotate).
    GamepadState pad = rawPad;
    bool select = pad.buttons & kPadSelect;
    if (select && !selectWasDown_)
        mode_ = (mode_ + 1) % 3;
    selectWasDown_ = select;
    if (!pad.connected())
        mode_ = kModeOff;
    auto now = std::chrono::steady_clock::now();
    float dt = std::fmin(0.1f, std::chrono::duration<float>(now - lastUpdate_).count());
    lastUpdate_ = now;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = head;
        connected_ = pad.connected();
        float sx = Dead(left_ ? pad.lx : pad.rx), sy = Dead(left_ ? pad.ly : pad.ry);
        float trigger = Dead(left_ ? pad.lt : pad.rt);
        float grip = (pad.buttons & (left_ ? kPadLB : kPadRB)) ? 1.f : 0.f;
        bool reset = pad.buttons & (left_ ? kPadLS : kPadRS);
        auto clamp = [](float v, float lo, float hi) { return std::fmax(lo, std::fmin(hi, v)); };
        if (mode_ == kModeMove) {
            constexpr float kSpeed = 0.4f; // m/s at full deflection
            offset_[0] = clamp(offset_[0] + sx * kSpeed * dt, -0.5f, 0.5f);
            offset_[1] = clamp(offset_[1] + sy * kSpeed * dt, -0.4f, 0.7f);
            offset_[2] = clamp(offset_[2] + (trigger - grip) * kSpeed * dt, -0.25f, 0.5f);
            if (reset)
                offset_[0] = offset_[1] = offset_[2] = 0.f;
        } else if (mode_ == kModeRotate) {
            constexpr float kTurn = 1.6f; // rad/s (~90 degrees/s) at full deflection
            constexpr float kMax = 1.6f;
            rot_[0] = clamp(rot_[0] - sx * kTurn * dt, -kMax, kMax); // stick right turns right
            rot_[1] = clamp(rot_[1] + sy * kTurn * dt, -kMax, kMax);
            rot_[2] = clamp(rot_[2] + (grip - trigger) * kTurn * dt, -kMax, kMax);
            if (reset)
                rot_[0] = rot_[1] = rot_[2] = 0.f;
        }
    }
    if (mode_ != loggedMode_) {
        static const char *names[] = {"off", "move hands", "rotate hands"};
        char msg[64];
        std::snprintf(msg, sizeof(msg), "lumen: hand mode: %s", names[mode_]);
        if (!left_)
            vr::VRDriverLog()->Log(msg); // once, not per hand
        loggedMode_ = mode_;
    }
    if (mode_ != kModeOff) {
        // None of this side's controls reach the game while adjusting hands.
        if (left_) {
            pad.lx = pad.ly = pad.lt = 0.f;
            pad.buttons &= ~(kPadLB | kPadLS);
        } else {
            pad.rx = pad.ry = pad.rt = 0.f;
            pad.buttons &= ~(kPadRB | kPadRS);
        }
    }
    if (objectId_ == vr::k_unTrackedDeviceIndexInvalid)
        return;
    vr::DriverPose_t pose = GetPose();
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, pose, sizeof(pose));

    auto *input = vr::VRDriverInput();
    auto set = [&](vr::VRInputComponentHandle_t h, bool v) { input->UpdateBooleanComponent(h, v, 0); };
    auto setf = [&](vr::VRInputComponentHandle_t h, float v) { input->UpdateScalarComponent(h, v, 0); };

    float sx = Dead(left_ ? pad.lx : pad.rx), sy = Dead(left_ ? pad.ly : pad.ry);
    float trigger = Dead(left_ ? pad.lt : pad.rt);
    bool grip = pad.buttons & (left_ ? kPadLB : kPadRB);
    bool stickClick = pad.buttons & (left_ ? kPadLS : kPadRS);
    bool button1 = pad.buttons & (left_ ? kPadX : kPadA);
    bool button2 = pad.buttons & (left_ ? kPadY : kPadB);

    setf(in_.joystickX, sx);
    setf(in_.joystickY, sy);
    set(in_.joystickTouch, sx != 0.f || sy != 0.f || stickClick);
    set(in_.joystickClick, stickClick);
    setf(in_.triggerValue, trigger);
    set(in_.triggerTouch, trigger > 0.f);
    set(in_.triggerClick, trigger > 0.7f);
    setf(in_.gripValue, grip ? 1.f : 0.f);
    set(in_.gripTouch, grip);
    set(in_.gripClick, grip);
    set(in_.button1Click, button1);
    set(in_.button1Touch, button1);
    set(in_.button2Click, button2);
    set(in_.button2Touch, button2);
    // Thumb resting on the controller (games use it for hand poses).
    set(in_.thumbrestTouch, !(sx != 0.f || sy != 0.f || button1 || button2));
    if (left_) {
        bool menu = pad.buttons & (kPadStart | kPadGuide);
        set(in_.systemClick, menu);
        set(in_.systemTouch, menu);
    }
}

} // namespace lumen
