#pragma once

// Audio over UDP, PC -> phone: what the PC plays (SteamVR and games), as raw
// PCM. Sent by lumen-capture to the same host as the video; the Gear VR app
// plays it. All integers little-endian.
//
// Each datagram is AudioPacketHeader + kAudioFramesPerPacket frames of
// 16-bit signed stereo at kAudioRate (left, right interleaved). Raw PCM is
// ~1.5 Mbit/s, a few percent of the video, and needs no codec on either end.
// A lost packet is 5 ms of silence; the receiver uses the sequence number to
// drop late, reordered packets.

#include <cstdint>

namespace lumen {

constexpr uint16_t kAudioPort = 9946;
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioFramesPerPacket = 240; // 5 ms
constexpr int kAudioPayloadBytes = kAudioFramesPerPacket * kAudioChannels * 2;

#pragma pack(push, 1)
struct AudioPacketHeader {
    char magic[4];     // "LMA1"
    uint32_t sequence; // +1 per packet
};
#pragma pack(pop)

static_assert(sizeof(AudioPacketHeader) == 8, "AudioPacketHeader must be 8 bytes");

} // namespace lumen
