// lumen-capture: streams SteamVR's frames to the phone. The Lumen driver
// hands over each compositor texture's memory and announces every finished
// frame; this process imports the textures into CUDA, crops and scales both
// eyes side by side on the GPU, encodes them with NVENC and sends them over
// UDP (src/video_stream.h). It also streams the PC's audio
// (helper/audio_streamer.h). Encoding runs inline in the receive loop; if it
// falls behind, the driver drops frame notifications rather than waiting,
// and the gaps show up as "skipped".
//
// Usage: lumen-capture --stream HOST[:PORT] [--codec h264|hevc] [--bitrate MBPS]
//                      [--mirror PORT] [--preview FILE] [--screenshot-dir DIR]
//
//   --mirror PORT     also send the encoded stream, as a plain byte stream, to
//                     that port on localhost (the Lumen app's video window)
//   --preview FILE    rewrite a quarter-size PNG of the frame ~4x a second
//   --screenshot-dir  where SIGUSR1 saves the next frame as a full-size PNG
//                     (both eyes side by side): kill -USR1 <pid>

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include "audio_streamer.h"
#include "cuda_frames.h"
#include "frame_ipc.h"
#include "frame_reader.h"
#include "udp_streamer.h"
#include "video_encoder.h"
#include "video_stream.h"

using namespace lumen;

namespace {

struct Options {
    std::string stream; // HOST[:PORT] to stream to
    std::string codec = "h264";
    int bitrateMbps = 30;
    int mirrorPort = 0; // also send the encoded stream to this localhost port
    std::string preview;       // small PNG of the stream, rewritten ~4x a second (Lumen app's preview)
    std::string screenshotDir; // where SIGUSR1 saves full-size PNGs
    int fps = 60;
};

// Set by SIGUSR1: save the next frame as a full-size PNG.
volatile std::sig_atomic_t gScreenshotRequested = 0;
void OnScreenshotSignal(int) { gScreenshotRequested = 1; }

// Print stats every this many frames (10 s).
constexpr size_t kReportInterval = 600;

uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int Listen() {
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, kHelperSocketName, sizeof(kHelperSocketName) - 1);
    socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + sizeof(kHelperSocketName) - 1;
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr *>(&addr), len) != 0 || listen(sock, 1) != 0) {
        std::perror("lumen-capture: listen");
        return -1;
    }
    return sock;
}

// Returns bytes read (0 on disconnect, <0 if nothing waiting with
// MSG_DONTWAIT); *fd gets an attached fd or -1.
ssize_t Receive(int sock, HelperMessage &msg, int *fd, int flags = 0) {
    iovec iov{&msg, sizeof(msg)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    msghdr hdr{};
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = control;
    hdr.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(sock, &hdr, MSG_CMSG_CLOEXEC | flags);
    *fd = -1;
    for (cmsghdr *c = CMSG_FIRSTHDR(&hdr); n > 0 && c; c = CMSG_NXTHDR(&hdr, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            std::memcpy(fd, CMSG_DATA(c), sizeof(int));
    return n;
}

struct Series {
    const char *name;
    std::vector<double> values;

    void Print() const {
        if (values.empty())
            return;
        std::vector<double> v = values;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v)
            sum += x;
        auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(p * v.size()))]; };
        std::fprintf(stderr, "  %-22s avg %7.2f  p50 %7.2f  p95 %7.2f  max %7.2f\n", name, sum / v.size(), pct(0.5),
                     pct(0.95), v.back());
    }
};

class Streamer {
public:
    Streamer(const Options &options, AVBufferRef *cudaDevice, CudaFrames &cuda)
        : options_(options), cudaDevice_(cudaDevice), cuda_(cuda) {}

