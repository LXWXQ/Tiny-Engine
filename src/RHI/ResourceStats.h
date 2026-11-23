#pragma once

#include "RHI/VmaConfig.h"   // 必须在 vk_mem_alloc.h 之前
#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include <cstdint>

// ===========================================================================
// T-04 的前置尺子：资源创建次数与显存占用
//
// 为什么要有这个文件：跨帧 RT 池（T-04）的验收写成「稳定态每帧 vkCreateImage 为 0、
// 显存不增长」，但在这之前这两件事都没法测 —— 创建点散在 9 个 vmaCreateImage 调用里，
// 显存从来没有被查询过（--texture-budget 只管降级策略，不管实际占用）。没有这两把
// 尺子，「池化之后不再每帧创建」只能靠读代码相信。
//
// 计数全是整数 ++；report 只在退出时调一次；显存采样每 60 帧一次 —— vmaGetHeapBudgets
// 要遍历堆内的 block，逐帧调没必要，10 Hz 足够看出斜率。
// ===========================================================================
namespace tiny::resourcestats
{
    // 与 framestats 同步：由 RenderGraph::beginFrame / endFrame 调用。
    void beginFrame();
    void endFrame();

    // 只在创建成功之后调用 —— 失败的创建不该计入。
    void countImage();
    void countBuffer();

    // 分配器必须在 device 销毁之前交进来；传 nullptr 关掉显存采样。
    void setAllocator(VmaAllocator allocator);

    void report();

    // 不需要 Vulkan 设备：只验帧边界复位、总计累加、峰值帧。
    bool selfTest();
}