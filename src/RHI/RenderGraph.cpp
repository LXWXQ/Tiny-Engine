#include "GpuProfiler.h"
#include "RHI/RenderGraph.h"
#include "RHI/CommandUtils.h"

#include <stdexcept>
#include <iostream>
#include <algorithm>

namespace
{
    // 「写」访问位。用于判断是否需要屏障：写->任何 都是 hazard。
    constexpr VkAccessFlags2 kWriteBits =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_HOST_WRITE_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT;

    bool isDepthFormat(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
        }
    }

    VkImageAspectFlags aspectOf(bool isDepth)
    {
        return isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// ============================================================================
// RenderGraph
// ============================================================================

RenderGraph::RenderGraph(Device& device) : device(device) {}

RenderGraph::~RenderGraph()
{
    for (TextureEntry& e : textures)
    {
        if (!e.owned) continue;   // 导入的资源由调用方负责销毁
        if (e.view != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), e.view, nullptr);
        if (e.image != VK_NULL_HANDLE) vmaDestroyImage(device.getVmaAllocator(), e.image, e.allocation);
    }

    for (BufferEntry& b : buffers)
    {
        if (!b.owned) continue;
        if (b.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), b.buffer, b.allocation);
    }
}

RenderGraph::SubresourceRange RenderGraph::normalizeTextureRange(
    const TextureEntry& entry, const SubresourceRange& range) const
{
    if (entry.desc.mipLevels == 0 || entry.desc.arrayLayers == 0)
        throw std::runtime_error("RenderGraph: texture has zero mip/layer count: " + entry.desc.name);
    if (range.baseMipLevel >= entry.desc.mipLevels || range.baseArrayLayer >= entry.desc.arrayLayers)
        throw std::runtime_error("RenderGraph: image subresource range starts out of bounds: " + entry.desc.name);

    SubresourceRange out = range;
    if (out.levelCount == 0)
        out.levelCount = entry.desc.mipLevels - out.baseMipLevel;
    if (out.layerCount == 0)
        out.layerCount = entry.desc.arrayLayers - out.baseArrayLayer;
    if (out.levelCount == 0 || out.layerCount == 0 ||
        out.baseMipLevel + out.levelCount > entry.desc.mipLevels ||
        out.baseArrayLayer + out.layerCount > entry.desc.arrayLayers)
    {
        throw std::runtime_error("RenderGraph: image subresource range exceeds texture bounds: " + entry.desc.name);
    }
    return out;
}

RenderGraph::BufferRange RenderGraph::normalizeBufferRange(
    const BufferEntry& entry, const BufferRange& range) const
{
    if (range.offset > entry.desc.size)
        throw std::runtime_error("RenderGraph: buffer range offset exceeds size: " + entry.desc.name);

    const VkDeviceSize size = (range.size == VK_WHOLE_SIZE)
        ? entry.desc.size - range.offset
        : range.size;
    if (size == 0 || range.offset + size > entry.desc.size)
        throw std::runtime_error("RenderGraph: buffer range exceeds buffer bounds: " + entry.desc.name);

    return BufferRange{ range.offset, size };
}

void RenderGraph::beginFrame(VkCommandBuffer graphicsCommandBuffer,
                             VkCommandBuffer computeCommandBuffer,
                             uint32_t frameSlot,
                             uint64_t frameNumber)
{
    cmd = graphicsCommandBuffer;
    computeCmd = computeCommandBuffer;
    crossQueueDependencies = {};
    currentFrameSlot = frameSlot;

    // P0-4：GPU 计时。关闭时 gpuTimer 为 nullptr，一个分支都不进。
#ifdef SENTINEL_ENABLE_GPU_TIMING
    if (gpuTimer && cmd != VK_NULL_HANDLE)
        gpuTimer->onFrameBegin(cmd, frameSlot, frameNumber);
#else
    (void)frameNumber;
#endif

    for (std::unique_ptr<Pass>& p : passes)
        p.reset();
    passes.clear();

    // 每帧复位「瞬时资源」的布局记录。
    // importTexture 时如果给了非 UNDEFINED 的初始布局，说明它是跨帧常驻的只读资源
    // （例如 IBL cubemap），这种不能复位 —— 否则会被当成「内容未定义」而丢弃。
    for (TextureEntry& e : textures)
    {
        if (!e.resetEachFrame) continue;
        for (TextureEntry::SubresourceState& state : e.subresources)
        {
            state.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            state.stage = VK_PIPELINE_STAGE_2_NONE;
            state.access = VK_ACCESS_2_NONE;
            state.lastAccessQueue = -1;
        }
    }

    for (BufferEntry& b : buffers)
    {
        if (b.desc.persistent) continue;
        b.stage = VK_PIPELINE_STAGE_2_NONE;
        b.access = VK_ACCESS_2_NONE;
        b.lastAccessQueue = -1;
    }
}

