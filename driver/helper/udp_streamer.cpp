#include "udp_streamer.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "video_stream.h"

namespace lumen {

UdpStreamer::~UdpStreamer() {
    if (socket_ >= 0)
        close(socket_);
}

bool ResolveTarget(const std::string &target, uint16_t defaultPort, sockaddr_in &out) {
    std::string host = target, port = std::to_string(defaultPort);
    if (auto colon = target.rfind(':'); colon != std::string::npos) {
        host = target.substr(0, colon);
        port = target.substr(colon + 1);
    }

    addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
        std::fprintf(stderr, "lumen-capture: can't resolve %s\n", target.c_str());
        return false;
    }
    std::memcpy(&out, result->ai_addr, sizeof(out));
    freeaddrinfo(result);
    return true;
}

bool UdpStreamer::Open(const std::string &target, bool hevc) {
    hevc_ = hevc;
    if (!ResolveTarget(target, kVideoPort, target_))
        return false;

    socket_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_ < 0)
        return false;
    // A keyframe is ~50-80 KB, sent as one burst; don't let the kernel drop
    // the tail of it locally.
    int sendBuffer = 4 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
    return true;
}

void UdpStreamer::SendFrame(const uint8_t *data, size_t size, bool keyframe, uint64_t captureTimeUs,
                            const float renderPose[4], const float tanHalfFov[2], const float bounds[4],
                            uint32_t width, uint32_t height) {
    size_t count = (size + kMaxPayload - 1) / kMaxPayload;
    if (count == 0 || count > 0xffff)
        return;

    VideoPacketHeader header{};
    std::memcpy(header.magic, "LMV3", 4);
    header.frameId = nextFrameId_++;
    header.packetCount = static_cast<uint16_t>(count);
    header.flags = (keyframe ? kVideoKeyframe : 0) | (hevc_ ? kVideoHevc : 0);
    header.captureTimeUs = captureTimeUs;
    std::memcpy(header.renderPose, renderPose, sizeof(header.renderPose));
    std::memcpy(header.tanHalfFov, tanHalfFov, sizeof(header.tanHalfFov));
    std::memcpy(header.bounds, bounds, sizeof(header.bounds));
    header.width = static_cast<uint16_t>(width);
    header.height = static_cast<uint16_t>(height);

    uint8_t datagram[kMaxDatagram];
    for (size_t i = 0; i < count; ++i) {
        size_t offset = i * kMaxPayload;
        size_t chunk = std::min(size_t(kMaxPayload), size - offset);
        header.packetIndex = static_cast<uint16_t>(i);
        std::memcpy(datagram, &header, sizeof(header));
        std::memcpy(datagram + sizeof(header), data + offset, chunk);
        ssize_t n = sendto(socket_, datagram, sizeof(header) + chunk, 0, reinterpret_cast<sockaddr *>(&target_),
                           sizeof(target_));
        if (n < 0)
            ++sendErrors_;
        else
            bytesSent_ += size_t(n);
    }
    ++framesSent_;

    // The mirror is a raw byte stream: chunk boundaries don't matter, the
    // decoder's parser finds the frames. Nobody listening is fine (UDP).
    if (mirror_.sin_port != 0)
        for (size_t offset = 0; offset < size; offset += 60000)
            sendto(socket_, data + offset, std::min(size_t(60000), size - offset), 0,
                   reinterpret_cast<sockaddr *>(&mirror_), sizeof(mirror_));
}

void UdpStreamer::SetMirror(uint16_t port) {
    mirror_ = sockaddr_in{};
    mirror_.sin_family = AF_INET;
    mirror_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    mirror_.sin_port = htons(port);
}

bool UdpStreamer::TakeKeyframeRequest() {
    bool requested = false;
    KeyframeRequest request;
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    while (recvfrom(socket_, &request, sizeof(request), 0, reinterpret_cast<sockaddr *>(&from), &fromLen) ==
           sizeof(request)) {
        // Only from the receiver we're streaming to.
        if (from.sin_addr.s_addr == target_.sin_addr.s_addr && std::memcmp(request.magic, "LMK1", 4) == 0)
            requested = true;
        fromLen = sizeof(from);
    }
    if (requested)
        ++keyframeRequests_;
    return requested;
}

} // namespace lumen
