#include "QueueStats.h"
#include "Device.h"

#include <cstdio>
#include <iostream>
#include <vector>

namespace tiny::queuestats
{
namespace
{
    // 每个 frame slot 四枚：graphics begin/end、compute begin/end。
    constexpr uint32_t kQueriesPerSlot = 4;
    constexpr uint32_t kSlotCount = 3;

    VkDevice g_device = VK_NULL_HANDLE;
    VkPhysicalDevice g_physical = VK_NULL_HANDLE;
    VkQueryPool g_pool = VK_NULL_HANDLE;
    double g_periodNs = 1.0;
    uint32_t g_currentSlot = 0;
    bool g_enabled = false;
    bool g_comparable = false;
    bool g_dedicatedCompute = false;
    bool g_slotPending[kSlotCount] = {};

    struct Totals
    {
        uint64_t samples = 0;
        double graphicsMs = 0.0;
        double computeMs = 0.0;
        double overlapMs = 0.0;
        double makespanMs = 0.0;
        double maxOverlapMs = 0.0;
    };

    Totals g_total;

    double toMs(uint64_t ticks)
    {
        return static_cast<double>(ticks) * g_periodNs / 1e6;
    }

    // 两个窗口的重叠长度（ticks）。抽成纯函数是为了能自检。
    uint64_t overlapTicks(uint64_t aBegin, uint64_t aEnd, uint64_t bBegin, uint64_t bEnd)
    {
        if (aEnd <= aBegin || bEnd <= bBegin)
            return 0;
        const uint64_t begin = aBegin > bBegin ? aBegin : bBegin;
        const uint64_t end = aEnd < bEnd ? aEnd : bEnd;
        return end > begin ? end - begin : 0;
    }

    // 两个窗口合起来的跨度：从最早开始到最晚结束。
    uint64_t unionTicks(uint64_t aBegin, uint64_t aEnd, uint64_t bBegin, uint64_t bEnd)
    {
        if (aEnd <= aBegin)
            return bEnd > bBegin ? bEnd - bBegin : 0;
        if (bEnd <= bBegin)
            return aEnd - aBegin;
        const uint64_t begin = aBegin < bBegin ? aBegin : bBegin;
        const uint64_t end = aEnd > bEnd ? aEnd : bEnd;
        return end - begin;
    }

    bool familySupportsTimestamps(VkPhysicalDevice phys, uint32_t family)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
        if (family >= count)
            return false;
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
        return families[family].timestampValidBits > 0;
    }

    void collectSlot(uint32_t slot)
    {
        if (!g_slotPending[slot])
            return;

        uint64_t data[kQueriesPerSlot] = { 0, 0, 0, 0 };
        const VkResult r = vkGetQueryPoolResults(
            g_device, g_pool, slot * kQueriesPerSlot, kQueriesPerSlot,
            sizeof(data), data, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS)
        {
            g_slotPending[slot] = false;
            return;
        }

        const uint64_t gBegin = data[0];
        const uint64_t gEnd = data[1];
        const uint64_t cBegin = data[2];
        const uint64_t cEnd = data[3];

        const double graphicsMs = toMs(gEnd > gBegin ? gEnd - gBegin : 0);
        const double computeMs = toMs(cEnd > cBegin ? cEnd - cBegin : 0);
        const double overlapMs = g_comparable ? toMs(overlapTicks(gBegin, gEnd, cBegin, cEnd)) : 0.0;
        const double makespanMs = g_comparable ? toMs(unionTicks(gBegin, gEnd, cBegin, cEnd))
                                               : graphicsMs + computeMs;

        Totals& t = g_total;
        ++t.samples;
        t.graphicsMs += graphicsMs;
        t.computeMs += computeMs;
        t.overlapMs += overlapMs;
        t.makespanMs += makespanMs;
        if (overlapMs > t.maxOverlapMs)
            t.maxOverlapMs = overlapMs;

        g_slotPending[slot] = false;
    }
}

