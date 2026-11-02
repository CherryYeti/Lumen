#include "device_provider.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace lumen {

namespace {

constexpr const char *kSection = "driver_lumen";

std::string GetSettingString(const char *key, const char *fallback) {
    char buffer[256];
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(kSection, key, buffer, sizeof(buffer), &error);
    return error == vr::VRSettingsError_None ? buffer : fallback;
}

DisplaySettings LoadDisplaySettings() {
    auto *settings = vr::VRSettings();
    DisplaySettings d;
    d.windowX = settings->GetInt32(kSection, "windowX");
    d.windowY = settings->GetInt32(kSection, "windowY");
    d.windowWidth = settings->GetInt32(kSection, "windowWidth");
    d.windowHeight = settings->GetInt32(kSection, "windowHeight");
    d.renderWidth = settings->GetInt32(kSection, "renderWidth");
    d.renderHeight = settings->GetInt32(kSection, "renderHeight");
    d.fovDegrees = settings->GetFloat(kSection, "fovDegrees");
    d.ipdMeters = settings->GetFloat(kSection, "ipdMeters");
    d.headHeightMeters = settings->GetFloat(kSection, "headHeightMeters");
    d.displayFrequency = settings->GetFloat(kSection, "displayFrequency");
    d.secondsFromVsyncToPhotons = settings->GetFloat(kSection, "secondsFromVsyncToPhotons");
    return d;
}

} // namespace

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext *driverContext) {
    VR_INIT_SERVER_DRIVER_CONTEXT(driverContext);

    hmd_ = std::make_unique<HmdDevice>(GetSettingString("serialNumber", "LUMEN-0001"),
                                       GetSettingString("modelNumber", "Lumen Phone HMD"),
                                       LoadDisplaySettings());

    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(hmd_->Serial().c_str(), vr::TrackedDeviceClass_HMD,
                                                      hmd_.get())) {
        vr::VRDriverLog()->Log("lumen: failed to add HMD");
        return vr::VRInitError_Driver_Unknown;
    }

    int port = vr::VRSettings()->GetInt32(kSection, "posePort");
    // Head-following pointer so a controller-less headset can click (see
    // pointer_device.h). Off with "gazePointer": false.
    vr::EVRSettingsError pointerError = vr::VRSettingsError_None;
    bool gazePointer = vr::VRSettings()->GetBool(kSection, "gazePointer", &pointerError);
    if (pointerError != vr::VRSettingsError_None || gazePointer) {
        pointer_ = std::make_unique<PointerDevice>(vr::VRSettings()->GetFloat(kSection, "headHeightMeters"));
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(PointerDevice::kSerial, vr::TrackedDeviceClass_Controller,
                                                          pointer_.get()))
            vr::VRDriverLog()->Log("lumen: failed to add gaze pointer");
    }

    float headHeight = vr::VRSettings()->GetFloat(kSection, "headHeightMeters");
    touch_[0] = std::make_unique<TouchDevice>(true, headHeight);
    touch_[1] = std::make_unique<TouchDevice>(false, headHeight);

    HmdDevice *hmd = hmd_.get();
    PointerDevice *pointer = pointer_.get();
    if (poseReceiver_.Start(static_cast<uint16_t>(port), [this, hmd, pointer](const Orientation &o,
                                                                              const PoseInput &input) {
            hmd->UpdateOrientation(o);
            if (pointer)
                pointer->Update(o, input);
            if (input.gamepad.connected())
                gamepadSeen_ = true; // RunFrame adds the Touch controllers
            for (auto &touch : touch_)
                touch->Update(o, input.gamepad);
        },
        [hmd](float level, bool charging) { hmd->UpdateBattery(level, charging); },
        [](const std::string &line) {
            // Printable text only, so a stray packet can't mangle the log.
            std::string clean;
            for (char c : line)
                clean += (c >= 32 && c < 127) ? c : '?';
            vr::VRDriverLog()->Log(("lumen: phone: " + clean).c_str());
        },
        [this](uint32_t eyeWidth, uint32_t eyeHeight, float fovDegrees) {
            std::lock_guard<std::mutex> lock(displayMutex_);
            displayPending_ = true;
            phoneEyeWidth_ = eyeWidth;
            phoneEyeHeight_ = eyeHeight;
            phoneFov_ = fovDegrees;
        })) {
        vr::VRDriverLog()->Log(("lumen: listening for poses on UDP " + std::to_string(port)).c_str());
    } else {
        vr::VRDriverLog()->Log(("lumen: could not bind UDP " + std::to_string(port)).c_str());
    }

    return vr::VRInitError_None;
}

