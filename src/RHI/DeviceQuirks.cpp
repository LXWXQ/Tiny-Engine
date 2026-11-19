#include "RHI/DeviceQuirks.h"

#include <cstdio>

namespace tiny
{
namespace
{
constexpr uint32_t kNvidia = 0x10DE;
constexpr uint32_t kAmd = 0x1002;
constexpr uint32_t kIntel = 0x8086;
} // namespace

std::string DeviceQuirks::describe() const
{
    const char* vendor = isNvidia ? "NVIDIA" : isAmd ? "AMD" : isIntel ? "Intel" : "其他";
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "%s | %s | subgroup={size=%u stages=0x%X shuffle=%d} | 异步compute=%d | "
                  "独立compute队列=%d",
                  vendor,
                  isIntegratedGpu ? "集成显卡" : "独立显卡",
                  subgroupSize,
                  static_cast<unsigned>(subgroupStages),
                  subgroupShuffle ? 1 : 0,
                  asyncComputePost ? 1 : 0,
                  hasDedicatedComputeQueue ? 1 : 0);
    return std::string(buffer);
}

DeviceQuirks detectDeviceQuirks(VkPhysicalDevice physicalDevice,
                                const VkPhysicalDeviceProperties& properties,
                                bool hasDedicatedComputeQueue)
{
    DeviceQuirks quirks;

    quirks.vendorId = properties.vendorID;
    quirks.deviceId = properties.deviceID;
    quirks.driverVersion = properties.driverVersion;
    quirks.deviceType = properties.deviceType;
    quirks.isNvidia = quirks.vendorId == kNvidia;
    quirks.isAmd = quirks.vendorId == kAmd;
    quirks.isIntel = quirks.vendorId == kIntel;
    quirks.isIntegratedGpu = quirks.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

    quirks.subgroupStages = subgroup.supportedStages;
    quirks.subgroupSize = subgroup.subgroupSize;

    // 只认"这个阶段真的报告支持"，不做"编译能过所以大概行"的推断。
    const bool basic = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
    quirks.subgroupInFragment = basic && (subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0;
    quirks.subgroupInCompute = basic && (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    quirks.subgroupShuffle = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
    quirks.subgroupOpsUsable = quirks.subgroupInCompute && quirks.subgroupInFragment;
    if (quirks.subgroupSize == 0)
        quirks.subgroupOpsUsable = false;

    quirks.hasDedicatedComputeQueue = hasDedicatedComputeQueue;
    // 核显上"异步" compute 与图形共用同一批 EU，抢占只会让总时间变长，所以默认关。
    quirks.asyncComputePost = hasDedicatedComputeQueue && !quirks.isIntegratedGpu;

    return quirks;
}

} // namespace tiny