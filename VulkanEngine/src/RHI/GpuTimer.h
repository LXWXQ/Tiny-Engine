#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// 每 pass 的 GPU 计时（P0-4）
//
// 开关模型（docs/60-debug-profiling/zero-overhead.md）：
//   * 编译期：SENTINEL_ENABLE_GPU_TIMING 关闭时，init() 直接不创建任何资源；
//   * 运行期：只有 --gpu-timing 时才 init(enabled=true)；
//   * 关闭时：queryPoolCount()==0，不分配、不写任何 GPU 命令。
//
// 时钟：vkCmdWriteTimestamp2 + 物理设备 limits.timestampPeriod。
// 读回时机：Renderer::beginFrame 已经等待过本 frame slot 的 timeline，
// 因此 onFrameBegin 里读回「上一轮使用本 slot 的帧」是安全的。
// ============================================================================
class GpuTimer
{
public:
    static constexpr uint32_t kMaxPassesPerFrame = 32;
    static constexpr uint32_t kQueriesPerPass = 2;   // begin + end

    ~GpuTimer();

    void init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t graphicsQueueFamily, uint32_t framesInFlight, bool enabled);
    void destroy();

    bool isEnabled() const { return enabled; }
    uint32_t queryPoolCount() const { return poolQueryCount; }
    size_t sampleCount() const { return samples.size(); }

    void onFrameBegin(VkCommandBuffer graphicsCmd, uint32_t frameSlot, uint64_t frameNumber);
    void beginPass(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t passIndex, const char* name);
    void endPass(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t passIndex);

    // 必须在 vkDeviceWaitIdle 之后调用，读回最后几帧的样本。
    void finalize();
    bool writeCsv(const std::string& path) const;

private:
    struct Sample
    {
        uint64_t frame = 0;
        std::string pass;
        double ms = 0.0;
    };
    struct SlotState
    {
        bool everUsed = false;
        uint64_t frameNumber = 0;
        uint32_t passCount = 0;
        std::vector<std::string> passNames;
    };

    void collectSlot(uint32_t frameSlot);

    VkDevice device = VK_NULL_HANDLE;
    VkQueryPool pool = VK_NULL_HANDLE;
    bool enabled = false;
    float timestampPeriodNs = 1.0f;
    uint32_t framesInFlight = 2;
    uint32_t poolQueryCount = 0;

    std::vector<SlotState> slots;
    std::vector<Sample> samples;
};