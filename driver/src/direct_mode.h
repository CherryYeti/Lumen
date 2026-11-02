#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helper_link.h"

namespace lumen {

// Takes over presentation from the compositor. The compositor asks us for
// swap textures, renders the final distorted frame into them, then calls
// SubmitLayer + Present. The textures are shared Vulkan images allocated
// through SteamVR's IPC resource manager, so the compositor can import them.
//
// The driver never touches the pixels itself: holding an imported allocation
// inside vrserver made the compositor exit. Instead each texture's memory fd
// is handed to the lumen-capture helper process (HelperLink), which imports
// and reads it, and every finished frame is announced to it.
class DirectModeComponent : public vr::IVRDriverDirectModeComponent {
public:
    explicit DirectModeComponent(float displayFrequency);
    // The headset's eye output size (longest side), passed to lumen-capture
    // as the cap on streamed eyes.
    void SetMaxEye(uint32_t pixels) { maxEye_ = pixels; }
    // Field of view sent along with each frame (see video_stream.h).
    void SetTanHalfFov(float tanX, float tanY) {
        tanHalfFov_[0] = tanX;
        tanHalfFov_[1] = tanY;
    }

    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *outSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override;
    void DestroyAllSwapTextureSets(uint32_t pid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t *throttling) override {}

private:
    struct TextureSet {
        uint32_t pid;
        vr::SharedTextureHandle_t handles[3];
        uint32_t index = 0;
        uint32_t width = 0, height = 0, format = 0;
    };

    std::shared_ptr<TextureSet> FindSet(vr::SharedTextureHandle_t handle);
    void ReleaseSet(const std::shared_ptr<TextureSet> &set);
    void ExportToHelper(const TextureSet &set, uint32_t width, uint32_t height, uint32_t format);
    void PollHelper();
    bool ShouldLog(uint64_t count) const { return count < 10 || count % 600 == 0; }

    std::mutex mutex_;
    std::vector<std::shared_ptr<TextureSet>> sets_;
    std::atomic<uint64_t> submitCount_{0};
    std::atomic<uint64_t> presentCount_{0};
    uint32_t layersThisFrame_ = 0;
    bool warnedLayers_ = false;
    const TextureSet *streamedSet_ = nullptr; // texture set last reported as streamed (identity only)
    static constexpr uint32_t kMaxLoggedLayers = 4;
    vr::SharedTextureHandle_t frameLayerTextures_[kMaxLoggedLayers] = {};
    vr::SharedTextureHandle_t lastTextures_[2] = {};
    float lastPose_[4] = {1, 0, 0, 0}; // w, x, y, z the last frame was rendered with
    float lastBounds_[4] = {0, 0, 1, 1}; // picture area within each eye texture
    float tanHalfFov_[2] = {1, 1};
    uint32_t maxEye_ = 0;

    HelperLink helper_;
    std::atomic<bool> helperConnected_{false};
    std::chrono::steady_clock::time_point nextHelperAttempt_;

    // Present() blocks until the next fake vsync; without this the compositor
    // renders as fast as the GPU allows.
    std::chrono::steady_clock::duration framePeriod_;
    std::chrono::steady_clock::time_point nextVsync_;
};

} // namespace lumen
