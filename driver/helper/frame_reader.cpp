#include "frame_reader.h"

#include <unistd.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace lumen {

namespace {

void Log(const std::string &msg) { std::fprintf(stderr, "lumen-capture: %s\n", msg.c_str()); }

// What SteamVR's NewSharedVulkanImage(renderable=true) appears to use. Must
// match the creator for the imported memory to be laid out the same way.
constexpr VkImageUsageFlags kSharedImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

} // namespace

FrameReader::~FrameReader() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        DestroyStaging();
        for (auto &[handle, texture] : textures_)
            DestroyTexture(texture);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_)
        vkDestroyInstance(instance_, nullptr);
}

bool FrameReader::InitDevice() {
    if (initialized_ || initFailed_)
        return initialized_;
    initFailed_ = true;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "lumen-capture";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    if (VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance_); r != VK_SUCCESS) {
        Log("vkCreateInstance failed: " + std::to_string(r));
        return false;
    }

    // The compositor renders on the discrete GPU; opaque fds only import on
    // the same device, so prefer it.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (!physicalDevice_ || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            physicalDevice_ = d;
    }
    if (!physicalDevice_) {
        Log("no Vulkan devices");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    Log(std::string("FrameReader using ") + props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);

    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, families.data());
    bool foundQueue = false;
    for (uint32_t i = 0; i < count && !foundQueue; ++i)
        if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) {
            queueFamily_ = i;
            foundQueue = true;
        }
    if (!foundQueue)
        return false;

    float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    if (VkResult r = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_); r != VK_SUCCESS) {
        Log("vkCreateDevice failed: " + std::to_string(r));
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);

    initialized_ = true;
    initFailed_ = false;
    return true;
}

int32_t FrameReader::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (memoryProperties_.memoryTypes[i].propertyFlags & flags) == flags)
            return static_cast<int32_t>(i);
    return -1;
}

void FrameReader::DestroyTexture(Texture &texture) {
    vkDestroyImage(device_, texture.image, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    texture = {};
}

bool FrameReader::Import(uint64_t sharedHandle, int memoryFd, uint32_t width, uint32_t height, VkFormat format,
                         uint64_t *allocationSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!InitDevice()) {
        close(memoryFd);
        return false;
    }

    Texture texture;
    texture.width = width;
    texture.height = height;

    VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = kSharedImageUsage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VkResult r = vkCreateImage(device_, &imageInfo, nullptr, &texture.image); r != VK_SUCCESS) {
        Log("Import: vkCreateImage failed: " + std::to_string(r));
        close(memoryFd);
        return false;
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device_, texture.image, &reqs);
    int32_t memoryType = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType < 0) {
        Log("Import: no device-local memory type");
        close(memoryFd);
        vkDestroyImage(device_, texture.image, nullptr);
        return false;
    }

    // A dedicated allocation, as SteamVR exports it (this has to match).
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.image = texture.image;
    VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importInfo.pNext = &dedicatedInfo;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    importInfo.fd = memoryFd;
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = static_cast<uint32_t>(memoryType);

    // A successful import takes ownership of the fd.
    if (VkResult r = vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory); r != VK_SUCCESS) {
        Log("Import: vkAllocateMemory(import) failed: " + std::to_string(r) + " size=" + std::to_string(reqs.size));
        close(memoryFd);
        vkDestroyImage(device_, texture.image, nullptr);
        return false;
    }
    if (VkResult r = vkBindImageMemory(device_, texture.image, texture.memory, 0); r != VK_SUCCESS) {
        Log("Import: vkBindImageMemory failed: " + std::to_string(r));
        DestroyTexture(texture);
        return false;
    }

    char msg[160];
    std::snprintf(msg, sizeof(msg), "imported 0x%llx %ux%u format %d, %llu bytes, memory type %d",
                  static_cast<unsigned long long>(sharedHandle), width, height, format,
                  static_cast<unsigned long long>(reqs.size), memoryType);
    Log(msg);
    textures_[sharedHandle] = texture;
    if (allocationSize)
        *allocationSize = reqs.size;
    return true;
}

void FrameReader::Forget(uint64_t sharedHandle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(sharedHandle);
    if (it == textures_.end())
        return;
    vkDeviceWaitIdle(device_);
    DestroyTexture(it->second);
    textures_.erase(it);
}

