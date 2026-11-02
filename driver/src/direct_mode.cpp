#include "direct_mode.h"

#include "shared_fd.h"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <thread>

namespace lumen {

namespace {

void Logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Logf(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    vr::VRDriverLog()->Log(buffer);
}

unsigned long long U64(uint64_t v) { return static_cast<unsigned long long>(v); }

// Rotation part of a 3x4 pose matrix as a unit quaternion (w, x, y, z).
void QuaternionFromMatrix(const vr::HmdMatrix34_t &m, float q[4]) {
    float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0) {
        float s = std::sqrt(trace + 1.f) * 2.f;
        q[0] = 0.25f * s;
        q[1] = (m.m[2][1] - m.m[1][2]) / s;
        q[2] = (m.m[0][2] - m.m[2][0]) / s;
        q[3] = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        q[0] = (m.m[2][1] - m.m[1][2]) / s;
        q[1] = 0.25f * s;
        q[2] = (m.m[0][1] + m.m[1][0]) / s;
        q[3] = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        q[0] = (m.m[0][2] - m.m[2][0]) / s;
        q[1] = (m.m[0][1] + m.m[1][0]) / s;
        q[2] = 0.25f * s;
        q[3] = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
        q[0] = (m.m[1][0] - m.m[0][1]) / s;
        q[1] = (m.m[0][2] + m.m[2][0]) / s;
        q[2] = (m.m[1][2] + m.m[2][1]) / s;
        q[3] = 0.25f * s;
    }
}

} // namespace

DirectModeComponent::DirectModeComponent(float displayFrequency)
    : framePeriod_(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / displayFrequency))),
      nextVsync_(std::chrono::steady_clock::now()) {}

void DirectModeComponent::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc,
                                               SwapTextureSet_t *outSet) {
    *outSet = {};
    vr::IVRIPCResourceManagerClient *ipc = vr::VRIPCResourceManager();
    if (!ipc) {
        Logf("lumen: CreateSwapTextureSet: no IPC resource manager");
        return;
    }

    auto set = std::make_shared<TextureSet>();
    set->pid = pid;
    set->width = desc->nWidth;
    set->height = desc->nHeight;
    set->format = desc->nFormat;
    for (int i = 0; i < 3; ++i) {
        // nFormat is a VkFormat on Linux.
        if (!ipc->NewSharedVulkanImage(desc->nFormat, desc->nWidth, desc->nHeight, /*renderable*/ true,
                                       /*mappable*/ false, /*computeAccess*/ false, /*mips*/ 1, /*layers*/ 1,
                                       &set->handles[i])) {
            Logf("lumen: CreateSwapTextureSet: NewSharedVulkanImage failed (%ux%u format %u)", desc->nWidth,
                 desc->nHeight, desc->nFormat);
            for (int j = 0; j < i; ++j)
                ipc->UnrefResource(set->handles[j]);
            return;
        }
        outSet->rSharedTextureHandles[i] = set->handles[i];
    }

    Logf("lumen: CreateSwapTextureSet pid=%u %ux%u format=%u samples=%u -> 0x%llx 0x%llx 0x%llx", pid,
         desc->nWidth, desc->nHeight, desc->nFormat, desc->nSampleCount, U64(set->handles[0]),
         U64(set->handles[1]), U64(set->handles[2]));

    // Register before checking the helper, so a connect racing with this
    // call exports the set either here or in PollHelper (or both; harmless).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sets_.push_back(set);
    }
    if (helperConnected_)
        ExportToHelper(*set, desc->nWidth, desc->nHeight, desc->nFormat);
}

void DirectModeComponent::ExportToHelper(const TextureSet &set, uint32_t width, uint32_t height, uint32_t format) {
    vr::IVRIPCResourceManagerClient *ipc = vr::VRIPCResourceManager();
    if (!ipc)
        return;
    for (auto h : set.handles) {
        // Each RefResource call makes a fresh one-shot fd server. No
        // UnrefResource: this doesn't seem to add a reference, and dropping one
        // frees the compositor's image.
        uint64_t ipcHandle = 0;
        if (!ipc->RefResource(h, &ipcHandle)) {
            Logf("lumen: RefResource 0x%llx failed", U64(h));
            continue;
        }
        int fd = ReceiveSharedFd(ipcHandle);
        if (fd < 0) {
            Logf("lumen: no fd for 0x%llx (ipc 0x%llx)", U64(h), U64(ipcHandle));
            continue;
        }
        helper_.SendImport(h, fd, width, height, format);
    }
}

void DirectModeComponent::PollHelper() {
    // Connect (or reconnect) at most once a second, then hand over every
    // existing texture so the helper can start mid-session.
    auto now = std::chrono::steady_clock::now();
    if (now < nextHelperAttempt_)
        return;
    nextHelperAttempt_ = now + std::chrono::seconds(1);
    if (!helper_.Connect())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    helperConnected_ = true;
    for (auto &set : sets_)
        ExportToHelper(*set, set->width, set->height, set->format);
}

std::shared_ptr<DirectModeComponent::TextureSet> DirectModeComponent::FindSet(vr::SharedTextureHandle_t handle) {
    for (auto &set : sets_)
        for (auto h : set->handles)
            if (h == handle)
                return set;
    return nullptr;
}

