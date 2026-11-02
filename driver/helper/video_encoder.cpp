#include "video_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstdio>

namespace lumen {

namespace {

void LogError(const char *what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    std::fprintf(stderr, "lumen-capture: %s: %s\n", what, buf);
}

} // namespace

VideoEncoder::~VideoEncoder() { Close(); }

bool VideoEncoder::Open(const std::string &codecName, uint32_t width, uint32_t height, int fps, int bitrateMbps,
                        AVBufferRef *cudaDevice) {
    std::string encoderName = codecName + "_nvenc";
    const AVCodec *codec = avcodec_find_encoder_by_name(encoderName.c_str());
    if (!codec) {
        std::fprintf(stderr, "lumen-capture: no encoder %s\n", encoderName.c_str());
        return false;
    }

    int err = 0;
    codec_ = avcodec_alloc_context3(codec);
    codec_->width = static_cast<int>(width);
    codec_->height = static_cast<int>(height);
    // A pool of CUDA surfaces holding RGBX; NVENC reads them in place.
    AVBufferRef *framesRef = av_hwframe_ctx_alloc(cudaDevice);
    auto *frames = reinterpret_cast<AVHWFramesContext *>(framesRef->data);
    frames->format = AV_PIX_FMT_CUDA;
    frames->sw_format = AV_PIX_FMT_RGB0;
    frames->width = static_cast<int>(width);
    frames->height = static_cast<int>(height);
    frames->initial_pool_size = 2;
    if ((err = av_hwframe_ctx_init(framesRef)) < 0) {
        LogError("av_hwframe_ctx_init", err);
        av_buffer_unref(&framesRef);
        Close();
        return false;
    }
    codec_->pix_fmt = AV_PIX_FMT_CUDA;
    codec_->hw_frames_ctx = framesRef; // codec context takes the reference
    codec_->time_base = {1, fps};
    codec_->framerate = {fps, 1};
    codec_->bit_rate = int64_t(bitrateMbps) * 1000000;
    codec_->rc_max_rate = codec_->bit_rate;
    codec_->rc_buffer_size = static_cast<int>(codec_->bit_rate / fps); // ~1 frame of buffering
    codec_->gop_size = fps;                                            // keyframe every second
    codec_->max_b_frames = 0;
    av_opt_set(codec_->priv_data, "preset", "p1", 0);
    av_opt_set(codec_->priv_data, "tune", "ull", 0);
    av_opt_set(codec_->priv_data, "rc", "cbr", 0);
    av_opt_set_int(codec_->priv_data, "zerolatency", 1, 0);
    av_opt_set_int(codec_->priv_data, "delay", 0, 0);
    av_opt_set_int(codec_->priv_data, "rc-lookahead", 0, 0);
    av_opt_set_int(codec_->priv_data, "forced-idr", 1, 0); // RequestKeyframe gives a real IDR

    if ((err = avcodec_open2(codec_, codec, nullptr)) < 0) {
        LogError("avcodec_open2", err);
        Close();
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return true;
}

int64_t VideoEncoder::Drain() {
    int64_t bytes = 0;
    for (;;) {
        int err = avcodec_receive_packet(codec_, packet_);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return bytes;
        if (err < 0) {
            LogError("avcodec_receive_packet", err);
            return -1;
        }
        bytes += packet_->size;
        bool keyframe = packet_->flags & AV_PKT_FLAG_KEY;
        if (keyframe)
            ++keyframes_;
        if (onPacket_)
            onPacket_(packet_->data, size_t(packet_->size), keyframe);
        av_packet_unref(packet_);
    }
}

bool VideoEncoder::EncodeCuda(const std::function<bool(CUdeviceptr, size_t)> &fill, int64_t pts,
                              EncodeStats &stats) {
    using Clock = std::chrono::steady_clock;
    if (!codec_ || !codec_->hw_frames_ctx)
        return false;
    auto t0 = Clock::now();
    av_frame_unref(frame_);
    if (int err = av_hwframe_get_buffer(codec_->hw_frames_ctx, frame_, 0); err < 0) {
        LogError("av_hwframe_get_buffer", err);
        return false;
    }
    if (!fill(reinterpret_cast<CUdeviceptr>(frame_->data[0]), size_t(frame_->linesize[0])))
        return false;
    frame_->pts = pts;
    stats.copyMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return Send(stats);
}

bool VideoEncoder::Send(EncodeStats &stats) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    frame_->pict_type = keyframeRequested_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    keyframeRequested_ = false;
    if (int err = avcodec_send_frame(codec_, frame_); err < 0) {
        LogError("avcodec_send_frame", err);
        return false;
    }
    // With delay=0 and no B-frames the packet for this frame is ready as soon
    // as send_frame returns, so this measures the full encode.
    stats.bytes = Drain();
    stats.encodeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return stats.bytes >= 0;
}

void VideoEncoder::Close() {
    avcodec_free_context(&codec_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
}

} // namespace lumen
