#pragma once
#include <vulkan/vulkan.h>
#include "RHI/VmaConfig.h"
#include <vk_mem_alloc.h>
#include "RHI/VmaNaming.h"
#include <cstdint>
#include <string>

// ============================================================================
// P1-4：无头截图 + 视觉回归的采集端。
//
// 开关模型（docs/60-debug-profiling/zero-overhead.md）：
//   * 编译期：SENTINEL_ENABLE_SCREENSHOT 关闭时 init() 不创建任何资源；
//   * 运行期：只有 --screenshot 才启用；
//   * 关闭时：bufferBytes()==0，不分配、不录制任何命令。
//
// 采集方式：把交换链图像（Present 之后处于 PRESENT_SRC_KHR）拷贝到一块
// host-visible staging buffer，再由 writePng() 落成 PNG。
// ============================================================================
class ScreenshotCapture
{
public:
    ~ScreenshotCapture();

    void init(VkDevice device, VmaAllocator allocator, VkFormat swapchainFormat,
              VkExtent2D extent, bool enabled);
    void destroy();
    void onResize(VkExtent2D extent);

    bool isEnabled() const { return enabled; }
    bool hasPendingCopy() const { return pendingReadback; }
    uint32_t stagedBytes() const { return stagingSize; }   // 零开销守卫用

    void setTargetFrame(uint64_t frame) { targetFrame = frame; }
    bool isTargetFrame(uint64_t frameNumber) const { return enabled && frameNumber == targetFrame; }

    // 在 renderGraph->endFrame() 之后、renderer->endFrame() 之前录制。
    //
    // 默认参数对应交换链（present 之后处于 PRESENT_SRC_KHR）。
    // 拍编辑器离屏视口时传 (SHADER_READ_ONLY_OPTIMAL, SHADER_READ_ONLY_OPTIMAL)：
    // 拷完必须**转回原布局**，否则渲染图记录的布局与真实布局不一致，
    // 下一帧它会用错误的 oldLayout 插屏障，验证层立刻报错。
    void recordCopy(VkCommandBuffer cmd, VkImage image, VkExtent2D extent,
                    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VkImageLayout newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // 提交完成（调用方需先 vkDeviceWaitIdle）后写出 PNG。
    bool writePng(const std::string& path);

private:
    void createStaging(VkExtent2D extent);

    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool enabled = false;
    bool pendingReadback = false;
    uint64_t targetFrame = 1;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    uint32_t stagingSize = 0;
    VkExtent2D capturedExtent{};
};