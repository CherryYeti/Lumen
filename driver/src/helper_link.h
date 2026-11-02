#pragma once

#include <cstdint>
#include <mutex>

#include "frame_ipc.h"

namespace lumen {

// Driver-side connection to the lumen-capture helper. Every send is
// non-blocking and silently dropped if the helper isn't running, so the
// compositor never waits on it.
class HelperLink {
public:
    ~HelperLink();

    bool Connect();
    bool IsConnected();

    // Takes ownership of fd (closed whether or not it's delivered).
    void SendImport(uint64_t handle, int fd, uint32_t width, uint32_t height, uint32_t format);
    void SendForget(uint64_t handle);
    void SendFrame(uint64_t left, uint64_t right, uint64_t frame, const float pose[4], const float tanHalfFov[2],
                   const float bounds[4], uint32_t maxEye);

private:
    bool Send(const HelperMessage &msg, int fd = -1);
    void Disconnect();

    std::mutex mutex_;
    int socket_ = -1;
};

} // namespace lumen