bool FrameReader::EnsureStaging(VkDeviceSize size) {
    if (staging_.size >= size)
        return true;
    DestroyStaging();

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging_.buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(device_, staging_.buffer, &reqs);

    // CPU reads from uncached (write-combined) memory are very slow, so
    // prefer cached memory and invalidate it after each copy.
    int32_t type = FindMemoryType(reqs.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0)
        type = FindMemoryType(reqs.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = static_cast<uint32_t>(type);
    if (type < 0 || vkAllocateMemory(device_, &allocInfo, nullptr, &staging_.memory) != VK_SUCCESS) {
        DestroyStaging();
        return false;
    }
    vkBindBufferMemory(device_, staging_.buffer, staging_.memory, 0);
    vkMapMemory(device_, staging_.memory, 0, VK_WHOLE_SIZE, 0, &staging_.mapped);
    staging_.size = size;
    Log(std::string("staging buffer ") + std::to_string(size) + " bytes, " +
        ((memoryProperties_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "cached"
                                                                                                    : "uncached"));

    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cmdInfo, &staging_.cmd);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &fenceInfo, nullptr, &staging_.fence);
    return true;
}

void FrameReader::DestroyStaging() {
    if (!device_)
        return;
    if (staging_.fence)
        vkDestroyFence(device_, staging_.fence, nullptr);
    if (staging_.cmd)
        vkFreeCommandBuffers(device_, commandPool_, 1, &staging_.cmd);
    if (staging_.buffer)
        vkDestroyBuffer(device_, staging_.buffer, nullptr);
    if (staging_.memory)
        vkFreeMemory(device_, staging_.memory, nullptr);
    staging_ = {};
}

bool FrameReader::FrameSize(uint64_t left, uint64_t right, uint32_t &width, uint32_t &height) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto l = textures_.find(left), r = textures_.find(right);
    if (l == textures_.end() || r == textures_.end() || l->second.height != r->second.height)
        return false;
    width = l->second.width + r->second.width;
    height = l->second.height;
    return true;
}

const uint8_t *FrameReader::ReadSideBySide(uint64_t left, uint64_t right, uint32_t &width, uint32_t &height) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto l = textures_.find(left), r = textures_.find(right);
    if (l == textures_.end() || r == textures_.end() || l->second.height != r->second.height)
        return nullptr;
    const Texture *eyes[2] = {&l->second, &r->second};
    width = eyes[0]->width + eyes[1]->width;
    height = eyes[0]->height;
    if (!EnsureStaging(VkDeviceSize(width) * height * 4))
        return nullptr;

    vkResetCommandBuffer(staging_.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(staging_.cmd, &begin);

    // We can't know the compositor's layout for these images. UNDEFINED is
    // technically allowed to discard contents, but in practice NVIDIA keeps
    // them.
    VkImageMemoryBarrier barriers[2];
    for (int eye = 0; eye < 2; ++eye) {
        VkImageMemoryBarrier &b = barriers[eye];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = eyes[eye]->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(staging_.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, barriers);

    // Pack both eyes into one row-major frame: each eye lands at its own
    // horizontal offset within rows of the full width.
    for (int eye = 0; eye < 2; ++eye) {
        VkBufferImageCopy region{};
        region.bufferOffset = eye == 0 ? 0 : VkDeviceSize(eyes[0]->width) * 4;
        region.bufferRowLength = width;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {eyes[eye]->width, eyes[eye]->height, 1};
        vkCmdCopyImageToBuffer(staging_.cmd, eyes[eye]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging_.buffer, 1, &region);
    }
    vkEndCommandBuffer(staging_.cmd);

    vkResetFences(device_, 1, &staging_.fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &staging_.cmd;
    if (vkQueueSubmit(queue_, 1, &submit, staging_.fence) != VK_SUCCESS ||
        vkWaitForFences(device_, 1, &staging_.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
        return nullptr;

    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = staging_.memory;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(device_, 1, &range);
    return static_cast<const uint8_t *>(staging_.mapped);
}

bool WritePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height) {
    // Each row is prefixed with filter type 0 (none).
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (width * 4 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t *row = rgba + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
            raw.push_back(255); // Compositor alpha is meaningless for display.
        }
    }
    uLongf compressedSize = compressBound(raw.size());
    std::vector<uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), raw.size(), 6) != Z_OK)
        return false;
    compressed.resize(compressedSize);

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    auto put32 = [](std::vector<uint8_t> &v, uint32_t x) {
        for (int s = 24; s >= 0; s -= 8)
            v.push_back(uint8_t(x >> s));
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        std::vector<uint8_t> out;
        put32(out, uint32_t(data.size()));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        put32(out, uint32_t(crc32(0, out.data() + 4, uInt(out.size() - 4))));
        std::fwrite(out.data(), 1, out.size(), f);
    };
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::fwrite(signature, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", compressed);
    chunk("IEND", {});
    return std::fclose(f) == 0;
}

} // namespace lumen
