#pragma once
#include <cstdint>

// ============================================================================
// TM-17：引擎里"状态变化"类事件的载荷
//
// 这里刻意只用整数，不引用 VkExtent2D / VkFormat —— Core 里的订阅者
// （截图、渲染管线）不需要为了一个事件把 vulkan.h 再 include 一遍；
// 发布端做一次转换就好。
// ============================================================================

namespace tiny::events {

// 交换链重建完成。renderWidth/renderHeight 是重建后渲染目标应取的尺寸
// （动态分辨率时与 swapchain 尺寸不同）。
struct SwapchainRecreated
{
    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t imageCount = 0;
    int32_t format = 0;
    bool firstCreation = false;
};

// 渲染目标尺寸变化（动态分辨率），交换链本身没动。
struct RenderTargetsResized
{
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace tiny::events
