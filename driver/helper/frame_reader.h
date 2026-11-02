#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lumen {

// The capture helper's own Vulkan device, used to read the compositor's output.
// Swap textures are imported from their OPAQUE_FD memory and copied to a
// persistent host-visible buffer on demand.
class FrameReader {
public:
    ~FrameReader();

    // Imports a shared texture the compositor renders into. The image is
    // recreated here with the same parameters SteamVR used, which it has to
    // match for an opaque-fd import to see the same pixel layout.
    // allocationSize, if given, receives the imported allocation's size.
    bool Import(uint64_t sharedHandle, int memoryFd, uint32_t width, uint32_t height, VkFormat format,
                uint64_t *allocationSize = nullptr);
    void Forget(uint64_t sharedHandle);

    // Size of the side-by-side frame for these two textures.
    bool FrameSize(uint64_t left, uint64_t right, uint32_t &width, uint32_t &height);

    // Copies both eyes into one side-by-side RGBA frame (tightly packed rows
    // of width * 4 bytes). The pointer stays valid until the next call.
    const uint8_t *ReadSideBySide(uint64_t left, uint64_t right, uint32_t &width, uint32_t &height);

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    };

    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void *mapped = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };

    bool InitDevice();
    bool EnsureStaging(VkDeviceSize size);
    void DestroyStaging();
    void DestroyTexture(Texture &texture);
    int32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

    std::mutex mutex_;
    bool initialized_ = false;
    bool initFailed_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    std::map<uint64_t, Texture> textures_;
    Staging staging_;
};

// Writes 8-bit RGBA pixels as a PNG. Returns false on I/O error.
bool WritePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height);

} // namespace lumen