void DirectModeComponent::ReleaseSet(const std::shared_ptr<TextureSet> &set) {
    for (auto h : set->handles)
        helper_.SendForget(h);
    if (auto *ipc = vr::VRIPCResourceManager())
        for (auto h : set->handles)
            ipc->UnrefResource(h);
    sets_.erase(std::remove(sets_.begin(), sets_.end(), set), sets_.end());
}

void DirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto set = FindSet(handle)) {
        Logf("lumen: DestroySwapTextureSet 0x%llx", U64(handle));
        ReleaseSet(set);
    }
}

void DirectModeComponent::DestroyAllSwapTextureSets(uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<TextureSet>> doomed;
    for (auto &set : sets_)
        if (set->pid == pid)
            doomed.push_back(set);
    for (auto &set : doomed)
        ReleaseSet(set);
    Logf("lumen: DestroyAllSwapTextureSets pid=%u (%zu sets)", pid, doomed.size());
}

void DirectModeComponent::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Both eyes may share one set; advance each set only once.
    auto left = FindSet(handles[0]);
    auto right = FindSet(handles[1]);
    if (left)
        left->index = (left->index + 1) % 3;
    if (right && right != left)
        right->index = (right->index + 1) % 3;
    (*indices)[0] = left ? left->index : 0;
    (*indices)[1] = right ? right->index : 0;
}

void DirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    uint64_t count = submitCount_++;
    if (layersThisFrame_ < kMaxLoggedLayers)
        frameLayerTextures_[layersThisFrame_] = perEye[0].hTexture;
    // While a game runs, the compositor passes its eye images through as the
    // first layer and may add its own overlay layer (dashboard, cursor) on
    // top. Stream the first: the game, or the compositor's own frame when no
    // game runs. Overlays over a game are dropped (blending them in isn't
    // implemented); streaming the last layer showed just the near-empty
    // overlay, i.e. a dark headset with a cursor.
    if (layersThisFrame_++ == 0) {
        lastTextures_[0] = perEye[0].hTexture;
        lastTextures_[1] = perEye[1].hTexture;
        // Both eyes share the HMD pose; it's what the client reprojects from.
        QuaternionFromMatrix(perEye[0].mHmdPose, lastPose_);
        // Games may render into part of the texture (VRChat: 96%).
        const vr::VRTextureBounds_t &b = perEye[0].bounds;
        lastBounds_[0] = b.uMin;
        lastBounds_[1] = b.vMin;
        lastBounds_[2] = b.uMax;
        lastBounds_[3] = b.vMax;
    }
    if (!ShouldLog(count))
        return;
    for (int eye = 0; eye < 2; ++eye) {
        const auto &e = perEye[eye];
        Logf("lumen: SubmitLayer #%llu eye=%d tex=0x%llx depth=0x%llx bounds=(%.2f,%.2f)-(%.2f,%.2f) "
             "pose.t=(%.2f,%.2f,%.2f) predict=%.4f",
             U64(count), eye, U64(e.hTexture), U64(e.hDepthTexture), e.bounds.uMin, e.bounds.vMin, e.bounds.uMax,
             e.bounds.vMax, e.mHmdPose.m[0][3], e.mHmdPose.m[1][3], e.mHmdPose.m[2][3],
             e.flHmdPosePredictionTimeInSecondsFromNow);
    }
}

void DirectModeComponent::Present(vr::SharedTextureHandle_t syncTexture) {
    uint64_t count = presentCount_++;
    uint32_t layers = layersThisFrame_;
    layersThisFrame_ = 0;
    if (ShouldLog(count))
        Logf("lumen: Present #%llu sync=0x%llx layers=%u", U64(count), U64(syncTexture), layers);
    {
        // Log when the streamed texture *set* changes (a game starting or
        // stopping); its 3 textures rotate every frame.
        std::shared_ptr<TextureSet> set;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            set = FindSet(lastTextures_[0]);
        }
        if (set.get() != streamedSet_) {
            Logf("lumen: streaming texture set of 0x%llx (%ux%u%s), %u layer(s) this frame", U64(lastTextures_[0]),
                 set ? set->width : 0, set ? set->height : 0, set ? "" : ", unknown set", layers);
            streamedSet_ = set.get();
        }
    }
    if (layers > 1 && !warnedLayers_) {
        char list[128] = "";
        for (uint32_t i = 0; i < layers && i < kMaxLoggedLayers; ++i)
            std::snprintf(list + strlen(list), sizeof(list) - strlen(list), " 0x%llx",
                          U64(frameLayerTextures_[i]));
        Logf("lumen: %u layers in one frame (left-eye textures:%s); streaming the first", layers, list);
        warnedLayers_ = true;
    }

    auto now = std::chrono::steady_clock::now();
    nextVsync_ += framePeriod_;
    if (nextVsync_ < now)
        nextVsync_ = now + framePeriod_; // Fell behind; resync instead of bursting.
    std::this_thread::sleep_until(nextVsync_);

    // By now (one frame period later) the GPU has finished this frame.
    if (helperConnected_ && !helper_.IsConnected())
        helperConnected_ = false;
    if (!helperConnected_)
        PollHelper();
    else
        helper_.SendFrame(lastTextures_[0], lastTextures_[1], count, lastPose_, tanHalfFov_, lastBounds_, maxEye_);
}

} // namespace lumen