void init(Device& device)
{
    g_device = device.getDevice();
    g_physical = device.getPhysicalDevice();

    const QueueFamilyIndices families = device.findPhysicalQueueFamilies();
    if (!families.graphicsFamily.has_value() || !families.computeFamily.has_value())
    {
        std::cout << "[Queue] 队列族信息不完整，--stats-queue 关闭\n";
        return;
    }

    g_dedicatedCompute = device.hasDedicatedComputeQueue();
    if (!familySupportsTimestamps(g_physical, families.graphicsFamily.value()))
    {
        std::cout << "[Queue] graphics queue family 不支持 timestamp，--stats-queue 关闭\n";
        return;
    }
    if (g_dedicatedCompute && !familySupportsTimestamps(g_physical, families.computeFamily.value()))
    {
        std::cout << "[Queue] compute queue family 不支持 timestamp，--stats-queue 关闭\n";
        return;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_physical, &props);
    g_periodNs = static_cast<double>(props.limits.timestampPeriod);

    // 同一时间域的两个条件：厂商声明所有队列时间戳可比，或两条队列同族。
    g_comparable = (props.limits.timestampComputeAndGraphics != VK_FALSE) || !g_dedicatedCompute;

    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = kSlotCount * kQueriesPerSlot;
    if (vkCreateQueryPool(g_device, &poolInfo, nullptr, &g_pool) != VK_SUCCESS)
    {
        g_pool = VK_NULL_HANDLE;
        std::cout << "[Queue] 创建 timestamp query pool 失败，--stats-queue 关闭\n";
        return;
    }

    g_enabled = true;
}

void shutdown()
{
    if (g_pool != VK_NULL_HANDLE && g_device != VK_NULL_HANDLE)
        vkDestroyQueryPool(g_device, g_pool, nullptr);
    g_pool = VK_NULL_HANDLE;
    g_enabled = false;
}

bool enabled() { return g_enabled; }
bool comparable() { return g_comparable; }

void beginFrame(uint32_t slot, uint64_t frameNumber)
{
    (void)frameNumber;
    if (!g_enabled || slot >= kSlotCount)
        return;

    // 复用到这个 slot 时，上一轮占用它的两次提交都已完成（Renderer 等过 timeline）。
    collectSlot(slot);
    g_currentSlot = slot;
    g_slotPending[slot] = true;
}

void writeBegin(VkCommandBuffer cmd, bool compute)
{
    if (!g_enabled || cmd == VK_NULL_HANDLE)
        return;
    // 这个设备的 hostQueryReset 没开，所以重置必须录进命令缓冲里。
    // 两条队列各管自己的一对 query，谁先执行都不会把对方的读数清掉。
    const uint32_t base = g_currentSlot * kQueriesPerSlot + (compute ? 2u : 0u);
    vkCmdResetQueryPool(cmd, g_pool, base, 2);
    const uint32_t query = base;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_pool, query);
}

void writeEnd(VkCommandBuffer cmd, bool compute)
{
    if (!g_enabled || cmd == VK_NULL_HANDLE)
        return;
    const uint32_t query = g_currentSlot * kQueriesPerSlot + (compute ? 3u : 1u);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pool, query);
}

void report()
{
    if (!g_enabled)
    {
        std::cout << "[Queue] --stats-queue 未启用（队列族不支持 timestamp 或未初始化）\n";
        return;
    }
    if (g_total.samples == 0)
    {
        std::cout << "[Queue] 没有采集到样本\n";
        return;
    }

    const double n = static_cast<double>(g_total.samples);
    const double graphicsMs = g_total.graphicsMs / n;
    const double computeMs = g_total.computeMs / n;
    const double overlapMs = g_total.overlapMs / n;
    const double makespanMs = g_total.makespanMs / n;

    const double graphicsPct = makespanMs > 0.0 ? graphicsMs * 100.0 / makespanMs : 0.0;
    const double computePct = makespanMs > 0.0 ? computeMs * 100.0 / makespanMs : 0.0;
    const double hiddenPct = computeMs > 0.01 ? overlapMs * 100.0 / computeMs : 0.0;

    std::printf("[Queue] frames=%llu | makespan=%.2f ms | graphics busy=%.2f ms (%.1f%%) | compute busy=%.2f ms (%.1f%%) | overlap=%.2f ms (%.1f%% of compute, max %.2f ms)\n",
        static_cast<unsigned long long>(g_total.samples), makespanMs, graphicsMs, graphicsPct,
        computeMs, computePct, overlapMs, hiddenPct, g_total.maxOverlapMs);
    std::printf("[Queue] dedicatedCompute=%d sameClockDomain=%d | compute 被图形遮住 %.1f%%\n",
        g_dedicatedCompute ? 1 : 0, g_comparable ? 1 : 0, hiddenPct);
}

bool selfTest()
{
    const bool a = overlapTicks(0, 100, 50, 200) == 50;
    const bool b = overlapTicks(0, 100, 100, 200) == 0;
    const bool c = overlapTicks(40, 60, 0, 100) == 20;
    const bool d = unionTicks(0, 100, 50, 200) == 200;
    const bool e = unionTicks(0, 100, 200, 300) == 300;
    const bool f = overlapTicks(0, 0, 0, 0) == 0;
    return a && b && c && d && e && f;
}
}
