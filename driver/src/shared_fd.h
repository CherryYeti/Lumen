#pragma once

#include <cstdint>

namespace lumen {

// Fetches the file descriptor behind a SteamVR shared resource.
//
// Undocumented protocol, worked out by observation (SteamVR 2.17.10):
// IVRIPCResourceManagerClient::RefResource(handle, &ipcHandle) makes SteamVR
// open a one-shot abstract datagram socket "\0fd-srv-<ipcHandle>" (decimal,
// NUL-padded to the full sun_path). A client binds "\0fd-cl-<ipcHandle>",
// sends any datagram to it and gets back the 8 bytes "VALVE\0vk" with the
// fd attached as SCM_RIGHTS. For images the fd is Vulkan OPAQUE_FD memory.
//
// Returns the fd (caller owns it) or -1.
int ReceiveSharedFd(uint64_t ipcHandle);

} // namespace lumen
