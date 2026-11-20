#pragma once

#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>

// ---------------------------------------------------------------------------
// TM-18：惰性分配（transient attachment）能力探测
//
// 为什么只探测、不改分配策略：
//   `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` 的内存只存在于带 tile 的 GPU
//   （移动端、部分集显）上；参考实现就是把整块 tile memory 当渲染目标，
//   用完即丢。桌面独显没有这个内存类型，改成 TRANSIENT_ATTACHMENT 只会让
//   驱动退化成普通分配，白折腾一遍。所以这一项的落地顺序是：先量本机能力、
//   记录结论，真到有移动端目标时再改分配器 —— 与 TM-06 / TM-08 / TM-14 同一套路。
//
// 判据只有两条（注意：Vulkan 没有「格式支持惰性分配」这种格式特性位，
// 这一点与指南里最初的写法不同，以代码为准）：
//   1) 驱动接受 TRANSIENT_ATTACHMENT 这个用法组合
//      —— vkGetPhysicalDeviceImageFormatProperties 返回 VK_SUCCESS；
//   2) 设备上真的存在 LAZILY_ALLOCATED 的内存类型。
//   第 2 条在桌面独显上几乎总是 0，这就是本项"探测完就收工"的原因。
// ---------------------------------------------------------------------------
namespace tiny::transient
{
    // 候选目标：名字 + 格式。用法固定为
    // TRANSIENT_ATTACHMENT | COLOR_ATTACHMENT | INPUT_ATTACHMENT
    // （transient 不能和 storage / transfer 共存）。
    struct Target
    {
        const char* name = "";
        VkFormat format = VK_FORMAT_UNDEFINED;
        // 深度/模板目标要用 DEPTH_STENCIL_ATTACHMENT，不能和 COLOR_ATTACHMENT 混着问；
        // 实测把深度格式按颜色用法去查会被驱动直接拒绝，结论就变成"不支持"了。
        bool depthStencil = false;
    };

    struct Result
    {
        const char* name = "";
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool transientUsageSupported = false;
        bool viable = false;          // 两条判据都成立
        const char* verdict = "";
    };

    // 本设备有多少个 LAZILY_ALLOCATED 内存类型（桌面独显通常 0）。
    uint32_t countLazilyAllocatedMemoryTypes(VkPhysicalDevice physicalDevice);

    // 纯逻辑判定，供 probe() 与 selfTest() 共用。
    const char* classify(bool transientUsageSupported, uint32_t lazilyAllocatedMemoryTypes);

    std::vector<Result> probe(VkPhysicalDevice physicalDevice, const std::vector<Target>& targets);

    void print(const std::vector<Result>& results, uint32_t lazilyAllocatedMemoryTypes,
               std::FILE* out);

    bool selfTest();
}
