#include "audio_streamer.h"

#include <arpa/inet.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "audio_stream.h"

namespace lumen {

bool AudioStreamer::Start(const sockaddr_in &target) {
    target_ = target;
    target_.sin_port = htons(kAudioPort);
    socket_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_ < 0)
        return false;
    running_ = true;
    thread_ = std::thread(&AudioStreamer::Loop, this);
    return true;
}

void AudioStreamer::Stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

void AudioStreamer::Loop() {
    const pa_sample_spec spec{PA_SAMPLE_S16LE, kAudioRate, kAudioChannels};
    // Deliver one packet's worth at a time, so capture adds ~5 ms, not the
    // server's default ~2 s fragments.
    pa_buffer_attr attr;
    std::memset(&attr, 0xff, sizeof(attr)); // -1: server default
    attr.fragsize = kAudioPayloadBytes;

    uint8_t datagram[sizeof(AudioPacketHeader) + kAudioPayloadBytes];
    AudioPacketHeader header{};
    std::memcpy(header.magic, "LMA1", 4);
    bool reportedError = false;
    while (running_) {
        int error = 0;
        pa_simple *pa = pa_simple_new(nullptr, "Lumen", PA_STREAM_RECORD, "@DEFAULT_MONITOR@",
                                      "Audio to the headset", &spec, nullptr, &attr, &error);
        if (!pa) {
            if (!reportedError)
                std::fprintf(stderr, "lumen-capture: can't capture audio (%s); retrying every 2 s\n",
                             pa_strerror(error));
            reportedError = true;
            for (int i = 0; i < 20 && running_; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        reportedError = false;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &target_.sin_addr, ip, sizeof(ip));
        std::fprintf(stderr, "lumen-capture: streaming the default output's audio to %s:%u\n", ip, kAudioPort);
        while (running_) {
            if (pa_simple_read(pa, datagram + sizeof(header), kAudioPayloadBytes, &error) < 0) {
                std::fprintf(stderr, "lumen-capture: audio capture stopped (%s)\n", pa_strerror(error));
                break;
            }
            std::memcpy(datagram, &header, sizeof(header));
            ++header.sequence;
            sendto(socket_, datagram, sizeof(datagram), 0, reinterpret_cast<const sockaddr *>(&target_),
                   sizeof(target_));
        }
        pa_simple_free(pa);
    }
}

} // namespace lumen