    // Called for every finished frame.
    void OnFrame(FrameReader &reader, const HelperMessage &msg) {
        using Clock = std::chrono::steady_clock;
        auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        if (failed_)
            return; // until the driver reconnects

        // The texture size comes from the Vulkan import.
        uint32_t width, height;
        if (!reader.FrameSize(msg.handle[0], msg.handle[1], width, height)) {
            if (msg.handle[0] != lastUnknown_) {
                std::fprintf(stderr, "lumen-capture: frame uses textures 0x%llx/0x%llx that weren't imported; skipping\n",
                             static_cast<unsigned long long>(msg.handle[0]),
                             static_cast<unsigned long long>(msg.handle[1]));
                lastUnknown_ = msg.handle[0];
            }
            return;
        }

        // Games may render into only part of each texture (SteamVR Home: the
        // top-left 68%), the rest black. Only that picture area is encoded,
        // scaled to fill the eye, so every client gets a full image and no
        // bits or decode time go to black; the stream's bounds are then the
        // whole frame. Sizes are rounded to 32 px, so slightly varying bounds
        // don't restart the encoder.
        float bounds[4] = {0.f, 0.f, 1.f, 1.f};
        const float *b = msg.bounds;
        bool validBounds = b[0] >= 0.f && b[1] >= 0.f && b[2] <= 1.f && b[3] <= 1.f && b[2] - b[0] > 0.05f &&
                           b[3] - b[1] > 0.05f;
        if (validBounds && (b[0] > 0.f || b[1] > 0.f || b[2] < 1.f || b[3] < 1.f)) {
            std::memcpy(bounds, b, sizeof(bounds));
            auto round32 = [](float v) { return std::max(32u, uint32_t(v / 32.f + 0.5f) * 32u); };
            uint32_t eyeW = round32((width / 2) * (b[2] - b[0])), eyeH = round32(height * (b[3] - b[1]));
            width = eyeW * 2;
            height = eyeH;
        }

        // Cap each eye: games pick their own render size (Steam Home:
        // 2409x2409), which can exceed what the phone can decode. The cap is
        // the headset's own eye size, which the driver sends.
        if (uint32_t maxEye = msg.maxEye; maxEye > 0) {
            uint32_t eyeW = width / 2, eyeH = height;
            uint32_t longest = std::max(eyeW, eyeH);
            if (longest > maxEye) {
                double s = double(maxEye) / longest;
                eyeW = std::max(16u, uint32_t(eyeW * s) & ~1u);
                eyeH = std::max(16u, uint32_t(eyeH * s) & ~1u);
                width = eyeW * 2;
                height = eyeH;
            }
        }

        // The frame size changes when a game takes over from the SteamVR
        // home/dashboard (each provides its own eye textures). Restart the
        // encoder at the new size; its first frame is a keyframe.
        if (encoder_ && (width != encoderWidth_ || height != encoderHeight_)) {
            std::fprintf(stderr, "lumen-capture: frame size changed %ux%u -> %ux%u, restarting the encoder\n",
                         encoderWidth_, encoderHeight_, width, height);
            encoder_.reset();
        }

        if (!encoder_) {
            if (!streamer_) {
                // One streamer for the whole session, so frame ids keep counting
                // across encoder restarts.
                streamer_ = std::make_unique<UdpStreamer>();
                if (!streamer_->Open(options_.stream, options_.codec == "hevc")) {
                    streamer_.reset();
                    failed_ = true;
                    return;
                }
                if (options_.mirrorPort > 0 && options_.mirrorPort < 65536)
                    streamer_->SetMirror(uint16_t(options_.mirrorPort));
            }
            encoder_ = std::make_unique<VideoEncoder>();
            encoder_->SetPacketCallback([this](const uint8_t *data, size_t size, bool keyframe) {
                const float wholeFrame[4] = {0.f, 0.f, 1.f, 1.f}; // the picture was cropped to fill it
                streamer_->SendFrame(data, size, keyframe, captureUs_, framePose_, frameFov_, wholeFrame,
                                     encoderWidth_, encoderHeight_);
            });
            if (!encoder_->Open(options_.codec, width, height, options_.fps, options_.bitrateMbps, cudaDevice_)) {
                encoder_.reset();
                failed_ = true;
                return;
            }
            encoderWidth_ = width;
            encoderHeight_ = height;
            std::fprintf(stderr, "lumen-capture: encoding frames of %ux%u %s at %d Mbps, streaming to %s\n", width,
                         height, options_.codec.c_str(), options_.bitrateMbps, options_.stream.c_str());
            firstFrame_ = msg.frame;
        } else if (msg.frame != lastFrame_ + 1) {
            skipped_ += msg.frame - lastFrame_ - 1;
        }
        lastFrame_ = msg.frame;

        // pts from the compositor's frame counter, so skipped frames leave gaps.
        int64_t pts = int64_t(msg.frame - firstFrame_);
        if (streamer_->TakeKeyframeRequest())
            encoder_->RequestKeyframe();
        EncodeStats stats;
        auto t0 = Clock::now();
        captureUs_ = NowUs();
        std::memcpy(framePose_, msg.pose, sizeof(framePose_));
        std::memcpy(frameFov_, msg.tanHalfFov, sizeof(frameFov_));
        bool ok = encoder_->EncodeCuda(
            [&](CUdeviceptr dst, size_t pitch) {
                return cuda_.CopySideBySide(msg.handle[0], msg.handle[1], dst, pitch, width / 2, height, bounds);
            },
            pts, stats);
        if (!ok) {
            encoder_.reset();
            failed_ = true;
            return;
        }
        auto t1 = Clock::now();
        if (gScreenshotRequested && !options_.screenshotDir.empty()) {
            gScreenshotRequested = 0;
            SaveScreenshot(reader, msg);
        }
        if (!options_.preview.empty() && t1 - lastPreview_ > std::chrono::milliseconds(250)) {
            WritePreview(reader, msg);
            lastPreview_ = t1;
        }

        copy_.values.push_back(stats.copyMs);
        encode_.values.push_back(stats.encodeMs);
        total_.values.push_back(ms(t1 - t0));
        kilobytes_.values.push_back(stats.bytes / 1024.0);
        ++encoded_;
        if (total_.values.size() >= kReportInterval)
            Report();
    }

