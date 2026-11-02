#include <openvr_driver.h>

#include <cstring>

#include "device_provider.h"

static lumen::DeviceProvider g_provider;

// Entry point SteamVR looks up in driver_lumen.so.
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *interfaceName, int *returnCode) {
    if (std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0)
        return &g_provider;

    if (returnCode)
        *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
