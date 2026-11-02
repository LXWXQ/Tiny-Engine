#include "GpuProfiler.h"
#include <iostream>

GpuProfiler::~GpuProfiler()
{
    destroy();
}

void GpuProfiler::init(VkPhysicalDevice physicalDevice, VkDevice dev, VkQueue queue,
                       VkCommandPool commandPool, bool enabled)
{
    device = dev;

#ifndef SENTINEL_ENABLE_TRACY
    enabled = false;
    (void)physicalDevice;
    (void)queue;
    (void)commandPool;
#endif

    if (!enabled)
        return;

#ifdef SENTINEL_ENABLE_TRACY
    // Tracy 的 VkCtx 构造会自己 begin/end/submit/wait 这个命令缓冲，
    // 所以必须给它一个"初始状态"（未开始录制）的命令缓冲，之后再释放。
    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer tmp = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocInfo, &tmp) != VK_SUCCESS)
    {
        std::cout << "[Diag] Tracy: 临时命令缓冲分配失败，GPU zone 关闭\n";
        return;
    }

    context = TracyVkContext(physicalDevice, device, queue, tmp);
    vkFreeCommandBuffers(device, commandPool, 1, &tmp);

    if (context != nullptr)
        std::cout << "[Diag] Tracy Vulkan context created (GPU per-pass zones active)\n";
    else
        std::cout << "[Diag] Tracy Vulkan context creation failed\n";
#endif
}

void GpuProfiler::destroy()
{
#ifdef SENTINEL_ENABLE_TRACY
    if (context != nullptr)
        TracyVkDestroy(reinterpret_cast<TracyVkCtx>(context));
#endif
    context = nullptr;
}

void GpuProfiler::collect(VkCommandBuffer cmd) const
{
#ifdef SENTINEL_ENABLE_TRACY
    if (context != nullptr && cmd != VK_NULL_HANDLE)
        TracyVkCollect(reinterpret_cast<TracyVkCtx>(context), cmd);
#else
    (void)cmd;
#endif
}