void RenderGraph::endFrame()
{
    if (cmd == VK_NULL_HANDLE)
        throw std::runtime_error("RenderGraph: 调用 endFrame 之前必须先 beginFrame");

    const bool hasComputePass = std::any_of(passes.begin(), passes.end(),
        [](const std::unique_ptr<Pass>& p) { return p->isCompute && p->enabled; });
    if (hasComputePass && computeCmd == VK_NULL_HANDLE)
        throw std::runtime_error("RenderGraph: 包含了 compute pass，但 beginFrame 没有提供 compute command buffer");

    // ---- 语义校验：先查"谁能读到没人写的东西"，再开始录制 ----
    // 放在最前面，是为了让错误在"这一帧的 GPU 命令还没提交"时就抛出来，
    // 而不是让画面先花一帧。
    validateWriteCoverage();

    // P0-4：只对 graphics pass 计时；compute 队列的时间戳另算。
    uint32_t timedPassIndex = 0;

    for (std::unique_ptr<Pass>& passPtr : passes)
    {
        Pass& pass = *passPtr;
        if (!pass.enabled) continue;

        const VkCommandBuffer passCmd = pass.isCompute ? computeCmd : cmd;

        // P0-4：本 pass 的 GPU 计时开始（仅 graphics）。
        if (!pass.isCompute)
            onPassTimingBegin(passCmd, timedPassIndex, pass.name.c_str());

        // ---- 0) 同一个 Pass 里对同一张图可能声明了多次访问 ----
        // 典型是 Bloom 的上采样：它既要把粗级结果当纹理采样，
        // 又要把本级当颜色附件累加写入。按声明顺序插入的屏障会变成
        // "转到 SHADER_READ、再转回 COLOR_ATTACHMENT"，白白多发一次。
        // 只保留最后一次声明即可 —— 屏障本来就只需要保证
        // "进入这个 Pass 时资源处于正确的布局"。
        std::vector<Access> accesses;
        accesses.reserve(pass.accesses.size());
        for (const Access& acc : pass.accesses)
        {
            if (!acc.texture.valid()) continue;
            auto it = std::find_if(accesses.begin(), accesses.end(),
                [&](const Access& a)
                {
                    return a.texture.id == acc.texture.id &&
                        a.range.baseMipLevel == acc.range.baseMipLevel &&
                        a.range.levelCount == acc.range.levelCount &&
                        a.range.baseArrayLayer == acc.range.baseArrayLayer &&
                        a.range.layerCount == acc.range.layerCount;
                });
            if (it != accesses.end())
                *it = acc;
            else
                accesses.push_back(acc);
        }

        // ---- 0.5) 声明合法性校验 ----
        // 渲染图的核心承诺是「声明即同步」，代价是漏声明不会报错，只会出
        // 屏障缺失或 usage 不匹配的怪画面。这里把最常见的两类错误在
        // 录制前就挑出来：句柄越界、以及拿没有对应 usage 的图当附件用。
        for (const Access& acc : accesses)
        {
            if (acc.texture.id >= textures.size())
            {
                throw std::runtime_error("RenderGraph: Pass \"" + pass.name +
                    "\" 引用了越界的纹理句柄");
            }

            const TextureEntry& te = textures[acc.texture.id];
            const uint32_t id = acc.texture.id;

            const bool usedAsColor = std::any_of(pass.colors.begin(), pass.colors.end(),
                [&](const ColorAttachment& c) { return c.texture.id == id; });
            const bool usedAsDepth = std::any_of(pass.depths.begin(), pass.depths.end(),
                [&](const DepthAttachment& d) { return d.texture.id == id; });

            if (usedAsColor && !(te.desc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
                throw std::runtime_error("RenderGraph: 纹理 \"" + te.desc.name +
                    "\" 被当作颜色附件，但创建时没有声明 COLOR_ATTACHMENT usage");
            if (usedAsDepth && !(te.desc.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
                throw std::runtime_error("RenderGraph: 纹理 \"" + te.desc.name +
                    "\" 被当作深度附件，但创建时没有声明 DEPTH_STENCIL_ATTACHMENT usage");
            if ((acc.access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT) &&
                !(te.desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
                throw std::runtime_error("RenderGraph: 纹理 \"" + te.desc.name +
                    "\" 被采样，但创建时没有声明 SAMPLED usage");
            if ((acc.access & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) &&
                !(te.desc.usage & VK_IMAGE_USAGE_STORAGE_BIT))
                throw std::runtime_error("RenderGraph: 纹理 \"" + te.desc.name +
                    "\" 被当作存储图像，但创建时没有声明 STORAGE usage");
        }

        std::vector<BufferAccess> bufferAccesses;
        bufferAccesses.reserve(pass.bufferAccesses.size());
        for (const BufferAccess& acc : pass.bufferAccesses)
        {
            if (!acc.buffer.valid()) continue;
            auto it = std::find_if(bufferAccesses.begin(), bufferAccesses.end(),
                [&](const BufferAccess& a)
                {
                    return a.buffer.id == acc.buffer.id &&
                        a.range.offset == acc.range.offset &&
                        a.range.size == acc.range.size;
                });
            if (it != bufferAccesses.end())
                *it = acc;
            else
                bufferAccesses.push_back(acc);
        }

        for (const BufferAccess& acc : bufferAccesses)
        {
            if (acc.buffer.id >= buffers.size())
            {
                throw std::runtime_error("RenderGraph: Pass \"" + pass.name +
                    "\" 引用了越界的 buffer 句柄");
            }

            const BufferEntry& be = buffers[acc.buffer.id];
            if ((acc.access & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) &&
                !(be.desc.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
            {
                throw std::runtime_error("RenderGraph: buffer \"" + be.desc.name +
                    "\" 被当作 storage buffer，但创建时没有声明 STORAGE_BUFFER usage");
            }
        }

        if (pass.isCompute && (!pass.colors.empty() || !pass.depths.empty()))
            throw std::runtime_error("RenderGraph: compute pass \"" + pass.name + "\" 不能声明颜色/深度附件");

        // P1-1：Tracy GPU zone 覆盖本 pass 的屏障 + 渲染 + 回调。
        // active=false（compute pass 或未启用）时是空操作。
        {
            SENTINEL_GPU_ZONE_SCOPE(gpuProfiler, passCmd, pass.name.c_str(), !pass.isCompute);

        // ---- 1) 资源屏障：由声明自动推导 ----
        for (const Access& acc : accesses)
            emitBarrierIfNeeded(acc, passCmd, pass.isCompute);
        for (const BufferAccess& acc : bufferAccesses)
            emitBufferBarrierIfNeeded(acc, passCmd, pass.isCompute);

        // ---- 2) 动态渲染 ----
        const bool raster = !pass.isCompute && (!pass.colors.empty() || !pass.depths.empty());

        if (raster)
        {
            std::vector<VkRenderingAttachmentInfo> colorInfos(pass.colors.size());
            for (size_t i = 0; i < pass.colors.size(); ++i)
            {
                const ColorAttachment& ca = pass.colors[i];
                const TextureEntry& te = textures[ca.texture.id];

                colorInfos[i] = VkRenderingAttachmentInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                colorInfos[i].imageView = te.view;
                colorInfos[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorInfos[i].loadOp = ca.loadOp;
                colorInfos[i].storeOp = ca.storeOp;
                colorInfos[i].clearValue = ca.clearValue;
            }

            VkRenderingAttachmentInfo depthInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            const bool hasDepth = !pass.depths.empty();
            if (hasDepth)
            {
                const DepthAttachment& da = pass.depths[0];
                const TextureEntry& te = textures[da.texture.id];
                depthInfo.imageView = te.view;
                depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthInfo.loadOp = da.loadOp;
                depthInfo.storeOp = da.storeOp;
                depthInfo.clearValue = da.clearValue;
            }

            // 尺寸：优先用显式指定的，否则取第一个附件的
            VkExtent2D extent = pass.extent;
            if (!pass.hasExtent)
            {
                const TextureHandle ref = pass.colors.empty() ? pass.depths[0].texture : pass.colors[0].texture;
                extent = textures[ref.id].desc.extent;
            }

            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea.offset = { 0, 0 };
            info.renderArea.extent = extent;
            info.layerCount = 1;
            info.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size());
            info.pColorAttachments = colorInfos.empty() ? nullptr : colorInfos.data();
            info.pDepthAttachment = hasDepth ? &depthInfo : nullptr;

            vkCmdBeginRendering(passCmd, &info);

            // viewport / scissor 在所有管线里都设成动态状态，必须显式设置
            cmdSetFullViewportAndScissor(passCmd, extent);
        }

        // ---- 3) 用户回调（真正的绘制 / dispatch）----
        if (pass.callback)
            pass.callback(passCmd);

        // ---- 4) 结束动态渲染 ----
        if (raster)
            vkCmdEndRendering(passCmd);

        // P0-4：本 pass 的 GPU 计时结束。
        if (!pass.isCompute)
        {
            onPassTimingEnd(passCmd, timedPassIndex);
            ++timedPassIndex;
        }
        }   // P1-1：结束本 pass 的 GPU zone 作用域

        // ---- 5) 记录本 Pass 造成的最终状态，供下一个 Pass 推导屏障 ----
        for (const Access& acc : accesses)
        {
            TextureEntry& te = textures[acc.texture.id];
            const SubresourceRange range = normalizeTextureRange(te, acc.range);
            for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer)
            {
                for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; ++mip)
                {
                    TextureEntry::SubresourceState& state =
                        te.subresources[mip * te.desc.arrayLayers + layer];
                    state.layout = acc.layout;
                    state.stage = acc.stage;
                    state.access = acc.access;
                    state.lastAccessQueue = pass.isCompute ? 1 : 0;
                }
            }
        }
        for (const BufferAccess& acc : bufferAccesses)
        {
            BufferEntry& be = buffers[acc.buffer.id];
            be.stage = acc.stage;
            be.access = acc.access;
            be.lastAccessQueue = pass.isCompute ? 1 : 0;
        }
    }

    for (std::unique_ptr<Pass>& p : passes)
        p.reset();
    passes.clear();
    cmd = VK_NULL_HANDLE;
    computeCmd = VK_NULL_HANDLE;
}

void RenderGraph::validateWriteCoverage()
{
    // TD-05: coverage is tracked per mip/layer (and per buffer interval), not
    // per whole resource. A pass that writes mip0 no longer satisfies a later
    // read of mip1.
    std::vector<std::vector<bool>> texWritten(textures.size());
    std::vector<std::vector<bool>> texPendingWrite(textures.size());
    std::vector<std::string> texLastWriter(textures.size());
    for (size_t i = 0; i < textures.size(); ++i)
    {
        const size_t count = static_cast<size_t>(textures[i].desc.mipLevels) * textures[i].desc.arrayLayers;
        texWritten[i].assign(count, false);
        texPendingWrite[i].assign(count, false);
    }

    std::vector<std::vector<BufferRange>> bufWritten(buffers.size());
    std::vector<std::vector<BufferRange>> bufPendingWrite(buffers.size());
    std::vector<std::string> bufLastWriter(buffers.size());

    std::vector<bool> texGraphicsTouched(textures.size(), false);
    std::vector<bool> texComputeTouched(textures.size(), false);
    std::vector<bool> texGraphicsWritten(textures.size(), false);
    std::vector<bool> texComputeWritten(textures.size(), false);
    std::vector<bool> bufGraphicsTouched(buffers.size(), false);
    std::vector<bool> bufComputeTouched(buffers.size(), false);
    std::vector<bool> bufGraphicsWritten(buffers.size(), false);
    std::vector<bool> bufComputeWritten(buffers.size(), false);
    crossQueueDependencies = {};
    std::vector<std::string> computeToGraphicsResources;
    std::vector<std::string> graphicsToComputeResources;
    auto addC2G = [&](const std::string& name)
    {
        crossQueueDependencies.computeToGraphics = true;
        if (std::find(computeToGraphicsResources.begin(), computeToGraphicsResources.end(), name) == computeToGraphicsResources.end())
            computeToGraphicsResources.push_back(name);
    };
    auto addG2C = [&](const std::string& name)
    {
        crossQueueDependencies.graphicsToCompute = true;
        if (std::find(graphicsToComputeResources.begin(), graphicsToComputeResources.end(), name) == graphicsToComputeResources.end())
            graphicsToComputeResources.push_back(name);
    };

    auto mergeIntervals = [](std::vector<BufferRange>& intervals)
    {
        std::sort(intervals.begin(), intervals.end(),
            [](const BufferRange& a, const BufferRange& b) { return a.offset < b.offset; });
        std::vector<BufferRange> merged;
        for (const BufferRange& r : intervals)
        {
            if (!merged.empty() && r.offset <= merged.back().offset + merged.back().size)
            {
                const VkDeviceSize end = std::max(merged.back().offset + merged.back().size, r.offset + r.size);
                merged.back().size = end - merged.back().offset;
            }
            else
            {
                merged.push_back(r);
            }
        }
        intervals = std::move(merged);
    };
    auto intervalsCover = [](const std::vector<BufferRange>& intervals,
                             VkDeviceSize offset, VkDeviceSize size)
    {
        const VkDeviceSize end = offset + size;
        for (const BufferRange& r : intervals)
            if (r.offset <= offset && r.offset + r.size >= end)
                return true;
        return false;
    };
    for (const std::unique_ptr<Pass>& passPtr : passes)
    {
        const Pass& pass = *passPtr;

        for (std::vector<bool>& pending : texPendingWrite)
            std::fill(pending.begin(), pending.end(), false);
        for (std::vector<BufferRange>& pending : bufPendingWrite)
            pending.clear();

        for (const Access& acc : pass.accesses)
        {
            if (!acc.texture.valid() || acc.texture.id >= textures.size() || !(acc.access & kWriteBits))
                continue;
            const TextureEntry& te = textures[acc.texture.id];
            const SubresourceRange range = normalizeTextureRange(te, acc.range);
            std::vector<bool>& pending = texPendingWrite[acc.texture.id];
            for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer)
                for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; ++mip)
                    pending[mip * te.desc.arrayLayers + layer] = true;
        }

        for (const BufferAccess& ba : pass.bufferAccesses)
        {
            if (!ba.buffer.valid() || ba.buffer.id >= buffers.size() || !(ba.access & kWriteBits))
                continue;
            bufPendingWrite[ba.buffer.id].push_back(normalizeBufferRange(buffers[ba.buffer.id], ba.range));
        }

        for (size_t i = 0; i < textures.size(); ++i)
        {
            const bool anyPending = std::any_of(texPendingWrite[i].begin(), texPendingWrite[i].end(),
                [](bool v) { return v; });
            if (!anyPending)
                continue;
            texLastWriter[i] = pass.enabled ? pass.name : (pass.name + "（本帧被 setEnabled(false) 跳过）");
            if (pass.enabled)
            {
                for (size_t s = 0; s < texWritten[i].size(); ++s)
                    if (texPendingWrite[i][s]) texWritten[i][s] = true;
            }
        }

        for (size_t i = 0; i < buffers.size(); ++i)
        {
            if (bufPendingWrite[i].empty())
                continue;
            bufLastWriter[i] = pass.enabled ? pass.name : (pass.name + "（本帧被 setEnabled(false) 跳过）");
            if (pass.enabled)
            {
                for (const BufferRange& r : bufPendingWrite[i])
                    bufWritten[i].push_back(r);
                mergeIntervals(bufWritten[i]);
            }
        }

        if (!pass.enabled)
            continue;
        for (const Access& acc : pass.accesses)
        {
            if (!acc.texture.valid() || acc.texture.id >= textures.size())
                continue;

            const uint32_t id = acc.texture.id;
            const TextureEntry& te = textures[id];
            const bool isWrite = (acc.access & kWriteBits) != 0;

            if (pass.isCompute)
            {
                if ((isWrite && texGraphicsTouched[id]) || (!isWrite && texGraphicsWritten[id]))
                    addG2C(te.desc.name);
                texComputeTouched[id] = true;
                texComputeWritten[id] = texComputeWritten[id] || isWrite;
            }
            else
            {
                if ((isWrite && texComputeTouched[id]) || (!isWrite && texComputeWritten[id]))
                    addC2G(te.desc.name);
                texGraphicsTouched[id] = true;
                texGraphicsWritten[id] = texGraphicsWritten[id] || isWrite;
            }

            if (isWrite || te.desc.persistent)
                continue;

            const SubresourceRange range = normalizeTextureRange(te, acc.range);
            for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer)
            {
                for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; ++mip)
                {
                    const size_t subresource = mip * te.desc.arrayLayers + layer;
                    if (texPendingWrite[id][subresource] || texWritten[id][subresource])
                        continue;

                    std::string detail;
                    if (!texLastWriter[id].empty())
                        detail = "\n  它本来是由 Pass \"" + texLastWriter[id] + "\" 写的。";
                    else
                        detail = "\n  没有任何 Pass 声明过要写这个子资源。";

                    throw std::runtime_error(
                        "RenderGraph 语义错误: Pass \"" + pass.name + "\" 读取了瞬时纹理 \""
                        + te.desc.name + "\" 的 mip " + std::to_string(mip) + " / layer "
                        + std::to_string(layer) + "，但该子资源这一帧到目前为止没有任何 Pass 写过它。"
                        + detail + "\n"
                        "  瞬时资源每帧内容都会重置，读到的将是未定义内容。\n"
                        "  请确认写入和读取声明的 SubresourceRange 指向同一个 mip/layer。");
                }
            }
        }
        for (const BufferAccess& acc : pass.bufferAccesses)
        {
            if (!acc.buffer.valid() || acc.buffer.id >= buffers.size())
                continue;

            const uint32_t id = acc.buffer.id;
            const BufferEntry& be = buffers[id];
            const bool isWrite = (acc.access & kWriteBits) != 0;

            if (pass.isCompute)
            {
                if ((isWrite && bufGraphicsTouched[id]) || (!isWrite && bufGraphicsWritten[id]))
                    addG2C(be.desc.name);
                bufComputeTouched[id] = true;
                bufComputeWritten[id] = bufComputeWritten[id] || isWrite;
            }
            else
            {
                if ((isWrite && bufComputeTouched[id]) || (!isWrite && bufComputeWritten[id]))
                    addC2G(be.desc.name);
                bufGraphicsTouched[id] = true;
                bufGraphicsWritten[id] = bufGraphicsWritten[id] || isWrite;
            }

            if (isWrite || be.desc.persistent)
                continue;

            const BufferRange range = normalizeBufferRange(be, acc.range);
            std::vector<BufferRange> covered = bufWritten[id];
            for (const BufferRange& r : bufPendingWrite[id])
                covered.push_back(r);
            mergeIntervals(covered);
            if (!intervalsCover(covered, range.offset, range.size))
            {
                std::string detail;
                if (!bufLastWriter[id].empty())
                    detail = "\n  它本来是由 Pass \"" + bufLastWriter[id] + "\" 写的。";
                else
                    detail = "\n  没有任何 Pass 声明过要写这段 buffer。";

                throw std::runtime_error(
                    "RenderGraph 语义错误: Pass \"" + pass.name + "\" 读取了瞬时 buffer \""
                    + be.desc.name + "\" 的区间 [" + std::to_string(range.offset) + ", "
                    + std::to_string(range.offset + range.size) + ")，但该区间这一帧到目前为止无人写入。"
                    + detail);
            }
        }
    }
    if (device.hasDedicatedComputeQueue())
    {
        for (size_t i = 0; i < textures.size(); ++i)
        {
            if (texGraphicsTouched[i] && texComputeTouched[i] && !textures[i].desc.crossQueue)
                throw std::runtime_error(
                    "RenderGraph 跨队列错误: 纹理 \"" + textures[i].desc.name +
                    "\" 同时被 graphics / compute 访问，但创建时没有声明 crossQueue=true。"
                    " 当前实现使用 CONCURRENT sharing 的简单路径；请在 TextureDesc 里设置 crossQueue=true。");
        }
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            if (bufGraphicsTouched[i] && bufComputeTouched[i] && !buffers[i].desc.crossQueue)
                throw std::runtime_error(
                    "RenderGraph 跨队列错误: buffer \"" + buffers[i].desc.name +
                    "\" 同时被 graphics / compute 访问，但创建时没有声明 crossQueue=true。"
                    " 当前实现使用 CONCURRENT sharing 的简单路径；请在 BufferDesc 里设置 crossQueue=true。");
        }
    }

    if (crossQueueDependencies.computeToGraphics && crossQueueDependencies.graphicsToCompute)
    {
        auto join = [](const std::vector<std::string>& names)
        {
            std::string s;
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (i) s += ", ";
                s += names[i];
            }
            return s;
        };
        throw std::runtime_error(
            "RenderGraph 跨队列错误: 同一帧同时存在 compute->graphics 与 graphics->compute 依赖，"
            "当前每个队列只有一条命令缓冲，无法表达双向顺序。\n"
            "  compute->graphics: " + join(computeToGraphicsResources) + "\n"
            "  graphics->compute: " + join(graphicsToComputeResources));
    }
}

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
                continue;

            // A source-less, destination-less barrier carries no dependency.
            if (state.layout == acc.layout &&
                state.stage == VK_PIPELINE_STAGE_2_NONE && state.access == VK_ACCESS_2_NONE &&
                acc.stage == VK_PIPELINE_STAGE_2_NONE && acc.access == VK_ACCESS_2_NONE)
                continue;

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
        return;

    const bool isRead = (acc.access & kWriteBits) == 0;
    const bool sameState = (e.stage == acc.stage && e.access == acc.access);
    if (sameState && isRead)
        return;

    if (!isRead && e.stage == VK_PIPELINE_STAGE_2_NONE && e.access == VK_ACCESS_2_NONE)
        return;

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
    vkCmdPipelineBarrier2(target, &dep);
}