void DeviceProvider::Cleanup() {
    poseReceiver_.Stop();
    pointer_.reset();
    touch_[0].reset();
    touch_[1].reset();
    hmd_.reset();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

// The phone said what it would like rendered (LMD1). SteamVR only reads the
// render size and FOV at startup, so when they differ from what's running,
// save them to the driver's settings (SteamVR keeps them in
// steamvr.vrsettings) and ask SteamVR to restart, once per change.
void DeviceProvider::ApplyPhoneDisplay() {
    uint32_t eyeWidth, eyeHeight;
    float fov;
    {
        std::lock_guard<std::mutex> lock(displayMutex_);
        if (!displayPending_)
            return;
        displayPending_ = false;
        eyeWidth = phoneEyeWidth_;
        eyeHeight = phoneEyeHeight_;
        fov = phoneFov_;
    }
    uint32_t renderWidth = static_cast<uint32_t>(eyeWidth / kOutputScale + 0.5f);
    uint32_t renderHeight = static_cast<uint32_t>(eyeHeight / kOutputScale + 0.5f);
    fov = std::round(fov * 10.f) / 10.f;
    const DisplaySettings &current = hmd_->Display();
    if (renderWidth == current.renderWidth && renderHeight == current.renderHeight &&
        std::fabs(fov - current.fovDegrees) < 0.05f)
        return;
    char wanted[128];
    std::snprintf(wanted, sizeof(wanted), "render %ux%u (eyes %ux%u), FOV %.1f", renderWidth, renderHeight, eyeWidth,
                  eyeHeight, fov);
    if (restartRequestedFor_ == wanted)
        return;
    restartRequestedFor_ = wanted;
    auto *settings = vr::VRSettings();
    settings->SetInt32(kSection, "renderWidth", static_cast<int32_t>(renderWidth));
    settings->SetInt32(kSection, "renderHeight", static_cast<int32_t>(renderHeight));
    settings->SetFloat(kSection, "fovDegrees", fov);
    vr::VRDriverLog()->Log((std::string("lumen: phone wants ") + wanted + "; saved, asking SteamVR to restart").c_str());
    vr::VRServerDriverHost()->RequestRestart("Lumen: the phone needs a different resolution. Restart SteamVR to apply it.",
                                             "", "", "");
}

void DeviceProvider::RunFrame() {
    ApplyPhoneDisplay();
    // Only show the Touch controllers once a gamepad is actually in use; then
    // they stay (reporting disconnected while the gamepad is gone).
    if (gamepadSeen_ && !touchAdded_) {
        touchAdded_ = true;
        for (auto &touch : touch_)
            if (!vr::VRServerDriverHost()->TrackedDeviceAdded(touch->Serial(), vr::TrackedDeviceClass_Controller,
                                                              touch.get()))
                vr::VRDriverLog()->Log("lumen: failed to add gamepad Touch controller");
    }
    vr::VREvent_t event;
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Input_HapticVibration) {
            // A game vibrating a gamepad Touch controller: pass it to the phone,
            // which rumbles the pad if Android can drive its motors.
            const auto &h = event.data.hapticVibration;
            for (uint32_t hand = 0; hand < 2; hand++) {
                if (!touch_[hand] || h.componentHandle != touch_[hand]->HapticHandle())
                    continue;
                HapticPacket packet{{'L', 'M', 'H', '1'}, hand, h.fDurationSeconds, h.fFrequency, h.fAmplitude};
                poseReceiver_.SendToPhone(&packet, sizeof(packet));
            }
            continue;
        }
        if (event.eventType == vr::VREvent_DashboardActivated || event.eventType == vr::VREvent_DashboardDeactivated) {
            bool open = event.eventType == vr::VREvent_DashboardActivated;
            vr::VRDriverLog()->Log(open ? "lumen: dashboard opened" : "lumen: dashboard closed");
            // Hand the dashboard to the gaze pointer (exact aim); without one,
            // the Touch controllers stay.
            if (pointer_) {
                pointer_->SetDashboardOpen(open);
                for (auto &touch : touch_)
                    touch->SetDashboardOpen(open);
            }
        }
    }
}

} // namespace lumen
