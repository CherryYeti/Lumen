#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "pose_receiver.h"

namespace lumen {

// A virtual controller that follows the head, so a headset with no
// controllers (Gear VR) can point, click and scroll in SteamVR. It emulates a
// Vive wand, so SteamVR's own bindings for the dashboard and games apply as-is:
// its laser points where the user looks, a Gear VR touchpad tap is the
// trigger, a finger moving on the touchpad is the Vive trackpad (scroll), and
// the Gear VR back button is the system button (opens/closes the dashboard).
//
// It sits behind the eyes (kBehindHead) so its model stays out of view while
// its laser still passes through the centre of the gaze.
class PointerDevice : public vr::ITrackedDeviceServerDriver {
public:
    explicit PointerDevice(float headHeightMeters) : headHeight_(headHeightMeters) {}

    static constexpr const char *kSerial = "LUMEN-POINTER";

    void Update(const Orientation &head, const PoseInput &input);
    // The SteamVR dashboard opened/closed (from DeviceProvider::RunFrame).
    void SetDashboardOpen(bool open) { dashboardOpen_ = open; }

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t objectId) override;
    void Deactivate() override { objectId_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void *GetComponent(const char *componentNameAndVersion) override { return nullptr; }
    void DebugRequest(const char *request, char *responseBuffer, uint32_t responseBufferSize) override;
    vr::DriverPose_t GetPose() override;

private:
    static constexpr float kBehindHead = 0.25f; // meters, along the head's +Z

    float headHeight_;
    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    std::mutex mutex_;
    Orientation head_;
    PoseInput input_; // effective input, gamepad merged in
    std::atomic<bool> dashboardOpen_{false};
    bool menuArmed_ = false, scrollTouching_ = false;
    float scrollX_ = 0.f, scrollY_ = 0.f;
    std::chrono::steady_clock::time_point lastUpdate_ = std::chrono::steady_clock::now();

    PoseInput WithGamepad(const PoseInput &raw);

    vr::VRInputComponentHandle_t triggerClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t triggerValue_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t systemClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t padTouch_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t padX_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t padY_ = vr::k_ulInvalidInputComponentHandle;
};

} // namespace lumen
