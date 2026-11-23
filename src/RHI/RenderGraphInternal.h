#pragma once

#include <vulkan/vulkan.h>

// RenderGraph 内部共用的小工具。
//
// 放这里而不是各自文件的匿名命名空间：RenderGraph.cpp 与 RenderGraphBarrier.cpp
// 必须用同一份判定 —— 屏障该不该发、aspect 取哪一位，靠的就是这几个值。
// 复制两份的代价不是多写几行，而是某天只改了一处，另一处静默失效。
namespace rgdetail
{
    // 「写」访问位。写 -> 任何 都是 hazard。
    constexpr VkAccessFlags2 kWriteBits =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_HOST_WRITE_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT;

    inline bool isDepthFormat(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
        }
    }

    inline VkImageAspectFlags aspectOf(bool isDepth)
    {
        return isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }
}