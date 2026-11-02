#include "cuda_frames.h"

#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include <nvrtc.h>

namespace lumen {

namespace {

bool Check(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS)
        return true;
    const char *name = nullptr;
    cuGetErrorName(r, &name);
    std::fprintf(stderr, "lumen-capture: %s failed: %s\n", what, name ? name : "?");
    return false;
}

struct ContextScope {
    explicit ContextScope(CUcontext c) { cuCtxPushCurrent(c); }
    ~ContextScope() {
        CUcontext popped;
        cuCtxPopCurrent(&popped);
    }
};

} // namespace

CudaFrames::~CudaFrames() {
    ContextScope scope(context_);
    if (module_)
        cuModuleUnload(module_);
    for (auto &[handle, texture] : textures_)
        Destroy(texture);
}

void CudaFrames::Destroy(Texture &texture) {
    if (texture.mipmap)
        cuMipmappedArrayDestroy(texture.mipmap);
    if (texture.memory)
        cuDestroyExternalMemory(texture.memory);
    texture = {};
}

bool CudaFrames::Import(uint64_t handle, int memoryFd, uint64_t allocationSize, uint32_t width, uint32_t height) {
    ContextScope scope(context_);
    Forget(handle);

    Texture texture;
    texture.width = width;
    texture.height = height;

    // Whether the allocation is dedicated has to match the exporter. Try
    // dedicated first (what worked on the Vulkan side), then not.
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memDesc{};
    memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    memDesc.handle.fd = memoryFd;
    memDesc.size = allocationSize;
    memDesc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
    CUresult r = cuImportExternalMemory(&texture.memory, &memDesc);
    if (r != CUDA_SUCCESS) {
        memDesc.flags = 0;
        r = cuImportExternalMemory(&texture.memory, &memDesc);
    }
    if (!Check(r, "cuImportExternalMemory")) {
        close(memoryFd); // Only a successful import takes ownership.
        return false;
    }

    // Same shape and usage as the Vulkan image (RGBA8, color attachment), so
    // CUDA interprets the driver's tiled layout the same way.
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC arrayDesc{};
    arrayDesc.offset = 0;
    arrayDesc.arrayDesc.Width = width;
    arrayDesc.arrayDesc.Height = height;
    arrayDesc.arrayDesc.Depth = 0;
    arrayDesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    arrayDesc.arrayDesc.NumChannels = 4;
    arrayDesc.arrayDesc.Flags = CUDA_ARRAY3D_COLOR_ATTACHMENT;
    arrayDesc.numLevels = 1;
    if (!Check(cuExternalMemoryGetMappedMipmappedArray(&texture.mipmap, texture.memory, &arrayDesc),
               "cuExternalMemoryGetMappedMipmappedArray") ||
        !Check(cuMipmappedArrayGetLevel(&texture.array, texture.mipmap, 0), "cuMipmappedArrayGetLevel")) {
        Destroy(texture);
        return false;
    }

    std::fprintf(stderr, "lumen-capture: CUDA imported 0x%llx (%s allocation)\n",
                 static_cast<unsigned long long>(handle),
                 memDesc.flags ? "dedicated" : "non-dedicated");
    textures_[handle] = texture;
    return true;
}

void CudaFrames::Forget(uint64_t handle) {
    auto it = textures_.find(handle);
    if (it == textures_.end())
        return;
    ContextScope scope(context_);
    Destroy(it->second);
    textures_.erase(it);
}

// Scales one eye's picture area (u0, v0 plus su x sv of the texture, in 0..1
// coordinates) to fill width x height: each output pixel averages 4 bilinear
// taps of the source (a 2x2 pattern inside the pixel), which keeps text
// readable at the ~1.6x reductions games need, where plain bilinear would alias.
static const char *kScaleSource = R"(
extern "C" __global__ void scale(cudaTextureObject_t src, unsigned char *dst, unsigned long long pitch,
                                 int width, int height, float u0, float v0, float su, float sv) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    float u = u0 + su * (x + 0.5f) / width, v = v0 + sv * (y + 0.5f) / height;
    float du = 0.25f * su / width, dv = 0.25f * sv / height;
    float4 a = tex2D<float4>(src, u - du, v - dv), b = tex2D<float4>(src, u + du, v - dv);
    float4 c = tex2D<float4>(src, u - du, v + dv), d = tex2D<float4>(src, u + du, v + dv);
    unsigned char *p = dst + y * pitch + x * 4;
    p[0] = (unsigned char)(255.f * 0.25f * (a.x + b.x + c.x + d.x) + 0.5f);
    p[1] = (unsigned char)(255.f * 0.25f * (a.y + b.y + c.y + d.y) + 0.5f);
    p[2] = (unsigned char)(255.f * 0.25f * (a.z + b.z + c.z + d.z) + 0.5f);
    p[3] = 255;
}
)";

