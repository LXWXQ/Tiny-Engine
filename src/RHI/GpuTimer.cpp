#include "GpuTimer.h"
#include <iostream>
#include <fstream>

namespace
{
    constexpr uint32_t kQueriesPerSlot =
        GpuTimer::kMaxPassesPerFrame * GpuTimer::kQueriesPerPass;
}

GpuTimer::~GpuTimer()
{
    destroy();
}

void GpuTimer::destroy()
{
    if (pool != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    poolQueryCount = 0;
    enabled = false;
    slots.clear();
}

void GpuTimer::init(VkDevice dev, VkPhysicalDevice phys,
                    uint32_t graphicsQueueFamily, uint32_t frames, bool en)
{
    device = dev;
    framesInFlight = (frames == 0) ? 1u : frames;
    slots.assign(framesInFlight, SlotState{});

#ifndef SENTINEL_ENABLE_GPU_TIMING
    // 编译期关闭：即使运行期给了 --gpu-timing 也不生效。
    en = false;
    (void)phys;
    (void)graphicsQueueFamily;
#endif

    if (!en)
        return;

    // 时间戳需要该队列族支持；timestampValidBits == 0 表示不支持。
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, nullptr);
    if (graphicsQueueFamily >= familyCount)
        return;
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, families.data());
    if (families[graphicsQueueFamily].timestampValidBits == 0)
    {
        std::cout << "[Diag] graphics queue family 不支持 timestamp，跳过 --gpu-timing\n";
        return;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    timestampPeriodNs = props.limits.timestampPeriod;

    poolQueryCount = framesInFlight * kQueriesPerSlot;

    VkQueryPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = poolQueryCount;
    if (vkCreateQueryPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
    {
        pool = VK_NULL_HANDLE;
        poolQueryCount = 0;
        std::cout << "[Diag] 创建 timestamp query pool 失败，--gpu-timing 关闭\n";
        return;
    }

    samples.reserve(4096);
    enabled = true;
}

void GpuTimer::onFrameBegin(VkCommandBuffer graphicsCmd, uint32_t frameSlot, uint64_t frameNumber)
{
    if (!enabled || frameSlot >= framesInFlight)
        return;

    // 上一轮使用本 slot 的帧此时已完成（Renderer 已等过 timeline）。
    collectSlot(frameSlot);

    SlotState& s = slots[frameSlot];
    s.everUsed = true;
    s.frameNumber = frameNumber;
    s.passCount = 0;
    s.passNames.clear();

    vkCmdResetQueryPool(graphicsCmd, pool, frameSlot * kQueriesPerSlot, kQueriesPerSlot);
}

void GpuTimer::beginPass(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t passIndex, const char* name)
{
    if (!enabled || frameSlot >= framesInFlight || passIndex >= kMaxPassesPerFrame)
        return;

    const uint32_t query = frameSlot * kQueriesPerSlot + passIndex * kQueriesPerPass;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, query);

    SlotState& s = slots[frameSlot];
    if (s.passNames.size() <= passIndex)
        s.passNames.resize(passIndex + 1);
    s.passNames[passIndex] = name ? name : "?";
    if (passIndex + 1 > s.passCount)
        s.passCount = passIndex + 1;
}

void GpuTimer::endPass(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t passIndex)
{
    if (!enabled || frameSlot >= framesInFlight || passIndex >= kMaxPassesPerFrame)
        return;

    const uint32_t query = frameSlot * kQueriesPerSlot + passIndex * kQueriesPerPass + 1;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, query);
}

void GpuTimer::collectSlot(uint32_t frameSlot)
{
    SlotState& s = slots[frameSlot];
    if (!s.everUsed || s.passCount == 0)
        return;

    const uint32_t base = frameSlot * kQueriesPerSlot;
    std::vector<uint64_t> data(static_cast<size_t>(s.passCount) * kQueriesPerPass, 0);

    const VkResult r = vkGetQueryPoolResults(
        device, pool, base, static_cast<uint32_t>(data.size()),
        data.size() * sizeof(uint64_t), data.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS)
        return;

    for (uint32_t i = 0; i < s.passCount; ++i)
    {
        const uint64_t t0 = data[static_cast<size_t>(i) * 2 + 0];
        const uint64_t t1 = data[static_cast<size_t>(i) * 2 + 1];
        Sample sample;
        sample.frame = s.frameNumber;
        sample.pass = (i < s.passNames.size()) ? s.passNames[i] : "?";
        sample.ms = (t1 > t0) ? static_cast<double>(t1 - t0) * timestampPeriodNs / 1e6 : 0.0;
        samples.push_back(std::move(sample));
    }
}

void GpuTimer::finalize()
{
    if (!enabled)
        return;
    for (uint32_t slot = 0; slot < framesInFlight; ++slot)
    {
        collectSlot(slot);
        slots[slot].everUsed = false;   // 防止重复收集
    }
}

bool GpuTimer::writeCsv(const std::string& path) const
{
    if (samples.empty())
        return false;

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
        return false;

    out << "frame,pass,gpu_ms\n";
    for (const Sample& s : samples)
        out << s.frame << "," << s.pass << "," << s.ms << "\n";
    return true;
}