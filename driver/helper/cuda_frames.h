#pragma once

#include <cuda.h>

#include <cstdint>
#include <map>

namespace lumen {

// The compositor's swap textures, imported into CUDA from the same OPAQUE_FD
// memory the Vulkan side uses, so frames can go to NVENC without leaving the
// GPU. All calls need the CUDA context passed to the constructor (they push
// it themselves).
class CudaFrames {
public:
    explicit CudaFrames(CUcontext context) : context_(context) {}
    ~CudaFrames();

    // Takes ownership of memoryFd. allocationSize must be the exporter's
    // allocation size (the Vulkan side's memory requirements give it).
    bool Import(uint64_t handle, int memoryFd, uint64_t allocationSize, uint32_t width, uint32_t height);
    void Forget(uint64_t handle);

    // Copies both eyes side by side into a device buffer of RGBX rows
    // (dstPitch bytes apart), each eye eyeWidth x eyeHeight. Only the picture
    // part of each texture is used (bounds: uMin, vMin, uMax, vMax; games may
    // render into less than the whole texture) and it fills the eye: a plain
    // copy when the whole texture is the picture at that size, otherwise a
    // filtered GPU scale. Synchronous.
    bool CopySideBySide(uint64_t left, uint64_t right, CUdeviceptr dst, size_t dstPitch, uint32_t eyeWidth,
                        uint32_t eyeHeight, const float bounds[4]);

private:
    struct Texture {
        CUexternalMemory memory = nullptr;
        CUmipmappedArray mipmap = nullptr;
        CUarray array = nullptr;
        uint32_t width = 0, height = 0;
    };
    void Destroy(Texture &texture);
    bool EnsureScaleKernel();
    bool Scale(const Texture &texture, CUdeviceptr dst, size_t dstPitch, uint32_t width, uint32_t height,
               const float bounds[4]);

    CUcontext context_;
    std::map<uint64_t, Texture> textures_;
    CUmodule module_ = nullptr;
    CUfunction scaleKernel_ = nullptr;
    bool kernelFailed_ = false;
};

} // namespace lumen