bool CudaFrames::EnsureScaleKernel() {
    if (scaleKernel_ || kernelFailed_)
        return scaleKernel_ != nullptr;
    kernelFailed_ = true;
    // Compile for this GPU at runtime (NVRTC), so the build needs no nvcc.
    CUdevice device;
    int major = 0, minor = 0;
    cuCtxGetDevice(&device);
    cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device);
    cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device);
    std::string arch = "--gpu-architecture=compute_" + std::to_string(major * 10 + minor);
    const char *options[] = {arch.c_str(), "--use_fast_math"};

    nvrtcProgram program;
    nvrtcCreateProgram(&program, kScaleSource, "scale.cu", 0, nullptr, nullptr);
    nvrtcResult result = nvrtcCompileProgram(program, 2, options);
    if (result != NVRTC_SUCCESS) {
        size_t size = 0;
        nvrtcGetProgramLogSize(program, &size);
        std::string log(size, '\0');
        nvrtcGetProgramLog(program, log.data());
        std::fprintf(stderr, "lumen-capture: scale kernel compile failed: %s\n", log.c_str());
        nvrtcDestroyProgram(&program);
        return false;
    }
    size_t ptxSize = 0;
    nvrtcGetPTXSize(program, &ptxSize);
    std::vector<char> ptx(ptxSize);
    nvrtcGetPTX(program, ptx.data());
    nvrtcDestroyProgram(&program);
    if (!Check(cuModuleLoadData(&module_, ptx.data()), "cuModuleLoadData") ||
        !Check(cuModuleGetFunction(&scaleKernel_, module_, "scale"), "cuModuleGetFunction"))
        return false;
    kernelFailed_ = false;
    std::fprintf(stderr, "lumen-capture: GPU scaler ready (%s)\n", arch.c_str() + 19);
    return true;
}

bool CudaFrames::Scale(const Texture &texture, CUdeviceptr dst, size_t dstPitch, uint32_t width, uint32_t height,
                       const float bounds[4]) {
    if (!EnsureScaleKernel())
        return false;
    CUDA_RESOURCE_DESC resource{};
    resource.resType = CU_RESOURCE_TYPE_ARRAY;
    resource.res.array.hArray = texture.array;
    CUDA_TEXTURE_DESC desc{};
    desc.addressMode[0] = desc.addressMode[1] = CU_TR_ADDRESS_MODE_CLAMP;
    desc.filterMode = CU_TR_FILTER_MODE_LINEAR;
    desc.flags = CU_TRSF_NORMALIZED_COORDINATES; // reads give 0..1 floats (no CU_TRSF_READ_AS_INTEGER)
    CUtexObject object;
    if (!Check(cuTexObjectCreate(&object, &resource, &desc, nullptr), "cuTexObjectCreate"))
        return false;
    unsigned long long pitch = dstPitch;
    int w = int(width), h = int(height);
    float u0 = bounds[0], v0 = bounds[1], su = bounds[2] - bounds[0], sv = bounds[3] - bounds[1];
    void *args[] = {&object, &dst, &pitch, &w, &h, &u0, &v0, &su, &sv};
    bool ok = Check(cuLaunchKernel(scaleKernel_, (width + 15) / 16, (height + 15) / 16, 1, 16, 16, 1, 0, nullptr,
                                   args, nullptr),
                    "cuLaunchKernel");
    cuStreamSynchronize(nullptr);
    cuTexObjectDestroy(object);
    return ok;
}

bool CudaFrames::CopySideBySide(uint64_t left, uint64_t right, CUdeviceptr dst, size_t dstPitch, uint32_t eyeWidth,
                                uint32_t eyeHeight, const float bounds[4]) {
    bool whole = bounds[0] == 0.f && bounds[1] == 0.f && bounds[2] == 1.f && bounds[3] == 1.f;
    auto l = textures_.find(left), r = textures_.find(right);
    if (l == textures_.end() || r == textures_.end())
        return false;
    ContextScope scope(context_);
    const Texture *eyes[2] = {&l->second, &r->second};
    for (int eye = 0; eye < 2; ++eye) {
        CUdeviceptr eyeDst = dst + (eye == 0 ? 0 : size_t(eyeWidth) * 4);
        if (!whole || eyes[eye]->width != eyeWidth || eyes[eye]->height != eyeHeight) {
            if (!Scale(*eyes[eye], eyeDst, dstPitch, eyeWidth, eyeHeight, bounds))
                return false;
            continue;
        }
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.srcArray = eyes[eye]->array;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = eyeDst;
        copy.dstPitch = dstPitch;
        copy.WidthInBytes = size_t(eyeWidth) * 4;
        copy.Height = eyeHeight;
        if (!Check(cuMemcpy2DAsync(&copy, nullptr), "cuMemcpy2DAsync"))
            return false;
    }
    return Check(cuStreamSynchronize(nullptr), "cuStreamSynchronize");
}

} // namespace lumen
