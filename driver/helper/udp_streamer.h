#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>

namespace lumen {

// Resolves "host" or "host:port" (IPv4) into an address.
bool ResolveTarget(const std::string &target, uint16_t defaultPort, sockaddr_in &out);

// Sends encoded frames to one receiver in the video_stream.h format and
// listens on the same socket for keyframe requests.
class UdpStreamer {
public:
    ~UdpStreamer();

    // target: "host" or "host:port" (default port kVideoPort).
    bool Open(const std::string &target, bool hevc);
    // renderPose: w, x, y, z; tanHalfFov: x, y; bounds: uMin, vMin, uMax, vMax;
    // width, height: encoded frame size (see video_stream.h).
    void SendFrame(const uint8_t *data, size_t size, bool keyframe, uint64_t captureTimeUs,
                   const float renderPose[4], const float tanHalfFov[2], const float bounds[4], uint32_t width,
                   uint32_t height);
    // Also send each encoded frame, as a plain H.264/HEVC byte stream, to this
    // UDP port on localhost (the Lumen app's video window plays it).
    void SetMirror(uint16_t port);
    // True if a keyframe request arrived since the last call.
    bool TakeKeyframeRequest();

    uint64_t FramesSent() const { return framesSent_; }
    uint64_t BytesSent() const { return bytesSent_; }
    uint64_t SendErrors() const { return sendErrors_; }
    uint64_t KeyframeRequests() const { return keyframeRequests_; }

private:
    int socket_ = -1;
    sockaddr_in target_{};
    sockaddr_in mirror_{}; // sin_port 0 = no mirror
    bool hevc_ = false;
    uint32_t nextFrameId_ = 0;
    uint64_t framesSent_ = 0, bytesSent_ = 0, sendErrors_ = 0, keyframeRequests_ = 0;
};

} // namespace lumen
