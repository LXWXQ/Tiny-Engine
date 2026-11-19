#pragma once

// TM-05：驱动/硬件"怪癖表"。
//
// 为什么需要它：能力位不等于行为。同一份 features 位在不同厂商驱动上表现不同
// （分组操作在某些厂商的片元阶段会被静默忽略、异步 compute 在部分核显上反而更慢），
// 而这些差异一旦散落成十几处 `if (vendorId == 0x10DE)`，改一个判断要翻半个仓库。
// 这里把"探测结果"集中成一个纯数据结构，采集一次、全仓库共享，判断点只读字段。
//
// 纪律：
//   - 只放"探测得到的事实 + 由事实推出的策略开关"，不放临时止血常量；
//   - 新增字段必须有明确的消费点（或至少一条日志），否则就是死字段；
//   - 本结构不持有 Vulkan 句柄，也不做任何 API 调用 —— 采集在 detect 里一次完成。

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace tiny
{

struct DeviceQuirks
{
    // ---- 身份 ----
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t driverVersion = 0;
    VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool isNvidia = false;
    bool isAmd = false;
    bool isIntel = false;
    bool isIntegratedGpu = false;

    // ---- 分组操作（subgroup）----
    // supportedStages/Operations 是驱动自报的，不能只看 shader 能不能编译通过。
    VkSubgroupFeatureFlags subgroupStages = 0;
    uint32_t subgroupSize = 0;
    bool subgroupInFragment = false;
    bool subgroupInCompute = false;
    bool subgroupShuffle = false;
    // 由上面推出：分组操作是否可用于"跨帧复用"的优化路径（决定着色器走哪条分支）。
    bool subgroupOpsUsable = false;

    // ---- 队列与提交 ----
    bool hasDedicatedComputeQueue = false;
    // 后处理是否值得走异步 compute：双队列存在且不是核显（核显上抢同一个硬件队列，只会更慢）。
    bool asyncComputePost = true;
    // 提交后等队列空闲：只在排查 GPU 崩溃时开，正常路径必须关。
    bool queueWaitOnSubmission = false;
    // 强制整张渲染图单队列：跨队列依赖出问题时的诊断开关。
    bool forceSingleQueue = false;

    // 一行摘要，启动日志用。
    std::string describe() const;
};

// 采集一次。physicalDevice 与它的 properties 由调用方传入，避免重复查询。
DeviceQuirks detectDeviceQuirks(VkPhysicalDevice physicalDevice,
                                const VkPhysicalDeviceProperties& properties,
                                bool hasDedicatedComputeQueue);

} // namespace tiny