// 屏障发射（T-03：按 (srcStage,dstStage,srcAccess,dstAccess,oldLayout,newLayout) 分组合并）。
//
// 从 RenderGraph.cpp 拆出来的理由：这里要加批处理，而声明的合法性校验、资源登记、
// 提交循环都还在 RenderGraph.cpp 里 —— 混在一起时改一处屏障要在一千五百行里找上下文。
//
// 批处理为什么要跨资源：单个 Pass 里的每张图通常只有 1 个 mip、1 个 layer，
// 逐子资源合并的收益是零（实测 21 个 Pass 里只有 IBL 的 6 级 mip 链能合）。
// 真正重复的是「同一个 Pass 里多张图做同一类转换」—— G-Buffer 五张图一起
// COLOR_ATTACHMENT -> SHADER_READ，Bloom 的上采样每级两次转换。
// 所以这里改成：emitBarrierIfNeeded 只把屏障追加进待发队列，由 flushImageBarriers
// 在 Pass 的命令之前一次性按桶发出去。
//
// 两个数字的口径差别（--stats-frame 的两列）：
//   entries = 追加了多少条 barrier —— 正确性的度量，批处理前后必须完全一致；
//   calls   = 真正调了几次 vkCmdPipelineBarrier2 —— 批处理收益的度量。

#include "RHI/RenderGraph.h"
#include "RenderGraphInternal.h"
#include "FrameStats.h"

#include <vector>

using namespace rgdetail;

namespace
{
    // 同一个桶 = 这六个字段全相同。srcStageMask 里已经含了跨队列的
    // ALL_COMMANDS 替换，所以跨队列与同队列的屏障自然落进不同桶。
    inline bool sameBarrierKey(const VkImageMemoryBarrier2& a, const VkImageMemoryBarrier2& b)
    {
        return a.srcStageMask == b.srcStageMask &&
            a.dstStageMask == b.dstStageMask &&
            a.srcAccessMask == b.srcAccessMask &&
            a.dstAccessMask == b.dstAccessMask &&
            a.oldLayout == b.oldLayout &&
            a.newLayout == b.newLayout;
    }
}

void RenderGraph::emitBarrierIfNeeded(const Access& acc, VkCommandBuffer target, bool currentQueueIsCompute)
{
    (void)target;   // 屏障不再就地发射，由 flushImageBarriers 统一发。

    if (!acc.texture.valid() || acc.texture.id >= textures.size())
        return;

    TextureEntry& e = textures[acc.texture.id];
    const bool isRead = (acc.access & kWriteBits) == 0;
    const SubresourceRange range = normalizeTextureRange(e, acc.range);

    for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer)
    {
        for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; ++mip)
        {
            TextureEntry::SubresourceState& state =
                e.subresources[mip * e.desc.arrayLayers + layer];
            const bool crossQueue = state.lastAccessQueue >= 0 &&
                (state.lastAccessQueue == 1) != currentQueueIsCompute;
            const bool sameState = (state.layout == acc.layout &&
                state.stage == acc.stage && state.access == acc.access);

            if (sameState && isRead)
            {
                tiny::framestats::countBarrierSkippedSameState();
                continue;
            }

            // A source-less, destination-less barrier carries no dependency.
            if (state.layout == acc.layout &&
                state.stage == VK_PIPELINE_STAGE_2_NONE && state.access == VK_ACCESS_2_NONE &&
                acc.stage == VK_PIPELINE_STAGE_2_NONE && acc.access == VK_ACCESS_2_NONE)
            {
                tiny::framestats::countBarrierSkippedSameState();
                continue;
            }

            VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask = crossQueue ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : state.stage;
            b.srcAccessMask = crossQueue ? VK_ACCESS_2_NONE : state.access;
            b.dstStageMask = acc.stage;
            b.dstAccessMask = acc.access;
            b.oldLayout = state.layout;
            b.newLayout = acc.layout;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = e.image;
            b.subresourceRange = { aspectOf(e.isDepth), mip, 1, layer, 1 };
            pendingImageBarriers.push_back(b);
        }
    }
}

void RenderGraph::flushImageBarriers(VkCommandBuffer target)
{
    if (pendingImageBarriers.empty())
        return;

    // 每次取队尾那条当桶的样板，把同键的邻居挑出来，凑够一批就发一次。
    // 桶内成员的顺序无所谓（同一批屏障互不重叠），所以用「与队尾交换」的
    // 删除方式，避免在热路径上做 N 次 erase。
    std::vector<VkImageMemoryBarrier2> batch;
    batch.reserve(pendingImageBarriers.size());

    while (!pendingImageBarriers.empty())
    {
        const VkImageMemoryBarrier2 proto = pendingImageBarriers.back();
        batch.clear();

        size_t i = 0;
        while (i < pendingImageBarriers.size())
        {
            if (sameBarrierKey(pendingImageBarriers[i], proto))
            {
                batch.push_back(pendingImageBarriers[i]);
                pendingImageBarriers[i] = pendingImageBarriers.back();
                pendingImageBarriers.pop_back();
            }
            else
            {
                ++i;
            }
        }

        if (batch.empty())
            continue;

        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(batch.size());
        dep.pImageMemoryBarriers = batch.data();
        tiny::framestats::countBarrierCall(static_cast<uint32_t>(batch.size()));
        vkCmdPipelineBarrier2(target, &dep);
    }
}

void RenderGraph::emitBufferBarrierIfNeeded(const BufferAccess& acc, VkCommandBuffer target, bool currentQueueIsCompute)
{
    if (!acc.buffer.valid() || acc.buffer.id >= buffers.size())
        return;

    BufferEntry& e = buffers[acc.buffer.id];
    const bool crossQueue = e.lastAccessQueue >= 0 &&
        (e.lastAccessQueue == 1) != currentQueueIsCompute;
    // The timeline semaphore wait is the cross-queue memory dependency.
    if (crossQueue)
    {
        tiny::framestats::countBarrierSkippedCrossQueue();
        return;
    }

    const bool isRead = (acc.access & kWriteBits) == 0;
    const bool sameState = (e.stage == acc.stage && e.access == acc.access);
    if (sameState && isRead)
    {
        tiny::framestats::countBarrierSkippedSameState();
        return;
    }

    if (!isRead && e.stage == VK_PIPELINE_STAGE_2_NONE && e.access == VK_ACCESS_2_NONE)
    {
        tiny::framestats::countBarrierSkippedSameState();
        return;
    }

    const BufferRange range = normalizeBufferRange(e, acc.range);
    VkBufferMemoryBarrier2 b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    b.srcStageMask = e.stage;
    b.srcAccessMask = e.access;
    b.dstStageMask = acc.stage;
    b.dstAccessMask = acc.access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = e.buffer;
    b.offset = range.offset;
    b.size = range.size;

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    tiny::framestats::countBarrierCall(1);
    vkCmdPipelineBarrier2(target, &dep);
}