RenderGraph::Pass& RenderGraph::addGraphicsPass(const std::string& name)
{
    passes.push_back(std::make_unique<Pass>(name));
    passes.back()->isCompute = false;
    return *passes.back();
}

RenderGraph::Pass& RenderGraph::addComputePass(const std::string& name)
{
    passes.push_back(std::make_unique<Pass>(name));
    passes.back()->isCompute = true;
    return *passes.back();
}

RenderGraph::TextureHandle RenderGraph::createTexture(const TextureDesc& desc)
{
    TextureEntry e;
    e.desc = desc;
    e.owned = true;
    e.isDepth = isDepthFormat(desc.format);
    // 常驻资源（在 desc 里显式标记）每帧不复位布局记录，否则会被当成
    // "内容未定义"而丢弃 —— 比如 IBL 的烘焙结果。
    e.resetEachFrame = !desc.persistent;
    e.subresources.resize(static_cast<size_t>(desc.mipLevels) * desc.arrayLayers);
    createOwnedTexture(e);

    textures.push_back(std::move(e));
    return TextureHandle{ static_cast<uint32_t>(textures.size() - 1) };
}

RenderGraph::TextureHandle RenderGraph::importTexture(const TextureDesc& desc, VkImage image, VkImageView view,
                                                      VkImageLayout initialLayout)
{
    TextureEntry e;
    e.desc = desc;
    e.image = image;
    e.view = view;
    e.owned = false;
    e.isDepth = isDepthFormat(desc.format);
    e.subresources.resize(static_cast<size_t>(desc.mipLevels) * desc.arrayLayers);
    for (TextureEntry::SubresourceState& state : e.subresources)
        state.layout = initialLayout;
    // 给了确定布局 = 跨帧常驻资源（例如 IBL cubemap），不要每帧复位。
    //
    // 注意这里把结论回写进了 desc.persistent，让"是否常驻"只有一个事实来源：
    // 否则 resetEachFrame（来自参数）和 desc.persistent（来自调用方）会各说各话，
    // 语义校验就会把 Environment/IBL 这类导入的常驻资源误判成"没人写的瞬时资源"
    //（这个误报在实现语义校验时真的发生过一次）。
    e.desc.persistent = (initialLayout != VK_IMAGE_LAYOUT_UNDEFINED);
    e.resetEachFrame = !e.desc.persistent;

    textures.push_back(std::move(e));
    return TextureHandle{ static_cast<uint32_t>(textures.size() - 1) };
}

