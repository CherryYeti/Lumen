#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "hmd_device.h"
#include "pointer_device.h"
#include "touch_device.h"
#include "pose_receiver.h"

namespace lumen {

class DeviceProvider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *driverContext) override;
    void Cleanup() override;
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    std::unique_ptr<HmdDevice> hmd_;
    std::unique_ptr<PointerDevice> pointer_;
    // Gamepad Touch controllers: created when the phone first reports a gamepad.
    std::unique_ptr<TouchDevice> touch_[2];
    std::atomic<bool> gamepadSeen_{false};
    bool touchAdded_ = false;
    PoseReceiver poseReceiver_;

    // The phone's requested display (LMD1), applied in RunFrame.
    void ApplyPhoneDisplay();
    std::mutex displayMutex_;
    bool displayPending_ = false;
    uint32_t phoneEyeWidth_ = 0, phoneEyeHeight_ = 0;
    float phoneFov_ = 0.f;
    std::string restartRequestedFor_; // settings a restart was already asked for
};

} // namespace lumen
