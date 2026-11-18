#include "RHI/Breadcrumbs.h"
#include "RHI/Device.h"
#include "RHI/VmaNaming.h"
#include "Core/StartupLog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

void Breadcrumbs::init(Device& device)
{
    this->device = &device;

    // 按优先级挑一种标记方式。NV 的 checkpoint 是驱动内部记录，
    // 崩溃后能一次性取回整条队列上的全部 checkpoint，粒度也最细，
    // 所以优先。
    if (device.hasNvDiagnosticCheckpoints())
    {
        pfnCmdSetCheckpointNV = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
            vkGetDeviceProcAddr(device.getDevice(), "vkCmdSetCheckpointNV"));
        pfnGetQueueCheckpointData2NV = reinterpret_cast<PFN_vkGetQueueCheckpointData2NV>(
            vkGetDeviceProcAddr(device.getDevice(), "vkGetQueueCheckpointData2NV"));

        if (pfnCmdSetCheckpointNV != nullptr && pfnGetQueueCheckpointData2NV != nullptr)
            mode = Mode::NvCheckpoint;
    }

    if (mode == Mode::Disabled && device.hasAmdBufferMarker())
    {
        pfnCmdWriteBufferMarkerAMD = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(
            vkGetDeviceProcAddr(device.getDevice(), "vkCmdWriteBufferMarkerAMD"));

        if (pfnCmdWriteBufferMarkerAMD != nullptr)
            mode = Mode::AmdMarker;
    }

    // NV 路径把游标存在驱动内部，不需要我们自己的缓冲；
    // AMD 路径和降级路径都要一块 4×4 字节的常驻映射内存。
    if (mode != Mode::NvCheckpoint)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kSlotCount * sizeof(uint32_t);
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // HOST_ACCESS_RANDOM + MAPPED：device lost 之后不能再提交任何命令，
        // 也就没有任何 copy 手段，只能直接读 CPU 映射着的这块内存。
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT |
                          tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>("breadcrumbs_cursor");

        VmaAllocationInfo allocationInfo{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &cursorBuffer, &cursorAllocation, &allocationInfo) == VK_SUCCESS)
        {
            cursorMapped = static_cast<uint32_t*>(allocationInfo.pMappedData);
            if (cursorMapped != nullptr)
                std::memset(cursorMapped, 0, static_cast<size_t>(bufferInfo.size));
        }
        else
        {
            cursorBuffer = VK_NULL_HANDLE;
            cursorAllocation = VK_NULL_HANDLE;
        }

        if (cursorMapped == nullptr)
        {
            // 连一块 host visible 内存都拿不到：彻底关掉，不要假装在工作。
            mode = Mode::Disabled;
        }
        else if (mode == Mode::Disabled)
        {
            mode = Mode::UpdateBuffer;
        }
    }

    for (Slot& slot : slots)
        slot.entries.reserve(512);

    active = (mode != Mode::Disabled);

    std::cout << "[Breadcrumbs] " << (active ? "已启用" : "不可用")
              << "，模式=" << getModeName() << std::endl;
}

Breadcrumbs::~Breadcrumbs()
{
    // Device 析构只是 breadcrumbs.reset() 掉这个对象，隐式析构不会替我们调
    // shutdown()。漏掉这一步的后果不是「泄漏几个字节」那么温和：游标缓冲会活到
    // vmaDestroyAllocator 之后，触发 VMA 的 "Some allocations were not freed"
    // 断言；Debug 下还会弹断言框把进程卡住，自检脚本看起来就像随机超时。
    shutdown();
}

void Breadcrumbs::shutdown()
{
    if (device != nullptr && cursorBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(device->getVmaAllocator(), cursorBuffer, cursorAllocation);
        cursorBuffer = VK_NULL_HANDLE;
        cursorAllocation = VK_NULL_HANDLE;
        cursorMapped = nullptr;
    }

    active = false;
    mode = Mode::Disabled;
    device = nullptr;

    pfnCmdSetCheckpointNV = nullptr;
    pfnGetQueueCheckpointData2NV = nullptr;
    pfnCmdWriteBufferMarkerAMD = nullptr;

    for (Slot& slot : slots)
    {
        slot.recorded = false;
        slot.counter = 0;
        slot.entries.clear();
    }
}

const char* Breadcrumbs::getModeName() const
{
    switch (mode)
    {
    case Mode::NvCheckpoint: return "nv-checkpoint（粒度到单个绘制）";
    case Mode::AmdMarker:    return "amd-marker（4 字节写）";
    case Mode::UpdateBuffer: return "update-buffer（降级，仅 pass 粒度）";
    case Mode::Disabled:
    default:                 return "disabled";
    }
}

