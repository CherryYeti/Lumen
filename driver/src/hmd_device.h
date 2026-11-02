#pragma once

#include <openvr_driver.h>

#include <mutex>
#include <string>

#include "direct_mode.h"
#include "pose_receiver.h"

namespace lumen {

struct DisplaySettings {
    int32_t windowX = 0, windowY = 0;
    uint32_t windowWidth = 1920, windowHeight = 1080;
    uint32_t renderWidth = 1200, renderHeight = 1350; // per eye
    float fovDegrees = 95.f;
    float ipdMeters = 0.063f;
    float headHeightMeters = 1.6f;
    float displayFrequency = 60.f;
    float secondsFromVsyncToPhotons = 0.011f;
};

// SteamVR sizes the direct-mode output (each eye's image) at this times the
// render size it's asked for (observed; supersampling doesn't change it).
constexpr float kOutputScale = 1.5f;

// Per-eye tan(half field of view), symmetric frustum.
void TanHalfFov(const DisplaySettings &display, float &tanX, float &tanY);

// The phone, as SteamVR sees it: an HMD with no physical display. The
// compositor hands each finished frame to the driver (driver direct mode)
// instead of scanning out to a monitor, and rotation comes from the phone's
// gyro. Lens distortion is left to the phone, so frames hold plain
// side-by-side eye images.
class HmdDevice : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
public:
    HmdDevice(std::string serial, std::string model, const DisplaySettings &display);

    const std::string &Serial() const { return serial_; }
    const DisplaySettings &Display() const { return display_; }
    void UpdateOrientation(const Orientation &orientation);
    // The phone's battery, shown by SteamVR as the headset's.
    void UpdateBattery(float level, bool charging);

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t objectId) override;
    void Deactivate() override;
    void EnterStandby() override {}
    void *GetComponent(const char *componentNameAndVersion) override;
    void DebugRequest(const char *request, char *responseBuffer, uint32_t responseBufferSize) override;
    vr::DriverPose_t GetPose() override;

    // IVRDisplayComponent
    void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) override;
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override;
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) override;
    void GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eye, float u, float v) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye eye, uint32_t channel, float u, float v) override;

private:
    std::string serial_;
    std::string model_;
    DisplaySettings display_;
    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;

    std::mutex poseMutex_;
    Orientation orientation_;

    DirectModeComponent directMode_;
};

} // namespace lumen
