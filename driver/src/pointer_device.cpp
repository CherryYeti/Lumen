#include "pointer_device.h"

#include <cmath>

namespace lumen {

namespace {

// Rotates v by unit quaternion q.
void Rotate(const Orientation &q, const float v[3], float out[3]) {
    // t = 2 * cross(q.xyz, v); out = v + w * t + cross(q.xyz, t)
    float tx = 2.f * (q.y * v[2] - q.z * v[1]);
    float ty = 2.f * (q.z * v[0] - q.x * v[2]);
    float tz = 2.f * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

} // namespace

vr::EVRInitError PointerDevice::Activate(uint32_t objectId) {
    objectId_ = objectId;
    auto *props = vr::VRProperties();
    vr::PropertyContainerHandle_t container = props->TrackedDeviceToPropertyContainer(objectId_);

    // Look like a Vive wand so SteamVR's existing bindings apply.
    props->SetStringProperty(container, vr::Prop_ControllerType_String, "vive_controller");
    props->SetStringProperty(container, vr::Prop_InputProfilePath_String, "{htc}/input/vive_controller_profile.json");
    props->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand);
    props->SetStringProperty(container, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5");
    props->SetStringProperty(container, vr::Prop_ModelNumber_String, "Lumen Gaze Pointer");
    props->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Lumen");
    props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "lumen");

    auto *input = vr::VRDriverInput();
    input->CreateBooleanComponent(container, "/input/trigger/click", &triggerClick_);
    input->CreateScalarComponent(container, "/input/trigger/value", &triggerValue_, vr::VRScalarType_Absolute,
                                 vr::VRScalarUnits_NormalizedOneSided);
    input->CreateBooleanComponent(container, "/input/system/click", &systemClick_);
    input->CreateBooleanComponent(container, "/input/trackpad/touch", &padTouch_);
    input->CreateScalarComponent(container, "/input/trackpad/x", &padX_, vr::VRScalarType_Absolute,
                                 vr::VRScalarUnits_NormalizedTwoSided);
    input->CreateScalarComponent(container, "/input/trackpad/y", &padY_, vr::VRScalarType_Absolute,
                                 vr::VRScalarUnits_NormalizedTwoSided);
    // The rest of a Vive wand's inputs exist but never change.
    vr::VRInputComponentHandle_t unused;
    for (const char *path :
         {"/input/trackpad/click", "/input/grip/click", "/input/application_menu/click"})
        input->CreateBooleanComponent(container, path, &unused);
    input->CreateHapticComponent(container, "/output/haptic", &unused);

    vr::VRDriverLog()->Log("lumen: gaze pointer activated");
    return vr::VRInitError_None;
}

void PointerDevice::DebugRequest(const char *request, char *responseBuffer, uint32_t responseBufferSize) {
    if (responseBufferSize > 0)
        responseBuffer[0] = '\0';
}

vr::DriverPose_t PointerDevice::GetPose() {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;

    std::lock_guard<std::mutex> lock(mutex_);
    pose.qRotation.w = head_.w;
    pose.qRotation.x = head_.x;
    pose.qRotation.y = head_.y;
    pose.qRotation.z = head_.z;
    // Behind the eyes along the gaze line: invisible, laser through the centre.
    const float back[3] = {0.f, 0.f, kBehindHead};
    float offset[3];
    Rotate(head_, back, offset);
    pose.vecPosition[0] = offset[0];
    pose.vecPosition[1] = headHeight_ + offset[1];
    pose.vecPosition[2] = offset[2];

    // Steps aside while a gamepad drives the virtual Touch controllers, so
    // games don't see a third, competing right hand; but takes over while the
    // SteamVR dashboard is open, since its gaze aim is exact at any distance.
    bool active = !input_.gamepad.connected() || dashboardOpen_;
    pose.poseIsValid = active;
    pose.result = active ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.deviceIsConnected = active;
    return pose;
}

PoseInput PointerDevice::WithGamepad(const PoseInput &raw) {
    // While the dashboard is open, the gamepad drives this pointer: RT or A
    // clicks, the right stick scrolls, Start/Guide closes the dashboard.
    PoseInput in = raw;
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastUpdate_).count();
    lastUpdate_ = now;
    if (dt > 0.1f)
        dt = 0.1f;
    const GamepadState &pad = raw.gamepad;
    if (!pad.connected() || !dashboardOpen_) {
        menuArmed_ = false;
        scrollTouching_ = false;
        return in;
    }
    if (pad.rt > 0.5f || (pad.buttons & kPadA))
        in.buttons |= kButtonTrigger;
    // The press that opened the dashboard is still held: wait for a release
    // before Start/Guide may close it again.
    bool menu = pad.buttons & (kPadStart | kPadGuide);
    if (!menu)
        menuArmed_ = true;
    if (menu && menuArmed_)
        in.buttons |= kButtonSystem;
    // SteamVR scrolls by finger movement on the Vive trackpad, so turn a held
    // stick into a finger sliding across the pad, lifting and starting over
    // at the edge.
    float sx = std::fabs(pad.rx) > 0.15f ? pad.rx : 0.f, sy = std::fabs(pad.ry) > 0.15f ? pad.ry : 0.f;
    if (sx != 0.f || sy != 0.f) {
        if (!scrollTouching_ || std::fabs(scrollX_) > 0.9f || std::fabs(scrollY_) > 0.9f) {
            if (scrollTouching_) {
                scrollTouching_ = false; // lift for one update at the edge
            } else {
                scrollTouching_ = true;
                scrollX_ = scrollY_ = 0.f;
            }
        } else {
            scrollX_ += sx * dt * 2.5f;
            scrollY_ += sy * dt * 2.5f;
        }
    } else {
        scrollTouching_ = false;
    }
    if (scrollTouching_) {
        in.buttons |= kButtonTrackpadTouch;
        in.padX = scrollX_;
        in.padY = scrollY_;
    }
    return in;
}

void PointerDevice::Update(const Orientation &head, const PoseInput &raw) {
    PoseInput previous;
    PoseInput input;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = head;
        previous = input_;
        input = WithGamepad(raw);
        input_ = input;
    }
    if (objectId_ == vr::k_unTrackedDeviceIndexInvalid)
        return;
    vr::DriverPose_t pose = GetPose();
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, pose, sizeof(pose));

    auto *driverInput = vr::VRDriverInput();
    uint32_t changed = input.buttons ^ previous.buttons;
    if (changed & kButtonTrigger) {
        bool pressed = input.buttons & kButtonTrigger;
        driverInput->UpdateBooleanComponent(triggerClick_, pressed, 0);
        driverInput->UpdateScalarComponent(triggerValue_, pressed ? 1.f : 0.f, 0);
    }
    if (changed & kButtonSystem)
        driverInput->UpdateBooleanComponent(systemClick_, input.buttons & kButtonSystem, 0);
    // Trackpad: position first, then touch, so a new touch starts from the
    // right place (SteamVR scrolls by how the position moves while touched).
    if (input.padX != previous.padX)
        driverInput->UpdateScalarComponent(padX_, input.padX, 0);
    if (input.padY != previous.padY)
        driverInput->UpdateScalarComponent(padY_, input.padY, 0);
    if (changed & kButtonTrackpadTouch)
        driverInput->UpdateBooleanComponent(padTouch_, input.buttons & kButtonTrackpadTouch, 0);
}

} // namespace lumen
