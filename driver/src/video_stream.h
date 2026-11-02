#pragma once

// Video over UDP, PC -> phone. Sent by lumen-capture; the Android client
// implements the same format. All integers little-endian.
//
// Each encoded frame (H.264/HEVC Annex-B access unit, SPS/PPS/VPS in-band on
// keyframes) is split into datagrams of at most kMaxDatagram bytes:
//
//   VideoPacketHeader (68 bytes) + payload
//
// Every packet also carries the head orientation SteamVR rendered the frame
// with, the per-eye field of view, the part of each eye's image that holds
// the picture (games may render into less than the whole texture) and the
// encoded frame size, so a headset client can reproject the frame to its
// current head pose (e.g. VrApi TimeWarp) and map it correctly. The frame size
// can change mid-stream (a game starting); the change starts with a keyframe.
//
// A receiver reassembles by frameId and drops a frame if any packet is
// missing. After a dropped frame the decoder's references are broken, so it
// sends a KeyframeRequest back to the address the video came from; the
// sender then forces the next frame to be an IDR.

#include <cstdint>

namespace lumen {

constexpr uint16_t kVideoPort = 9945;
constexpr int kMaxDatagram = 1400; // stays under a 1500-byte MTU with IP/UDP headers

enum VideoFlags : uint8_t {
    kVideoKeyframe = 1 << 0,
    kVideoHevc = 1 << 1, // otherwise H.264
};

#pragma pack(push, 1)
struct VideoPacketHeader {
    char magic[4];          // "LMV3"
    uint32_t frameId;       // increases by 1 per sent frame (gaps = sender skipped)
    uint16_t packetIndex;   // 0 .. packetCount-1
    uint16_t packetCount;   // packets in this frame
    uint8_t flags;          // VideoFlags
    uint8_t reserved[3];
    uint64_t captureTimeUs; // sender's steady clock when the frame was read; for latency stats
    float renderPose[4];    // w, x, y, z: head orientation the frame was rendered with (OpenVR frame)
    float tanHalfFov[2];    // x, y: tan of half the per-eye field of view (symmetric frustum)
    float bounds[4];        // uMin, vMin, uMax, vMax: picture area within each eye's half, 0..1, v down
    uint16_t width, height; // encoded frame size (both eyes side by side)
};

// Receiver -> sender, same UDP socket pair.
struct KeyframeRequest {
    char magic[4]; // "LMK1"
};
#pragma pack(pop)

static_assert(sizeof(VideoPacketHeader) == 68, "VideoPacketHeader must be 68 bytes");
constexpr int kMaxPayload = kMaxDatagram - int(sizeof(VideoPacketHeader));

} // namespace lumen
