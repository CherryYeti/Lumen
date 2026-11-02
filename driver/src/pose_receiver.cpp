#include "pose_receiver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>

namespace lumen {

bool PoseReceiver::Start(uint16_t port, Callback onPose, BatteryCallback onBattery, LogCallback onLog,
                         DisplayCallback onDisplay) {
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0)
        return false;

    int reuse = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Wake up periodically so Stop() doesn't hang waiting for a packet.
    timeval timeout{0, 100 * 1000};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(socket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }

    onPose_ = std::move(onPose);
    onBattery_ = std::move(onBattery);
    onLog_ = std::move(onLog);
    onDisplay_ = std::move(onDisplay);
    running_ = true;
    thread_ = std::thread(&PoseReceiver::Loop, this);
    return true;
}

void PoseReceiver::Stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

bool PoseReceiver::SendToPhone(const void *data, size_t size) {
    std::lock_guard<std::mutex> lock(phoneMutex_);
    if (phone_.sin_family != AF_INET || socket_ < 0)
        return false;
    return sendto(socket_, data, size, 0, reinterpret_cast<const sockaddr *>(&phone_), sizeof(phone_)) ==
           ssize_t(size);
}

void PoseReceiver::Loop() {
    PosePacket packet;
    char datagram[1500];
    while (running_) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(socket_, datagram, sizeof(datagram), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (n >= 4 && std::memcmp(datagram, "LML1", 4) == 0) {
            if (onLog_)
                onLog_(std::string(datagram + 4, size_t(n - 4)));
            continue;
        }
        if (n == sizeof(DisplayPacket) && std::memcmp(datagram, "LMD1", 4) == 0) {
            DisplayPacket display;
            std::memcpy(&display, datagram, sizeof(display));
            if (onDisplay_ && display.eyeWidth >= 256 && display.eyeWidth <= 4096 && display.eyeHeight >= 256 &&
                display.eyeHeight <= 4096 && std::isfinite(display.fovDegrees) && display.fovDegrees >= 40.f &&
                display.fovDegrees <= 140.f)
                onDisplay_(display.eyeWidth, display.eyeHeight, display.fovDegrees);
            continue;
        }
        if (n < 4 || size_t(n) > sizeof(packet))
            continue;
        std::memcpy(&packet, datagram, size_t(n));
        if (n == sizeof(BatteryPacket) && std::memcmp(packet.magic, "LMB1", 4) == 0) {
            BatteryPacket battery;
            std::memcpy(&battery, &packet, sizeof(battery));
            if (onBattery_ && std::isfinite(battery.level))
                onBattery_(std::fmax(0.f, std::fmin(1.f, battery.level)), battery.flags & 1);
            continue;
        }
        bool v1 = n == 24 && std::memcmp(packet.magic, "LMN1", 4) == 0;
        bool v4 = n == sizeof(packet) && std::memcmp(packet.magic, "LMN4", 4) == 0;
        if (!v1 && !v4)
            continue;
        PoseInput input;
        if (v4) {
            input.buttons = packet.buttons;
            auto axis = [](float v, float lo) { return std::isfinite(v) ? std::fmax(lo, std::fmin(1.f, v)) : 0.f; };
            input.gamepad.buttons = packet.padButtons;
            input.gamepad.lx = axis(packet.lx, -1.f);
            input.gamepad.ly = axis(packet.ly, -1.f);
            input.gamepad.rx = axis(packet.rx, -1.f);
            input.gamepad.ry = axis(packet.ry, -1.f);
            input.gamepad.lt = axis(packet.lt, 0.f);
            input.gamepad.rt = axis(packet.rt, 0.f);
            input.padX = axis(packet.padX, -1.f);
            input.padY = axis(packet.padY, -1.f);
        }

        // Drop reordered packets. After a pause, accept anything so a restarted
        // sender (sequence back at 0) isn't ignored.
        auto now = std::chrono::steady_clock::now();
        bool recent = haveSequence_ && now - lastPacketTime_ < std::chrono::milliseconds(500);
        if (recent && static_cast<int32_t>(packet.sequence - lastSequence_) <= 0)
            continue;
        lastSequence_ = packet.sequence;
        lastPacketTime_ = now;
        haveSequence_ = true;
        {
            std::lock_guard<std::mutex> lock(phoneMutex_);
            phone_ = from;
        }

        float len = std::sqrt(packet.w * packet.w + packet.x * packet.x +
                              packet.y * packet.y + packet.z * packet.z);
        if (!std::isfinite(len) || len < 1e-4f)
            continue;

        onPose_({packet.w / len, packet.x / len, packet.y / len, packet.z / len}, input);
    }
}

} // namespace lumen
