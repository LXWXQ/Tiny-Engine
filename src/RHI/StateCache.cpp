#include "StateCache.h"

#include "FrameStats.h"

#include <cstdio>
#include <cstring>

namespace tiny::statecache
{
namespace
{
// 命令缓冲的数量是固定的：一帧里真正被复用的只有引擎的两条（图形/计算），
// 剩下的是地形构建、纹理上传这类一次性缓冲。8 个槽足够，超出按轮转覆盖 ——
// 覆盖只会让下一次绑定多做一次，不会出错。
constexpr size_t kSlots = 8;

struct Slot
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkPipeline graphics = VK_NULL_HANDLE;
    VkPipeline compute = VK_NULL_HANDLE;
    // 只有显式登记过的缓冲才敢跳过重复绑定：没登记的缓冲可能刚被 reset 过。
    bool tracked = false;
};

Slot g_slots[kSlots];
size_t g_nextSlot = 0;

// 只有这两条命令缓冲受管（每帧 reset + begin，缓存与之一一对应）。
VkCommandBuffer g_graphicsCommandBuffer = VK_NULL_HANDLE;
VkCommandBuffer g_computeCommandBuffer = VK_NULL_HANDLE;

bool g_enabled = true;

Stats g_frame;
Stats g_total;
uint64_t g_frames = 0;

Slot& slotFor(VkCommandBuffer commandBuffer)
{
    for (Slot& slot : g_slots)
    {
        if (slot.commandBuffer == commandBuffer)
            return slot;
    }
    for (Slot& slot : g_slots)
    {
        if (slot.commandBuffer == VK_NULL_HANDLE)
        {
            slot.commandBuffer = commandBuffer;
            return slot;
        }
    }
    Slot& recycled = g_slots[g_nextSlot % kSlots];
    ++g_nextSlot;
    recycled = Slot{};
    recycled.commandBuffer = commandBuffer;
    return recycled;
}

bool isTracked(VkCommandBuffer commandBuffer)
{
    for (const Slot& slot : g_slots)
    {
        if (slot.commandBuffer == commandBuffer)
            return slot.tracked;
    }
    return false;
}

void trackInSlots(VkCommandBuffer commandBuffer)
{
    Slot& slot = slotFor(commandBuffer);
    slot = Slot{};
    slot.commandBuffer = commandBuffer;
    slot.tracked = true;
}

// 每一次"有人要求绑定"都记一笔：受管缓冲走缓存，未受管的直接发。
// 计数放在这里而不是 needsBind 里，是因为未受管缓冲根本不经过判定 ——
// 之前放在 needsBind 里，出现了 issued(17.5) > bindCalls(17.0) 的自相矛盾。
void recordBindCall()
{
    ++g_frame.bindCalls;
    ++g_total.bindCalls;
}

bool needsBind(VkCommandBuffer commandBuffer, VkPipeline pipeline, bool compute)
{
    Slot& slot = slotFor(commandBuffer);
    VkPipeline& bound = compute ? slot.compute : slot.graphics;
    if (bound == pipeline)
    {
        ++g_frame.skipped;
        ++g_total.skipped;
        return false;
    }
    return true;
}

void noteBound(VkCommandBuffer commandBuffer, VkPipeline pipeline, bool compute)
{
    Slot& slot = slotFor(commandBuffer);
    VkPipeline& bound = compute ? slot.compute : slot.graphics;
    bound = pipeline;
    ++g_frame.issued;
    ++g_total.issued;
}
}

void beginFrame(VkCommandBuffer graphicsCommandBuffer, VkCommandBuffer computeCommandBuffer)
{
    // 第一帧之前（启动期的 IBL / 纹理上传）发生的绑定不属于任何一帧：
    // 它们会先落进 g_total，这里一次性丢掉，两张表的窗口才对齐。
    if (g_frames == 0)
        g_total = Stats{};

    for (Slot& slot : g_slots)
        slot = Slot{};
    g_nextSlot = 0;
    g_frame = Stats{};
    g_graphicsCommandBuffer = graphicsCommandBuffer;
    g_computeCommandBuffer = computeCommandBuffer;
    trackInSlots(graphicsCommandBuffer);
    trackInSlots(computeCommandBuffer);
    ++g_frames;
}

void setEnabled(bool enabled)
{
    g_enabled = enabled;
}

bool isEnabled()
{
    return g_enabled;
}

void trackCommandBuffer(VkCommandBuffer commandBuffer)
{
    if (commandBuffer == VK_NULL_HANDLE)
        return;
    trackInSlots(commandBuffer);
}

void invalidateAll()
{
    for (Slot& slot : g_slots)
    {
        slot.graphics = VK_NULL_HANDLE;
        slot.compute = VK_NULL_HANDLE;
    }
}

