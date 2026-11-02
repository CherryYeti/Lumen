#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "pose_receiver.h"

namespace lumen {

// One of two virtual Oculus Touch controllers driven by a gamepad paired with
// the phone. Touch's layout matches an Xbox pad almost 1:1, and SteamVR games
// ship Touch bindings, so games work without custom binding files:
//
//   left:  left stick, LT trigger, LB grip, X, Y, Start/Guide = menu (dashboard)
//   right: right stick, RT trigger, RB grip, A, B
//
// A gamepad has no hand tracking, so the hands sit at spots in front of the
// body (following the head's yaw) and point where the head looks, like a gaze
// pointer held at waist height. Tap View/Select to cycle hand-move and
// hand-rotate modes (see Update).
class TouchDevice : public vr::ITrackedDeviceServerDriver {
public:
    TouchDevice(bool left, float headHeightMeters) : left_(left), headHeight_(headHeightMeters) {}

    const char *Serial() const { return left_ ? "LUMEN-TOUCH-LEFT" : "LUMEN-TOUCH-RIGHT"; }
    void Update(const Orientation &head, const GamepadState &pad);
    // The SteamVR dashboard opened/closed (from DeviceProvider::RunFrame).
    void SetDashboardOpen(bool open) { dashboardOpen_ = open; }
    // The haptic output games vibrate (VREvent_Input_HapticVibration names it).
    vr::VRInputComponentHandle_t HapticHandle() const { return haptic_; }

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t objectId) override;
    void Deactivate() override { objectId_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void *GetComponent(const char *componentNameAndVersion) override { return nullptr; }
    void DebugRequest(const char *request, char *responseBuffer, uint32_t responseBufferSize) override;
    vr::DriverPose_t GetPose() override;

private:
    struct Inputs {
        vr::VRInputComponentHandle_t joystickClick, joystickTouch, joystickX, joystickY;
        vr::VRInputComponentHandle_t triggerValue, triggerTouch, triggerClick;
        vr::VRInputComponentHandle_t gripValue, gripTouch, gripClick;
        vr::VRInputComponentHandle_t button1Click, button1Touch, button2Click, button2Touch; // A/B or X/Y
        vr::VRInputComponentHandle_t systemClick, systemTouch; // left only
        vr::VRInputComponentHandle_t thumbrestTouch;
    };

    bool left_;
    float headHeight_;
    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    std::mutex mutex_;
    Orientation head_;
    bool connected_ = false;
    Inputs in_{};
    vr::VRInputComponentHandle_t haptic_ = vr::k_ulInvalidInputComponentHandle;
    std::atomic<bool> dashboardOpen_{false};
    float offset_[3] = {0, 0, 0}; // hand moved by the user: right, up, forward (m)
    float rot_[3] = {0, 0, 0}; // hand rotated by the user: yaw, pitch, roll (rad)
    enum { kModeOff, kModeMove, kModeRotate };
    int mode_ = kModeOff, loggedMode_ = kModeOff;
    bool selectWasDown_ = false;
    std::chrono::steady_clock::time_point lastUpdate_ = std::chrono::steady_clock::now();
};

} // namespace lumen
