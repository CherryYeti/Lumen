#include "shared_fd.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace lumen {

namespace {

// SteamVR's naming of the one-shot sockets for ipcHandle (abstract
// namespace, sun_path[0] = NUL):
//   2.17 and earlier: "fd-srv-<ipcHandle decimal>", NUL-padded to the whole
//                     sun_path (address length = sizeof(sockaddr_un)).
//   2.18.4:           the same text without its last character, address
//                     length exactly the name (no padding). Observed
//                     (handle 4295273622 -> "fd-srv-429527362"); looks like an
//                     off-by-one in SteamVR's own length, so the client name is
//                     cut the same way.
socklen_t AbstractAddress(const char *prefix, uint64_t ipcHandle, bool v218, sockaddr_un &addr) {
    addr = sockaddr_un{};
    addr.sun_family = AF_UNIX;
    int len = std::snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "%s%llu", prefix,
                            static_cast<unsigned long long>(ipcHandle));
    if (!v218)
        return sizeof(addr);
    addr.sun_path[len] = '\0'; // drop the last character
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
}

// One attempt with one naming scheme. Returns the fd, -1 on failure, or -2 if
// no server exists under that name (try the other scheme).
int Fetch(uint64_t ipcHandle, bool v218) {
    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;

    timeval timeout{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_un client, server;
    socklen_t clientLen = AbstractAddress("fd-cl-", ipcHandle, v218, client);
    socklen_t serverLen = AbstractAddress("fd-srv-", ipcHandle, v218, server);
    int fd = -1;
    char request[8] = {};
    if (bind(sock, reinterpret_cast<sockaddr *>(&client), clientLen) == 0) {
        if (sendto(sock, request, sizeof(request), 0, reinterpret_cast<sockaddr *>(&server), serverLen) < 0) {
            fd = errno == ECONNREFUSED || errno == ENOENT ? -2 : -1;
        } else {
            char reply[64];
            iovec iov{reply, sizeof(reply)};
            alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);

            ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
            if (n == 8 && std::memcmp(reply, "VALVE\0vk", 8) == 0) {
                for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
                    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                        std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
            }
        }
    }
    close(sock);
    return fd;
}

} // namespace

int ReceiveSharedFd(uint64_t ipcHandle) {
    int fd = Fetch(ipcHandle, true);
    if (fd == -2)
        fd = Fetch(ipcHandle, false);
    return fd < 0 ? -1 : fd;
}

} // namespace lumen