RenderGraph::BufferHandle RenderGraph::createBuffer(const BufferDesc& desc)
{
    if (desc.size == 0)
        throw std::runtime_error("RenderGraph: buffer \"" + desc.name + "\" 的大小不能为 0");

    BufferEntry e;
    e.desc = desc;
    e.owned = true;
    createOwnedBuffer(e);

    buffers.push_back(std::move(e));
    return BufferHandle{ static_cast<uint32_t>(buffers.size() - 1) };
}

RenderGraph::BufferHandle RenderGraph::importBuffer(const BufferDesc& desc, VkBuffer buffer)
{
    if (buffer == VK_NULL_HANDLE)
        throw std::runtime_error("RenderGraph: importBuffer 收到了 VK_NULL_HANDLE");

    BufferEntry e;
    e.desc = desc;
    e.buffer = buffer;
    e.owned = false;
    buffers.push_back(std::move(e));
    return BufferHandle{ static_cast<uint32_t>(buffers.size() - 1) };
}

void RenderGraph::createOwnedBuffer(BufferEntry& e)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = e.desc.size;
    info.usage = e.desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // TD-02 简单路径：跨队列资源使用 CONCURRENT sharing，避免 ownership transfer。
    const QueueFamilyIndices queueFamilies = device.findPhysicalQueueFamilies();
    uint32_t sharedFamilies[2] = {
        queueFamilies.graphicsFamily.value(),
        queueFamilies.computeFamily.value()
    };
    if (e.desc.crossQueue && device.hasDedicatedComputeQueue())
    {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = sharedFamilies;
    }

    VmaAllocationCreateInfo alloc{};
    alloc.usage = e.desc.hostVisible ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (e.desc.hostVisible)
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo allocInfo{};
    if (vmaCreateBuffer(device.getVmaAllocator(), &info, &alloc,
        &e.buffer, &e.allocation, &allocInfo) != VK_SUCCESS)
        throw std::runtime_error("RenderGraph: buffer 创建失败: " + e.desc.name);

    if (e.desc.hostVisible)
        e.mapped = allocInfo.pMappedData;
}

