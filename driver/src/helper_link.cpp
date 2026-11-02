#include "helper_link.h"

#include <openvr_driver.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lumen {

HelperLink::~HelperLink() { Disconnect(); }

bool HelperLink::IsConnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    return socket_ >= 0;
}

bool HelperLink::Connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0)
        return true;
    socket_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_ < 0)
        return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, kHelperSocketName, sizeof(kHelperSocketName) - 1);
    socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + sizeof(kHelperSocketName) - 1;
    if (connect(socket_, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
        close(socket_);
        socket_ = -1;
        return false;
    }
    vr::VRDriverLog()->Log("lumen: connected to lumen-capture helper");
    return true;
}

void HelperLink::Disconnect() {
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

bool HelperLink::Send(const HelperMessage &msg, int fd) {
    iovec iov{const_cast<HelperMessage *>(&msg), sizeof(msg)};
    msghdr hdr{};
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    if (fd >= 0) {
        hdr.msg_control = control;
        hdr.msg_controllen = sizeof(control);
        cmsghdr *c = CMSG_FIRSTHDR(&hdr);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(socket_, &hdr, MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(msg))
        return true;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        vr::VRDriverLog()->Log("lumen: lost lumen-capture helper");
        Disconnect();
    }
    return false;
}

void HelperLink::SendImport(uint64_t handle, int fd, uint32_t width, uint32_t height, uint32_t format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0)
        Send({kImport, width, height, format, {handle, 0}, 0}, fd);
    close(fd); // The receiver gets its own copy.
}

void HelperLink::SendForget(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0)
        Send({kForget, 0, 0, 0, {handle, 0}, 0});
}

void HelperLink::SendFrame(uint64_t left, uint64_t right, uint64_t frame, const float pose[4],
                           const float tanHalfFov[2], const float bounds[4], uint32_t maxEye) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0)
        Send({kFrame, 0, 0, 0, {left, right}, frame, {pose[0], pose[1], pose[2], pose[3]},
              {tanHalfFov[0], tanHalfFov[1]}, {bounds[0], bounds[1], bounds[2], bounds[3]}, maxEye});
}

} // namespace lumen
