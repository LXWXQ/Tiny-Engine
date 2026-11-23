#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

class Device;

// T-07 的第一步：队列窗口时间戳。
//
// 为什么先做这个：计划里 T-07 要把「一帧只在两个提交点、整帧一刀切」改成
// 按 FramePlan 分多个提交点。改之前必须先能回答一个问题：现在两条队列到底
// 有没有在 GPU 上重叠？没有这个数字，改完也无法判断是真收益还是噪声。
//
// 做法：每条命令缓冲头尾各写一枚 timestamp（同一个 query pool，按 frame slot
// 分四枚），下一轮复用到这个 slot 时读回（此时 Renderer 已经等过两条 timeline，
// 结果必然可用）。图形与计算的时间戳只有在 timestampComputeAndGraphics 为真、
// 或两条队列同族时才是同一时间域；否则只报各自耗时，不报重叠。
namespace tiny::queuestats
{
    void init(Device& device);
    void shutdown();

    bool enabled();
    bool comparable();   // 两条队列的时间戳是否同一时间域

    // 每帧开始：先收上一轮占用本 slot 的读数，再重置本 slot 的四枚 query。
    void beginFrame(uint32_t slot, uint64_t frameNumber);
    void writeBegin(VkCommandBuffer cmd, bool compute);
    void writeEnd(VkCommandBuffer cmd, bool compute);

    void report();
    bool selfTest();
}