    // Writes a quarter-size PNG of the current frame for the Lumen app's
    // preview (SteamVR's own "Display VR View" stays black in driver direct
    // mode). Written to a temp file and renamed, so readers never see half a file.
    void WritePreview(FrameReader &reader, const HelperMessage &msg) {
        uint32_t width, height;
        const uint8_t *rgbx = reader.ReadSideBySide(msg.handle[0], msg.handle[1], width, height);
        if (!rgbx)
            return;
        const uint32_t f = 4, w = width / f, h = height / f;
        previewPixels_.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    uint32_t sum = 0;
                    for (uint32_t dy = 0; dy < f; ++dy)
                        for (uint32_t dx = 0; dx < f; ++dx)
                            sum += rgbx[(size_t(y * f + dy) * width + x * f + dx) * 4 + c];
                    previewPixels_[(size_t(y) * w + x) * 4 + c] = uint8_t(sum / (f * f));
                }
        std::string tmp = options_.preview + ".tmp";
        if (WritePng(tmp, previewPixels_.data(), w, h))
            std::rename(tmp.c_str(), options_.preview.c_str());
    }

    // Full-size PNG of the current frame, both eyes side by side.
    void SaveScreenshot(FrameReader &reader, const HelperMessage &msg) {
        uint32_t width, height;
        const uint8_t *rgbx = reader.ReadSideBySide(msg.handle[0], msg.handle[1], width, height);
        if (!rgbx) {
            std::fprintf(stderr, "lumen-capture: screenshot failed: frame not readable\n");
            return;
        }
        char name[64];
        std::time_t t = std::time(nullptr);
        std::strftime(name, sizeof(name), "lumen-%Y%m%d-%H%M%S.png", std::localtime(&t));
        std::string path = options_.screenshotDir + "/" + name;
        if (WritePng(path, rgbx, width, height))
            std::fprintf(stderr, "lumen-capture: screenshot saved: %s (%ux%u)\n", path.c_str(), width, height);
        else
            std::fprintf(stderr, "lumen-capture: screenshot failed: can't write %s\n", path.c_str());
    }

private:
    // Prints stats since the last report, then starts a new window.
    void Report() {
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart_).count();
        std::fprintf(stderr,
                     "lumen-capture: %llu frames encoded, %llu skipped so far. Last %zu frames (ms; size in KiB):\n",
                     static_cast<unsigned long long>(encoded_), static_cast<unsigned long long>(skipped_),
                     total_.values.size());
        for (Series *s : {&copy_, &encode_, &total_, &kilobytes_}) {
            s->Print();
            s->values.clear();
        }
        uint64_t bytes = streamer_->BytesSent();
        std::fprintf(stderr, "  stream: %.1f Mbit/s, %llu keyframes, %llu keyframe requests, %llu send errors\n",
                     seconds > 0 ? (bytes - windowBytes_) * 8 / seconds / 1e6 : 0.0,
                     static_cast<unsigned long long>(encoder_->Keyframes()),
                     static_cast<unsigned long long>(streamer_->KeyframeRequests()),
                     static_cast<unsigned long long>(streamer_->SendErrors()));
        windowBytes_ = bytes;
        windowStart_ = std::chrono::steady_clock::now();
    }

    Options options_;
    AVBufferRef *cudaDevice_;
    CudaFrames &cuda_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<UdpStreamer> streamer_;
    uint64_t captureUs_ = 0; // time the frame being encoded was read
    float framePose_[4] = {1, 0, 0, 0}, frameFov_[2] = {1, 1}; // sent with the frame being encoded
    uint32_t encoderWidth_ = 0, encoderHeight_ = 0;
    uint64_t lastUnknown_ = 0; // last texture reported as not imported
    bool failed_ = false;      // encoding failed; stop until the driver reconnects
    std::chrono::steady_clock::time_point lastPreview_;
    std::vector<uint8_t> previewPixels_;
    uint64_t firstFrame_ = 0, lastFrame_ = 0, skipped_ = 0, encoded_ = 0;
    std::chrono::steady_clock::time_point windowStart_ = std::chrono::steady_clock::now();
    uint64_t windowBytes_ = 0;
    Series copy_{"crop/scale to encoder"}, encode_{"NVENC encode"}, total_{"total"}, kilobytes_{"compressed size"};
};