void RenderGraph::createOwnedTexture(TextureEntry& e)
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = e.desc.format;
    info.extent = { e.desc.extent.width, e.desc.extent.height, 1 };
    info.mipLevels = e.desc.mipLevels;
    info.arrayLayers = e.desc.arrayLayers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = e.desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const QueueFamilyIndices queueFamilies = device.findPhysicalQueueFamilies();
    uint32_t sharedFamilies[2] = {
        queueFamilies.graphicsFamily.value(),
        queueFamilies.computeFamily.value()
    };
    if (e.desc.crossQueue && device.hasDedicatedComputeQueue())
    {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = sharedFamilies;
    }
    info.flags = e.desc.flags;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(device.getVmaAllocator(), &info, &alloc, &e.image, &e.allocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("RenderGraph: 图像创建失败: " + e.desc.name);

    VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = e.image;
    if (e.desc.arrayLayers == 6 && (e.desc.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
        view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    else if (e.desc.arrayLayers > 1)
        view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = e.desc.format;
    view.subresourceRange = { aspectOf(e.isDepth), 0, e.desc.mipLevels, 0, e.desc.arrayLayers };

    if (vkCreateImageView(device.getDevice(), &view, nullptr, &e.view) != VK_SUCCESS)
        throw std::runtime_error("RenderGraph: 图像视图创建失败: " + e.desc.name);
}

void RenderGraph::resizeOwnedTexture(TextureHandle handle, VkExtent2D extent)
{
    if (!handle.valid() || handle.id >= textures.size())
        throw std::runtime_error("RenderGraph: resizeOwnedTexture invalid handle");

    TextureEntry& e = textures[handle.id];
    if (!e.owned)
        throw std::runtime_error("RenderGraph: resizeOwnedTexture called on imported texture \"" + e.desc.name + "\"");
    if (e.desc.extent.width == extent.width && e.desc.extent.height == extent.height)
        return;
    if (extent.width == 0 || extent.height == 0)
        throw std::runtime_error("RenderGraph: resizeOwnedTexture got zero extent");

    if (e.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device.getDevice(), e.view, nullptr);
        e.view = VK_NULL_HANDLE;
    }
    if (e.image != VK_NULL_HANDLE)
    {
        vmaDestroyImage(device.getVmaAllocator(), e.image, e.allocation);
        e.image = VK_NULL_HANDLE;
        e.allocation = VK_NULL_HANDLE;
    }

    e.desc.extent = extent;
    for (TextureEntry::SubresourceState& state : e.subresources)
    {
        state.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        state.stage = VK_PIPELINE_STAGE_2_NONE;
        state.access = VK_ACCESS_2_NONE;
        state.lastAccessQueue = -1;
    }
    createOwnedTexture(e);
}

void RenderGraph::updateImportedTexture(TextureHandle handle, const TextureDesc& desc,
                                        VkImage image, VkImageView view,
                                        VkImageLayout initialLayout)
{
    if (!handle.valid() || handle.id >= textures.size())
        throw std::runtime_error("RenderGraph: updateImportedTexture invalid handle");
    if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
        throw std::runtime_error("RenderGraph: updateImportedTexture received null image/view");

    TextureEntry& e = textures[handle.id];
    if (e.owned)
        throw std::runtime_error("RenderGraph: updateImportedTexture called on owned texture \"" + e.desc.name + "\"");

    e.desc = desc;
    e.image = image;
    e.view = view;
    e.isDepth = isDepthFormat(desc.format);
    e.desc.persistent = (initialLayout != VK_IMAGE_LAYOUT_UNDEFINED);
    e.resetEachFrame = !e.desc.persistent;
    e.subresources.assign(static_cast<size_t>(desc.mipLevels) * desc.arrayLayers, {});
    for (TextureEntry::SubresourceState& state : e.subresources)
        state.layout = initialLayout;
}

VkImage RenderGraph::getImage(TextureHandle h) const
{
    if (!h.valid() || h.id >= textures.size()) return VK_NULL_HANDLE;
    return textures[h.id].image;
}

VkImageView RenderGraph::getView(TextureHandle h) const
{
    if (!h.valid() || h.id >= textures.size()) return VK_NULL_HANDLE;
    return textures[h.id].view;
}

VkFormat RenderGraph::getFormat(TextureHandle h) const
{
    if (!h.valid() || h.id >= textures.size()) return VK_FORMAT_UNDEFINED;
    return textures[h.id].desc.format;
}

VkExtent2D RenderGraph::getExtent(TextureHandle h) const
{
    if (!h.valid() || h.id >= textures.size()) return VkExtent2D{ 0, 0 };
    return textures[h.id].desc.extent;
}

const RenderGraph::TextureDesc& RenderGraph::getDesc(TextureHandle h) const
{
    if (!h.valid() || h.id >= textures.size())
        throw std::runtime_error("RenderGraph: 非法的纹理句柄");
    return textures[h.id].desc;
}

VkBuffer RenderGraph::getBuffer(BufferHandle h) const
{
    if (!h.valid() || h.id >= buffers.size()) return VK_NULL_HANDLE;
    return buffers[h.id].buffer;
}

const RenderGraph::BufferDesc& RenderGraph::getBufferDesc(BufferHandle h) const
{
    if (!h.valid() || h.id >= buffers.size())
        throw std::runtime_error("RenderGraph: 非法的 buffer 句柄");
    return buffers[h.id].desc;
}

void* RenderGraph::getBufferMapped(BufferHandle h) const
{
    if (!h.valid() || h.id >= buffers.size()) return nullptr;
    return buffers[h.id].mapped;
}

void RenderGraph::invalidateBuffer(BufferHandle h) const
{
    if (!h.valid() || h.id >= buffers.size()) return;
    const BufferEntry& e = buffers[h.id];
    if (e.owned && e.allocation != VK_NULL_HANDLE && e.mapped != nullptr)
        vmaInvalidateAllocation(device.getVmaAllocator(), e.allocation, 0, VK_WHOLE_SIZE);
}

void RenderGraph::presentTransition(TextureHandle handle)
{
    if (!handle.valid()) return;
    Pass& p = addGraphicsPass("PresentTransition");
    // 没有颜色/深度附件 -> 渲染图只发屏障，不会 vkCmdBeginRendering
    p.transitionTo(handle, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void RenderGraph::setPassEnabled(const std::string& passName, bool enabled)
{
    for (std::unique_ptr<Pass>& p : passes)
        if (p->name == passName) p->enabled = enabled;
}

// ============================================================================
// Pass 的资源访问声明
// ============================================================================

RenderGraph::Pass& RenderGraph::Pass::transitionTo(TextureHandle t, VkImageLayout layout,
                                                  VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                                                  const SubresourceRange& range)
{
    if (t.valid())
        accesses.push_back(Access{ t, layout, stage, access, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::read(TextureHandle t, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                                           const SubresourceRange& range)
{
    if (t.valid())
        accesses.push_back(Access{ t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, access, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::readStorage(TextureHandle t, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                                                 const SubresourceRange& range)
{
    if (t.valid())
        accesses.push_back(Access{ t, VK_IMAGE_LAYOUT_GENERAL, stage, access, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::writeStorage(TextureHandle t, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                                                  const SubresourceRange& range)
{
    if (t.valid())
        accesses.push_back(Access{ t, VK_IMAGE_LAYOUT_GENERAL, stage, access, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::readBuffer(BufferHandle b, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                                                const BufferRange& range)
{
    if (b.valid())
        bufferAccesses.push_back(BufferAccess{ b, stage, access, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::writeBuffer(BufferHandle b, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                                                 const BufferRange& range)
{
    if (b.valid())
        bufferAccesses.push_back(BufferAccess{ b, stage, access, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::writeColor(TextureHandle t, VkAttachmentLoadOp loadOp, VkClearValue clear,
                                                 VkAttachmentStoreOp storeOp, const SubresourceRange& range)
{
    if (!t.valid()) return *this;

    ColorAttachment ca;
    ca.texture = t;
    ca.loadOp = loadOp;
    ca.storeOp = storeOp;
    ca.clearValue = clear;
    colors.push_back(ca);

    // LOAD_OP_LOAD 会读取附件原有内容：除了写，还必须声明读，
    // 否则屏障的 dstAccessMask 只覆盖写，同步校验会报 SYNC-HAZARD-READ_AFTER_WRITE。
    VkAccessFlags2 colorAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
        colorAccess |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;

    accesses.push_back(Access{ t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               colorAccess, range });
    return *this;
}

RenderGraph::Pass& RenderGraph::Pass::writeDepth(TextureHandle t, VkAttachmentLoadOp loadOp, VkClearValue clear,
                                                 VkAttachmentStoreOp storeOp, const SubresourceRange& range)
{
    if (!t.valid()) return *this;

    if (clear.depthStencil.depth == 0.0f && clear.depthStencil.stencil == 0)
        clear.depthStencil = { 1.0f, 0 };   // 深度默认清 1.0

    DepthAttachment da;
    da.texture = t;
    da.loadOp = loadOp;
    da.storeOp = storeOp;
    da.clearValue = clear;
    depths.push_back(da);

    // 同上：LOAD 语义下深度附件也会被读取。
    VkAccessFlags2 depthAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
        depthAccess |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    accesses.push_back(Access{ t, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                               depthAccess, range });
    return *this;
}