void bindGraphics(VkCommandBuffer commandBuffer, VkPipeline pipeline)
{
    tiny::framestats::countPipelineBind(pipeline, false);
    recordBindCall();
    if (!g_enabled || !isTracked(commandBuffer))
    {
        // 不认识的命令缓冲一律照发：它可能刚被 reset 过，缓存里的记录不可信。
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        ++g_frame.issued;
        ++g_total.issued;
        ++g_frame.unmanaged;
        ++g_total.unmanaged;
        return;
    }
    if (!needsBind(commandBuffer, pipeline, false))
        return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    noteBound(commandBuffer, pipeline, false);
}

void bindCompute(VkCommandBuffer commandBuffer, VkPipeline pipeline)
{
    tiny::framestats::countPipelineBind(pipeline, true);
    recordBindCall();
    if (!g_enabled || !isTracked(commandBuffer))
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        ++g_frame.issued;
        ++g_total.issued;
        ++g_frame.unmanaged;
        ++g_total.unmanaged;
        return;
    }
    if (!needsBind(commandBuffer, pipeline, true))
        return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    noteBound(commandBuffer, pipeline, true);
}

const Stats& frameStats()
{
    return g_frame;
}

uint64_t totalIssued()
{
    return g_total.issued;
}

uint64_t totalSkipped()
{
    return g_total.skipped;
}

void report()
{
    if (g_frames == 0)
        return;
    const auto perFrame = [](uint64_t value) {
        return static_cast<double>(value) / static_cast<double>(g_frames);
    };
    const double savedPct = g_total.bindCalls == 0
        ? 0.0
        : 100.0 * static_cast<double>(g_total.skipped) / static_cast<double>(g_total.bindCalls);
    std::printf("[StateCache] frames=%llu | bindCalls/frame=%.1f issued=%.1f unmanaged=%.1f skipped=%.1f | saved=%.1f%%\n",
                static_cast<unsigned long long>(g_frames), perFrame(g_total.bindCalls),
                perFrame(g_total.issued), perFrame(g_total.unmanaged), perFrame(g_total.skipped), savedPct);
}

bool selfTest()
{
    // 只验账本，不碰 Vulkan：句柄用序号伪装，绝不进入 vkCmdBindPipeline。
    const Stats savedFrame = g_frame;
    const Stats savedTotal = g_total;
    Slot savedSlots[kSlots];
    for (size_t i = 0; i < kSlots; ++i)
        savedSlots[i] = g_slots[i];
    const size_t savedNext = g_nextSlot;
    const uint64_t savedFrames = g_frames;
    const VkCommandBuffer savedGraphicsCb = g_graphicsCommandBuffer;
    const VkCommandBuffer savedComputeCb = g_computeCommandBuffer;

    const VkCommandBuffer cb = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x1000));
    const VkCommandBuffer computeCb = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x2000));
    const VkPipeline a = reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(0x11));
    const VkPipeline b = reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(0x22));

    beginFrame(cb, computeCb);

    bool ok = true;
    // beginFrame 自动登记的两条主缓冲是可缓存的；没登记的缓冲必须每次都发。
    ok = ok && isTracked(cb) && isTracked(computeCb);
    const VkCommandBuffer stranger = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x3000));
    ok = ok && !isTracked(stranger);
    trackCommandBuffer(stranger);
    ok = ok && isTracked(stranger);

    ok = ok && needsBind(cb, a, false);    // 第一次必须发
    noteBound(cb, a, false);
    ok = ok && !needsBind(cb, a, false);   // 同一个再绑一次，跳过
    ok = ok && needsBind(cb, b, false);    // 换管线必须发
    noteBound(cb, b, false);
    ok = ok && needsBind(computeCb, b, true); // 计算流是另一条缓冲，不共享状态
    // bindCalls 由 recordBindCall() 记（真实路径在 bindGraphics/bindCompute 入口），
    // needsBind 只负责判定，所以这里单独补三笔来验不变量 issued + skipped == bindCalls。
    recordBindCall();
    recordBindCall();
    recordBindCall();
    ok = ok && frameStats().bindCalls == 3 && frameStats().issued == 2 && frameStats().skipped == 1;
    ok = ok && needsBind(cb, a, true);      // 图形流绑过 a，但计算的绑定点仍是空的
    invalidateAll();
    ok = ok && needsBind(cb, b, false);     // 失效之后必须重新发

    g_frame = savedFrame;
    g_total = savedTotal;
    for (size_t i = 0; i < kSlots; ++i)
        g_slots[i] = savedSlots[i];
    g_nextSlot = savedNext;
    g_frames = savedFrames;
    g_graphicsCommandBuffer = savedGraphicsCb;
    g_computeCommandBuffer = savedComputeCb;
    return ok;
}
}
