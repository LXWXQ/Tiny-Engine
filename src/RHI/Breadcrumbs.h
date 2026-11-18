#pragma once

#include "RHI/VmaConfig.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class Device;

// ===========================================================================
// ② GPU 崩溃定位（breadcrumbs）
//
// 现状痛点：Device::reportDeviceFault() 已经能拿到 VK_EXT_device_fault 的
// 故障地址（src/RHI/Device.cpp:124-177），但那个地址翻译不回"哪个 pass、
// 哪一次绘制"。于是每次 device lost 都只能靠猜、靠注释掉 pass 二分，
// 一次十几分钟。而且一旦驱动不支持 device_fault，那条路直接 early return，
// 连地址都没有。
//
// 这里补上缺的那一半：在命令流里顺序埋"我到了这里"的标记，崩溃后把最后
// 到达的标记翻译回"第 N 帧 / 哪个 pass / 第几个对象"。
//
// 与 Granite vulkan/breadcrumbs.* 的异同：机制相同（CPU 侧表 + GPU 只带一个
// 32 位游标），做了四处适配：
//
//   1. 槽位是固定的 4 个（kFramesInFlight(2) × {graphics, compute}），
//      不需要 Granite 那套 8192 槽的动态分配/vacant 池。
//   2. 标记值编码成 (slot << 32 | counter)。于是 NV 的 64 位 checkpoint 能
//      直接带出槽位，不必像 Granite 那样把 index 和 counter 硬塞进 32 位。
//   3. 多一条降级路径：没有 NV/AMD 扩展时用 vkCmdUpdateBuffer 写游标
//      （代价是粒度降到 pass 级，因为它在渲染通道内非法）。
//   4. 游标缓冲是常驻映射的 HOST_VISIBLE|HOST_COHERENT 内存。理由很实在：
//      device lost 之后不能再提交任何命令，也就没有任何 copy 手段，只能
//      直接读 CPU 映射着的那块内存。
//
// 开销：NV checkpoint 是驱动内部写；AMD buffer marker 是一次 4 字节写。
// 每帧几十上百条，相对一个 pass 的工作量可忽略。因此默认就开着
// （崩溃只发生在用户机器上，只在 Debug 里开等于没开）。
// ===========================================================================
class Breadcrumbs
{
public:
    enum class Kind : uint32_t
    {
        Pass = 0,
        Draw = 1,
        Dispatch = 2,
        Copy = 3,
    };

    static constexpr uint32_t kSlotCount = 4;            // kFramesInFlight(2) × {graphics, compute}
    static constexpr uint32_t kMaxLabelLength = 40;      // 含结尾 '\0'
    static constexpr uint32_t kCompleted = 0xFFFFFFFFu;  // 该槽位跑完了全部命令

    // 槽位约定：Renderer/RenderGraph 只按这一个公式算，不要各自另写。
    static uint32_t slotFor(uint32_t frameSlot, bool compute)
    {
        return frameSlot * 2u + (compute ? 1u : 0u);
    }

    void init(Device& device);
    void shutdown();

    // 析构要自己收尾：Device 只 reset() 掉本对象，不会替我们调 shutdown()。
    ~Breadcrumbs();

    bool isActive() const { return active && enabled; }
    void setEnabled(bool value) { enabled = value; }
    const char* getModeName() const;

    // 命令缓冲开始/结束时调用（必须在渲染通道之外，必须紧挨 begin/end）。
    void beginSlot(VkCommandBuffer cmd, uint32_t slot, uint64_t frameNumber);
    void finishSlot(VkCommandBuffer cmd, uint32_t slot);

    // 渲染通道/逻辑区段的边界。beginPass 之后、endPass 之前的崩溃都归到
    // 这个名字上。endPass 必须在 vkCmdEndRendering 之后调用（降级路径
    // 的 vkCmdUpdateBuffer 在渲染通道内非法）。
    void beginPass(VkCommandBuffer cmd, uint32_t slot, const char* passName);
    void endPass(VkCommandBuffer cmd, uint32_t slot, const char* passName);

    // 更细粒度（每绘制/每 dispatch）。调用方自己保证不在渲染通道内
    // 使用降级路径 —— NV/AMD 路径没有这个限制。
    void checkpoint(VkCommandBuffer cmd, uint32_t slot, Kind kind, const char* label);

    // device lost 之后调用。queue 传提交失败的那条队列（VK_NULL_HANDLE
    // 表示两条队列都查）。返回写进 startup-error.log 的路径（空 = 没写盘）。
    std::string reportAfterDeviceLost(VkQueue queue, const std::string& directory = std::string());

    // 纯逻辑自检（不碰 GPU），沿用本项目的 selfTest 习惯。
    static bool selfTest();

private:
    enum class Mode
    {
        Disabled,
        NvCheckpoint,   // VK_NV_device_diagnostic_checkpoint：粒度到单个绘制
        AmdMarker,      // VK_AMD_buffer_marker：4 字节写
        UpdateBuffer,   // 只剩 vkCmdUpdateBuffer：只能 pass 粒度
    };

    struct Entry
    {
        uint32_t counter = 0;
        Kind     kind = Kind::Pass;
        char     label[kMaxLabelLength] = {};
    };

    struct Slot
    {
        bool                  recorded = false;
        uint64_t              frameNumber = 0;
        uint32_t              counter = 0;   // 0 = 还没有任何检查点
        std::vector<Entry>    entries;
    };

    static uint64_t encodeMarker(uint32_t slot, uint32_t counter)
    {
        return (static_cast<uint64_t>(slot) << 32) | static_cast<uint64_t>(counter);
    }

    void emitMarker(VkCommandBuffer cmd, uint32_t slot, uint32_t value, bool insideRenderPass);

    // 写入游标前的那道最小屏障（只覆盖这 4 个字节、只声明 TRANSFER_WRITE）。
    void emitCursorWriteBarrier(VkCommandBuffer cmd, uint32_t slot);
    uint32_t pushEntry(uint32_t slot, Kind kind, const char* label);

    void collectCursorsFromQueue(VkQueue queue, uint32_t* cursors) const;
    void collectCursorsFromMemory(uint32_t* cursors) const;

    std::string formatSlot(uint32_t slot, uint32_t cursor) const;

    Device* device = nullptr;
    Mode    mode = Mode::Disabled;
    bool    active = false;
    bool    enabled = true;

    VkBuffer      cursorBuffer = VK_NULL_HANDLE;
    VmaAllocation cursorAllocation = VK_NULL_HANDLE;
    uint32_t*     cursorMapped = nullptr;

    std::array<Slot, kSlotCount> slots{};

    PFN_vkCmdSetCheckpointNV        pfnCmdSetCheckpointNV = nullptr;
    PFN_vkGetQueueCheckpointData2NV pfnGetQueueCheckpointData2NV = nullptr;
    PFN_vkCmdWriteBufferMarkerAMD   pfnCmdWriteBufferMarkerAMD = nullptr;
};