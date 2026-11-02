#pragma once

#include <cuda.h>

#include <cstdint>
#include <functional>
#include <string>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace lumen {

struct EncodeStats {
    int64_t bytes = 0;     // compressed output for this frame
    double copyMs = 0;     // filling the encoder's input surface (GPU copy/scale)
    double encodeMs = 0;   // send_frame + receive_packet
};

// Encodes RGBX CUDA surfaces with NVENC (h264_nvenc / hevc_nvenc); frames
// never leave the GPU. Tuned for latency: fastest preset, ultra-low-latency
// tuning, no B-frames, no lookahead, CBR. NVENC converts RGB to YUV itself.
// Output goes to the packet callback as Annex-B access units, with parameter
// sets in-band so a receiver can start from any keyframe.
class VideoEncoder {
public:
    ~VideoEncoder();

    // codec: "h264" or "hevc". cudaDevice: an AV_HWDEVICE_TYPE_CUDA device.
    bool Open(const std::string &codec, uint32_t width, uint32_t height, int fps, int bitrateMbps,
              AVBufferRef *cudaDevice);
    // fill writes RGBX rows into the CUDA surface (device pointer, pitch).
    bool EncodeCuda(const std::function<bool(CUdeviceptr, size_t)> &fill, int64_t pts, EncodeStats &stats);
    void Close();

    // Called with each encoded access unit (Annex-B) as soon as it's ready.
    void SetPacketCallback(std::function<void(const uint8_t *data, size_t size, bool keyframe)> onPacket) {
        onPacket_ = std::move(onPacket);
    }
    // Makes the next encoded frame an IDR.
    void RequestKeyframe() { keyframeRequested_ = true; }
    uint64_t Keyframes() const { return keyframes_; }

private:
    int64_t Drain();
    bool Send(EncodeStats &stats);

    AVCodecContext *codec_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *packet_ = nullptr;
    std::function<void(const uint8_t *, size_t, bool)> onPacket_;
    bool keyframeRequested_ = false;
    uint64_t keyframes_ = 0;
};

} // namespace lumen
