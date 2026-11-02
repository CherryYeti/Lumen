#pragma once

// Messages from the driver (inside vrserver) to the lumen-capture helper.
// The helper owns all Vulkan imports so vrserver never holds a foreign
// allocation (doing so made the compositor exit).
//
// Transport: abstract SOCK_SEQPACKET Unix socket kHelperSocketName, one
// HelperMessage per packet. kImport packets carry the image's memory fd as
// SCM_RIGHTS.

#include <cstdint>

namespace lumen {

constexpr char kHelperSocketName[] = "lumen-capture"; // abstract: "\0lumen-capture"

enum HelperMessageType : uint32_t {
    kImport = 1, // handle[0] = shared texture; width/height/format describe it; fd attached
    kForget = 2, // handle[0] = shared texture to drop
    kFrame = 3,  // handle[0..1] = left/right textures of a finished frame, plus pose and FOV
};

struct HelperMessage {
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t format; // VkFormat
    uint64_t handle[2];
    uint64_t frame;
    float pose[4];       // kFrame: w, x, y, z head orientation the frame was rendered with
    float tanHalfFov[2]; // kFrame: per-eye tan(half FOV), x and y
    float bounds[4];     // kFrame: uMin, vMin, uMax, vMax of the picture within each eye texture
    uint32_t maxEye;     // kFrame: the headset's eye size (longest side, px); stream eyes are capped to it
};

} // namespace lumen
