#include "hmd_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace lumen {

HmdDevice::HmdDevice(std::string serial, std::string model, const DisplaySettings &display)
    : serial_(std::move(serial)), model_(std::move(model)), display_(display),
      directMode_(display.displayFrequency) {
    float tanX, tanY;
    TanHalfFov(display_, tanX, tanY);
    directMode_.SetTanHalfFov(tanX, tanY);
    float maxEye = kOutputScale * std::max(display_.renderWidth, display_.renderHeight);
    directMode_.SetMaxEye(static_cast<uint32_t>(maxEye + 0.5f));
}

vr::EVRInitError HmdDevice::Activate(uint32_t objectId) {
    objectId_ = objectId;
    auto *props = vr::VRProperties();
    vr::PropertyContainerHandle_t container = props->TrackedDeviceToPropertyContainer(objectId_);

    props->SetStringProperty(container, vr::Prop_ModelNumber_String, model_.c_str());
    props->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Lumen");
    props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "lumen");
    props->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, display_.ipdMeters);
    props->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    props->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, display_.displayFrequency);
    props->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, display_.secondsFromVsyncToPhotons);
    props->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 2);
    props->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
    props->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
    props->SetBoolProperty(container, vr::Prop_HasDisplayComponent_Bool, true);
    // Battery: the phone reports its level (see UpdateBattery).
    props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);
    props->SetBoolProperty(container, vr::Prop_HasCameraComponent_Bool, false);
    props->SetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool, true);
    props->SetBoolProperty(container, vr::Prop_HasVirtualDisplayComponent_Bool, false);

    vr::VRDriverLog()->Log("lumen: HMD activated");
    return vr::VRInitError_None;
}

void HmdDevice::Deactivate() {
    objectId_ = vr::k_unTrackedDeviceIndexInvalid;
}

void *HmdDevice::GetComponent(const char *componentNameAndVersion) {
    std::string msg = std::string("lumen: GetComponent ") + componentNameAndVersion;
    void *component = nullptr;
    if (std::strcmp(componentNameAndVersion, vr::IVRDisplayComponent_Version) == 0)
        component = static_cast<vr::IVRDisplayComponent *>(this);
    else if (std::strcmp(componentNameAndVersion, vr::IVRDriverDirectModeComponent_Version) == 0)
        component = static_cast<vr::IVRDriverDirectModeComponent *>(&directMode_);
    vr::VRDriverLog()->Log((msg + (component ? " -> provided" : " -> not provided")).c_str());
    return component;
}

void HmdDevice::DebugRequest(const char *request, char *responseBuffer, uint32_t responseBufferSize) {
    if (responseBufferSize > 0)
        responseBuffer[0] = '\0';
}

vr::DriverPose_t HmdDevice::GetPose() {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;

    {
        std::lock_guard<std::mutex> lock(poseMutex_);
        pose.qRotation.w = orientation_.w;
        pose.qRotation.x = orientation_.x;
        pose.qRotation.y = orientation_.y;
        pose.qRotation.z = orientation_.z;
    }

    // Rotation-only tracking: the head stays at a fixed point.
    pose.vecPosition[1] = display_.headHeightMeters;

    pose.poseIsValid = true;
    pose.result = vr::TrackingResult_Running_OK;
    pose.deviceIsConnected = true;
    return pose;
}

void HmdDevice::UpdateOrientation(const Orientation &orientation) {
    {
        std::lock_guard<std::mutex> lock(poseMutex_);
        orientation_ = orientation;
    }
    if (objectId_ != vr::k_unTrackedDeviceIndexInvalid) {
        vr::DriverPose_t pose = GetPose();
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, pose, sizeof(pose));
    }
}

void HmdDevice::GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) {
    *x = display_.windowX;
    *y = display_.windowY;
    *width = display_.windowWidth;
    *height = display_.windowHeight;
}

void HmdDevice::GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) {
    *width = display_.renderWidth;
    *height = display_.renderHeight;
}

void HmdDevice::GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) {
    *width = display_.windowWidth / 2;
    *height = display_.windowHeight;
    *x = eye == vr::Eye_Left ? 0 : display_.windowWidth / 2;
    *y = 0;
}

void TanHalfFov(const DisplaySettings &display, float &tanX, float &tanY) {
    // fovDegrees is horizontal per eye; vertical follows the render target's
    // aspect, so pixels come out square.
    constexpr float kPi = 3.14159265f;
    tanX = std::tan(display.fovDegrees * 0.5f * kPi / 180.f);
    tanY = tanX * static_cast<float>(display.renderHeight) / static_cast<float>(display.renderWidth);
}

void HmdDevice::GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) {
    float tanX, tanY;
    TanHalfFov(display_, tanX, tanY);
    *left = -tanX;
    *right = tanX;
    *top = -tanY;
    *bottom = tanY;
}

vr::DistortionCoordinates_t HmdDevice::ComputeDistortion(vr::EVREye eye, float u, float v) {
    // No distortion here; the phone corrects for its lenses.
    vr::DistortionCoordinates_t coords{};
    coords.rfRed[0] = coords.rfGreen[0] = coords.rfBlue[0] = u;
    coords.rfRed[1] = coords.rfGreen[1] = coords.rfBlue[1] = v;
    return coords;
}

bool HmdDevice::ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye eye, uint32_t channel, float u, float v) {
    return false; // Not implemented; SteamVR falls back to solving it numerically.
}

void HmdDevice::UpdateBattery(float level, bool charging) {
    if (objectId_ == vr::k_unTrackedDeviceIndexInvalid)
        return;
    auto *props = vr::VRProperties();
    vr::PropertyContainerHandle_t container = props->TrackedDeviceToPropertyContainer(objectId_);
    props->SetFloatProperty(container, vr::Prop_DeviceBatteryPercentage_Float, level);
    props->SetBoolProperty(container, vr::Prop_DeviceIsCharging_Bool, charging);
}

} // namespace lumen
