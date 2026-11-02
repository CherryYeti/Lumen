#pragma once

#include <netinet/in.h>

#include <atomic>
#include <thread>

namespace lumen {

// Captures what the PC plays (the default output's monitor, through
// PulseAudio or PipeWire's PulseAudio server) and sends it to the phone in
// the audio_stream.h format, on its own thread.
class AudioStreamer {
public:
    ~AudioStreamer() { Stop(); }

    // target: the phone's address; the port is replaced with kAudioPort.
    bool Start(const sockaddr_in &target);
    void Stop();

private:
    void Loop();

    int socket_ = -1;
    sockaddr_in target_{};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace lumen
