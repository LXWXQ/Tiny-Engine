#include "ScreenshotCapture.h"
#include <iostream>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

ScreenshotCapture::~ScreenshotCapture()
{
    destroy();
}

void ScreenshotCapture::destroy()
{
    if (stagingBuffer != VK_NULL_HANDLE && allocator != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);
    stagingBuffer = VK_NULL_HANDLE;
    stagingAllocation = VK_NULL_HANDLE;
    stagingMapped = nullptr;
    stagingSize = 0;
    enabled = false;
    pendingReadback = false;
}

static bool isSupportedScreenshotFormat(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
}

void ScreenshotCapture::init(VkDevice dev, VmaAllocator alloc, VkFormat fmt,
                            VkExtent2D extent, bool en)
{
    device = dev;
    allocator = alloc;
    format = fmt;

#ifndef TINY_ENABLE_SCREENSHOT
    en = false;
    (void)extent;
#endif

    if (!en)
        return;

    if (!isSupportedScreenshotFormat(format))
    {
        std::cout << "[Diag] --screenshot: 交换链格式不支持读回（仅支持 8 位 BGRA/RGBA），已禁用\n";
        return;
    }

    createStaging(extent);
    enabled = (stagingBuffer != VK_NULL_HANDLE);
    if (enabled)
        std::cout << "[Diag] screenshot enabled: " << extent.width << "x" << extent.height << "\n";
}

void ScreenshotCapture::createStaging(VkExtent2D extent)
{
    if (stagingBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);
    stagingBuffer = VK_NULL_HANDLE;
    stagingAllocation = VK_NULL_HANDLE;
    stagingMapped = nullptr;

    capturedExtent = extent;
    stagingSize = extent.width * extent.height * 4;
    if (stagingSize == 0)
    {
        stagingSize = 0;
        return;
    }

    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = stagingSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocInfo.flags |= tiny_vmaNameFlag();
    allocInfo.pUserData = const_cast<char*>("ScreenshotStaging");

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
        &stagingBuffer, &stagingAllocation, &info) != VK_SUCCESS)
    {
        stagingBuffer = VK_NULL_HANDLE;
        stagingAllocation = VK_NULL_HANDLE;
        stagingSize = 0;
        return;
    }
    stagingMapped = info.pMappedData;
}

void ScreenshotCapture::onResize(VkExtent2D extent)
{
    if (!enabled)
        return;
    if (extent.width == capturedExtent.width && extent.height == capturedExtent.height)
        return;
    createStaging(extent);
}

void ScreenshotCapture::recordCopy(VkCommandBuffer cmd, VkImage image, VkExtent2D extent,
                                   VkImageLayout oldLayout, VkImageLayout newLayout)
{
    if (!enabled)
        return;
    if (extent.width != capturedExtent.width || extent.height != capturedExtent.height)
        createStaging(extent);
    if (stagingBuffer == VK_NULL_HANDLE)
        return;

    VkImageMemoryBarrier2 toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toTransfer.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = oldLayout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { extent.width, extent.height, 1 };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

    VkImageMemoryBarrier2 toPresent{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    toPresent.dstAccessMask = VK_ACCESS_2_NONE;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = newLayout;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = image;
    toPresent.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    dep.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(cmd, &dep);

    pendingReadback = true;
}

bool ScreenshotCapture::writePng(const std::string& path)
{
    if (!pendingReadback || stagingMapped == nullptr || stagingSize == 0)
        return false;

    const uint32_t w = capturedExtent.width;
    const uint32_t h = capturedExtent.height;
    const uint8_t* src = static_cast<const uint8_t*>(stagingMapped);
    const bool bgra = (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM);

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    const size_t pixels = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < pixels; ++i)
    {
        const uint8_t* p = src + i * 4;
        uint8_t* q = rgba.data() + i * 4;
        if (bgra) { q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = p[3]; }
        else      { q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3]; }
    }

    const int ok = stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h),
                                  4, rgba.data(), static_cast<int>(w * 4));
    pendingReadback = false;

    std::cout << "[Diag] screenshot " << (ok ? "written: " : "FAILED: ") << path
              << " (" << w << "x" << h << ")\n";
    return ok != 0;
}