void Breadcrumbs::emitCursorWriteBarrier(VkCommandBuffer cmd, uint32_t slot)
{
    // 为什么每条检查点前都要一道屏障：同步验证把 marker 写入算作一次
    // TRANSFER_WRITE，而游标缓冲是跨检查点、跨帧复用的同一块内存 —— 少了屏障，
    // 每条 marker 都会报 SYNC-HAZARD-WRITE-AFTER-WRITE（--validation-features sync
    // 下 60 帧能刷出两千多条）。调试设施自己变成验证层的报错来源，是本末倒置。
    // 代价很小：barrier 只覆盖那 4 个字节，access 只声明 TRANSFER_WRITE。
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = cursorBuffer;
    barrier.offset = slot * sizeof(uint32_t);
    barrier.size = sizeof(uint32_t);

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Breadcrumbs::emitMarker(VkCommandBuffer cmd, uint32_t slot, uint32_t value, bool insideRenderPass)
{
    if (!isActive() || slot >= kSlotCount || cmd == VK_NULL_HANDLE)
        return;

    switch (mode)
    {
    case Mode::NvCheckpoint:
        // 注意签名是 const void*：驱动只当它是个不透明 64 位值。
        pfnCmdSetCheckpointNV(cmd, reinterpret_cast<const void*>(static_cast<uintptr_t>(encodeMarker(slot, value))));
        break;

    case Mode::AmdMarker:
        // TOP_OF_PIPE：语义是"执行到这里"，而不是"到这里的事都做完了"。
        // 我们要的正是"最后到达的那条"，用 BOTTOM 反而会把游标压到末尾。
        emitCursorWriteBarrier(cmd, slot);
        pfnCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, cursorBuffer,
                                   slot * static_cast<uint32_t>(sizeof(uint32_t)), value);
        break;

    case Mode::UpdateBuffer:
        // 渲染通道内禁止传输命令，只能放弃这一条（粒度退化为 pass）。
        if (insideRenderPass)
            return;
        emitCursorWriteBarrier(cmd, slot);
        vkCmdUpdateBuffer(cmd, cursorBuffer, slot * static_cast<uint32_t>(sizeof(uint32_t)),
                          static_cast<VkDeviceSize>(sizeof(uint32_t)), &value);
        break;

    case Mode::Disabled:
    default:
        break;
    }
}

uint32_t Breadcrumbs::pushEntry(uint32_t slot, Kind kind, const char* label)
{
    Slot& s = slots[slot];
    ++s.counter;

    // 永远不要撞上哨兵值，否则"跑完了"和"第 4294967295 个检查点"无法区分。
    if (s.counter == kCompleted)
        s.counter = 1;

    Entry entry{};
    entry.counter = s.counter;
    entry.kind = kind;
    std::snprintf(entry.label, kMaxLabelLength, "%s", label != nullptr ? label : "");
    s.entries.push_back(entry);

    return s.counter;
}

void Breadcrumbs::beginSlot(VkCommandBuffer cmd, uint32_t slot, uint64_t frameNumber)
{
    if (slot >= kSlotCount)
        return;

    Slot& s = slots[slot];
    s.recorded = true;
    s.frameNumber = frameNumber;
    s.counter = 0;
    s.entries.clear();

    // 把 GPU 那格清零，避免上一帧同槽位的残值被误读成本帧的检查点。
    emitMarker(cmd, slot, 0, false);
}

void Breadcrumbs::finishSlot(VkCommandBuffer cmd, uint32_t slot)
{
    if (slot >= kSlotCount)
        return;

    // 哨兵：读到这里说明该槽位正常跑完了，崩溃不在它身上。
    emitMarker(cmd, slot, kCompleted, false);
}

void Breadcrumbs::beginPass(VkCommandBuffer cmd, uint32_t slot, const char* passName)
{
    if (!isActive() || slot >= kSlotCount)
        return;

    emitMarker(cmd, slot, pushEntry(slot, Kind::Pass, passName), false);
}

void Breadcrumbs::endPass(VkCommandBuffer cmd, uint32_t slot, const char* passName)
{
    if (!isActive() || slot >= kSlotCount)
        return;

    char label[kMaxLabelLength];
    std::snprintf(label, kMaxLabelLength, "end %s", passName != nullptr ? passName : "");

    // 调用点保证已经在 vkCmdEndRendering 之后。
    emitMarker(cmd, slot, pushEntry(slot, Kind::Pass, label), false);
}

void Breadcrumbs::checkpoint(VkCommandBuffer cmd, uint32_t slot, Kind kind, const char* label)
{
    if (!isActive() || slot >= kSlotCount)
        return;

    // 绘制/dispatch 级检查点基本都发生在渲染通道内部，按"通道内"处理。
    const bool insideRenderPass = (kind != Kind::Pass);
    emitMarker(cmd, slot, pushEntry(slot, kind, label), insideRenderPass);
}

bool Breadcrumbs::selfTest()
{
    bool ok = true;

    // 槽位映射：frameSlot ∈ {0,1} × {graphics, compute} 必须落在 4 格内且不撞车。
    ok = ok && slotFor(0, false) == 0u;
    ok = ok && slotFor(0, true) == 1u;
    ok = ok && slotFor(1, false) == 2u;
    ok = ok && slotFor(1, true) == 3u;
    ok = ok && slotFor(1, true) < kSlotCount;

    // 标记编解码往返：槽位和 counter 不能互相污染。
    const uint32_t counters[] = { 0u, 1u, 1234u, kCompleted };
    for (uint32_t slot = 0; slot < kSlotCount; ++slot)
    {
        for (uint32_t counter : counters)
        {
            const uint64_t marker = encodeMarker(slot, counter);
            ok = ok && static_cast<uint32_t>(marker >> 32) == slot;
            ok = ok && static_cast<uint32_t>(marker & 0xFFFFFFFFull) == counter;
        }
    }

    // 长名字必须被截断而不是越界。
    char label[kMaxLabelLength];
    std::snprintf(label, kMaxLabelLength, "%s",
                  "一个非常非常长的 pass 名字用来测试截断是否安全 ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    ok = ok && std::strlen(label) < kMaxLabelLength;

    if (ok)
        std::cout << "[Breadcrumbs] selfTest PASS | 槽位映射 + 标记编解码 + 名字截断" << std::endl;
    else
        std::cout << "[Breadcrumbs] selfTest FAIL" << std::endl;

    return ok;
}
