#pragma once
#include <vulkan/vulkan.h>

// ============================================================================
// P1-1：Tracy GPU 剖析上下文（每 pass 一个 zone）。
//
// 开关模型：
//   * 编译期：SENTINEL_ENABLE_TRACY 关闭时本类不创建任何资源，宏也展开为空；
//   * 运行期：Tracy 构建配合 TRACY_ON_DEMAND —— 没有 server 连接时 zone 跳过，
//             近似零开销（见 CMakeLists 的 Tracy 块）。
//
// 用法（RenderGraph 的 pass 循环里，作用域块内一行）：
//   SENTINEL_GPU_ZONE_SCOPE(gpuProfiler, passCmd, pass.name.c_str(), !pass.isCompute);
// 它声明一个 RAII 变量，zone 覆盖到所在作用域结束。
// ============================================================================
class GpuProfiler
{
public:
    ~GpuProfiler();

    void init(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue,
              VkCommandPool commandPool, bool enabled);
    void destroy();

    bool isEnabled() const { return context != nullptr; }
    void* rawContext() const { return context; }

    // 每帧结束时在 frame 命令缓冲里调用一次（收集上一帧的 GPU 数据）。
    void collect(VkCommandBuffer cmd) const;

private:
    VkDevice device = VK_NULL_HANDLE;
    void* context = nullptr;
};

#ifdef SENTINEL_ENABLE_TRACY
#  include "tracy/TracyVulkan.hpp"
#  define SENTINEL_GPU_ZONE_SCOPE(prof, cmd, name, active)                       \
     TracyVkZoneTransient(reinterpret_cast<TracyVkCtx>((prof) != nullptr ? (prof)->rawContext() : nullptr), \
                          ___tracy_vk_zone, cmd, name,                           \
                          ((prof) != nullptr && (prof)->isEnabled() && (active)))
#else
#  define SENTINEL_GPU_ZONE_SCOPE(prof, cmd, name, active)                       \
     do { (void)(prof); (void)(cmd); (void)(name); (void)(active); } while (0)
#endif