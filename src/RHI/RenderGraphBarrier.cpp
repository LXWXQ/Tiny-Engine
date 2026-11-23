// 屏障发射（T-03 的落点：按 (srcStage,dstStage,layout) 分组合并）。
//
// 从 RenderGraph.cpp 拆出来的理由：这里马上要加批处理，而声明的合法性校验、资源登记、
// 提交循环都还在 RenderGraph.cpp 里 —— 混在一起时改一处屏障要在一千五百行里找上下文。
// 拆的时候只搬函数，不改一行行为：--stats-frame 的三个数字必须逐位不变。

#include "RHI/RenderGraph.h"
#include "RenderGraphInternal.h"
#include "FrameStats.h"

using namespace rgdetail;

void RenderGraph::emitBarrierIfNeeded(const Access& acc, VkCommandBuffer target, bool currentQueueIsCompute)
{
    if (!acc.texture.valid() || acc.texture.id >= textures.size())
        return;

    TextureEntry& e = textures[acc.texture.id];
    const bool isRead = (acc.access & kWriteBits) == 0;
    const SubresourceRange range = normalizeTextureRange(e, acc.range);

    // Emit one barrier per touched subresource. For 1-mip/1-layer resources this
    // is exactly the old behavior; for mip chains it guarantees mip1 is not
    // transitioned when a pass only declared mip0.
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

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            tiny::framestats::countBarrierCall(1);
            vkCmdPipelineBarrier2(target, &dep);
        }
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