void Serve(int client, const Options &options, AVBufferRef *cudaDevice) {
    FrameReader reader;
    CudaFrames cuda(
        reinterpret_cast<AVCUDADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(cudaDevice->data)->hwctx)
            ->cuda_ctx);
    Streamer streamer(options, cudaDevice, cuda);
    std::set<uint64_t> imported;

    auto handle = [&](const HelperMessage &msg, int fd) {
        if (msg.type == kImport && fd >= 0) {
            if (imported.count(msg.handle[0]))
                reader.Forget(msg.handle[0]); // Re-sent after a reconnect.
            // Vulkan and CUDA each consume an fd, so give Vulkan a duplicate.
            // Vulkan goes first: CUDA needs the allocation size it reports.
            int cudaFd = dup(fd);
            uint64_t size = 0;
            if (reader.Import(msg.handle[0], fd, msg.width, msg.height, static_cast<VkFormat>(msg.format), &size))
                imported.insert(msg.handle[0]);
            if (cudaFd >= 0) {
                if (size)
                    cuda.Import(msg.handle[0], cudaFd, size, msg.width, msg.height);
                else
                    close(cudaFd);
            }
            return; // Imports took ownership of the fds (or closed them).
        }
        if (msg.type == kForget) {
            reader.Forget(msg.handle[0]);
            cuda.Forget(msg.handle[0]);
            imported.erase(msg.handle[0]);
        }
        if (fd >= 0)
            close(fd);
    };

    HelperMessage msg;
    int fd;
    while (Receive(client, msg, &fd) == sizeof(msg)) {
        if (msg.type != kFrame) {
            handle(msg, fd);
            continue;
        }
        // If we fell behind, frame notifications have queued up and the older
        // ones point at textures the compositor has since overwritten. Skip
        // to the newest; the gap counts as skipped frames.
        HelperMessage latest = msg, next;
        bool connected = true;
        for (;;) {
            ssize_t n = Receive(client, next, &fd, MSG_DONTWAIT);
            if (n < 0)
                break;
            if (n != sizeof(next)) {
                connected = false;
                break;
            }
            if (next.type == kFrame)
                latest = next;
            else
                handle(next, fd);
        }
        streamer.OnFrame(reader, latest);
        if (!connected)
            break;
    }
    std::fprintf(stderr, "lumen-capture: driver disconnected\n");
}

bool ParseArgs(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == "--stream" && hasValue)
            o.stream = argv[++i];
        else if (a == "--codec" && hasValue)
            o.codec = argv[++i];
        else if (a == "--bitrate" && hasValue)
            o.bitrateMbps = std::atoi(argv[++i]);
        else if (a == "--mirror" && hasValue)
            o.mirrorPort = std::atoi(argv[++i]);
        else if (a == "--preview" && hasValue)
            o.preview = argv[++i];
        else if (a == "--screenshot-dir" && hasValue)
            o.screenshotDir = argv[++i];
        else
            return false;
    }
    return !o.stream.empty() && (o.codec == "h264" || o.codec == "hevc") && o.bitrateMbps > 0;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: lumen-capture --stream HOST[:PORT] [--codec h264|hevc] [--bitrate MBPS]\n"
                     "                     [--mirror PORT] [--preview FILE] [--screenshot-dir DIR]\n");
        return 2;
    }
    // SA_RESTART: the signal mustn't interrupt the receive loop (that would
    // look like the driver disconnecting).
    struct sigaction action {};
    action.sa_handler = OnScreenshotSignal;
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, nullptr);
    AVBufferRef *cudaDevice = nullptr;
    if (int err = av_hwdevice_ctx_create(&cudaDevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0); err < 0) {
        std::fprintf(stderr, "lumen-capture: could not create CUDA device (%d)\n", err);
        return 1;
    }
    // Audio runs for the whole process, independent of driver (re)connects.
    AudioStreamer audio;
    sockaddr_in target{};
    if (!ResolveTarget(options.stream, kVideoPort, target) || !audio.Start(target))
        std::fprintf(stderr, "lumen-capture: audio not started\n");
    int listener = Listen();
    if (listener < 0)
        return 1;
    std::fprintf(stderr, "lumen-capture: waiting for the driver on @%s\n", kHelperSocketName);
    for (;;) {
        int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0)
            continue;
        std::fprintf(stderr, "lumen-capture: driver connected\n");
        Serve(client, options, cudaDevice);
        close(client);
    }
}
