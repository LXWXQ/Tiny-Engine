#include "GpuProfiler.h"
#include "RHI/ResourceStats.h"
#include "RHI/RenderTargetPool.h"
#include "RHI/RenderGraph.h"
#include "RenderGraphInternal.h"
#include "RHI/CommandUtils.h"
#include "RHI/Breadcrumbs.h"
#include "RHI/ScratchArena.h"

// T-10：每帧屏障条数 / 管线绑定计数。
#include "FrameStats.h"

// TM-08：资源别名化的只读报告（只出数字，不改分配）。
#include "RHI/RenderGraphAlias.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdio>

// TM-10：准备阶段的并行跑在作业系统上。
#include "Core/JobSystem.h"

using namespace rgdetail;
namespace
{
    // T-04：TextureDesc -> 池的键。mip/layer 为 0 在这里归一成 1，与 VkImageCreateInfo 一致。
    tiny::RenderTargetPool::Desc poolDescFromTexture(const RenderGraph::TextureDesc& desc, bool isDepth)
    {
        tiny::RenderTargetPool::Desc out{};
        out.format = desc.format;
        out.extent = desc.extent;
        out.usage = desc.usage;
        out.flags = desc.flags;
        out.mipLevels = desc.mipLevels == 0 ? 1 : desc.mipLevels;
        out.arrayLayers = desc.arrayLayers == 0 ? 1 : desc.arrayLayers;
        out.crossQueue = desc.crossQueue;
        out.isDepth = isDepth;
        return out;
    }
}

// ============================================================================
// RenderGraph
// ============================================================================

RenderGraph::RenderGraph(Device& device) : device(device)
{
    // T-04：跨帧 RT 池。它持有 Device 的引用，生命周期必须短于 Device ——
    // Application 在销毁 Device 之前先 renderGraph.reset()，这里靠的是同一条约定。
    texturePool = std::make_unique<tiny::RenderTargetPool>(device);
}

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
    pendingImageBarriers.clear();
    tiny::framestats::beginFrame();
    tiny::resourcestats::beginFrame();
    if (texturePool) texturePool->beginFrame(frameNumber);

    // TM-07：帧首把暂存区轮转到这一帧的槽位（offset 归零、上一轮淘汰的旧块回收）。
    device.getScratch().beginFrame(frameSlot);

    // ②：每帧把两条命令流的 GPU 游标清零，并把帧号记进 CPU 侧表。
    // device lost 之后就是靠「CPU 表 + GPU 里的那一个 32 位游标」还原现场。
    if (Breadcrumbs* crumbs = device.getBreadcrumbs(); crumbs != nullptr && crumbs->isActive())
    {
        if (cmd != VK_NULL_HANDLE)
            crumbs->beginSlot(cmd, Breadcrumbs::slotFor(frameSlot, false), frameNumber);
        if (computeCmd != VK_NULL_HANDLE)
            crumbs->beginSlot(computeCmd, Breadcrumbs::slotFor(frameSlot, true), frameNumber);
    }

    // P0-4：GPU 计时。关闭时 gpuTimer 为 nullptr，一个分支都不进。
#ifdef TINY_ENABLE_GPU_TIMING
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

    // TM-08：生命周期边界每帧重算（常驻资源也复位 —— 报告看的是本帧的用法）。
    for (TextureEntry& e : textures)
    {
        e.firstWritePass = -1;
        e.lastUsePass = -1;
        // T-01：外部声明是"每帧一次"的，上一帧的标记不能漏到这一帧。
        e.externalDeclared = false;
        e.externalUsed = false;
    }

    for (BufferEntry& b : buffers)
    {
        if (b.desc.persistent) continue;
        b.stage = VK_PIPELINE_STAGE_2_NONE;
        b.access = VK_ACCESS_2_NONE;
        b.lastAccessQueue = -1;
    }
    for (BufferEntry& b : buffers)
    {
        b.externalDeclared = false;
        b.externalUsed = false;
    }
}

// TM-10：一个 Pass 的「准备」阶段 —— 全是纯数据工作。
// 这里刻意不碰 VkCommandBuffer、也不写 textures / buffers 的可变字段（那些是
// 屏障推导用的顺序状态），才能安全并行；同理这里不能抛异常，错误先存进
// prepared.error，回到串行阶段按 Pass 顺序抛。
void RenderGraph::preparePass(PreparedPass& prepared)
{
    Pass& pass = *prepared.pass;

    try
    {
        // 0) 同一个 Pass 里对同一张图可能声明了多次访问。
        // 典型是 Bloom 的上采样：既要把粗级结果当纹理采样，又要把本级当颜色
        // 附件累加写入。按声明顺序插屏障会变成「转到 SHADER_READ、再转回
        // COLOR_ATTACHMENT」，白白多发一次。只保留最后一次声明即可 ——
        // 屏障本来就只需要保证「进入这个 Pass 时资源处于正确的布局」。
        prepared.accesses.reserve(pass.accesses.size());
        for (const Access& acc : pass.accesses)
        {
            if (!acc.texture.valid()) continue;
            auto it = std::find_if(prepared.accesses.begin(), prepared.accesses.end(),
                [&](const Access& a)
                {
                    return a.texture.id == acc.texture.id &&
                        a.range.baseMipLevel == acc.range.baseMipLevel &&
                        a.range.levelCount == acc.range.levelCount &&
                        a.range.baseArrayLayer == acc.range.baseArrayLayer &&
                        a.range.layerCount == acc.range.layerCount;
                });
            if (it != prepared.accesses.end())
                *it = acc;
            else
                prepared.accesses.push_back(acc);
        }

        // 0.5) 声明合法性校验：句柄越界、拿没有对应 usage 的图当附件用。
        // 渲染图的承诺是「声明即同步」，代价是漏声明不报错、只出怪画面，
        // 所以把最常见的两类错误在录制前挑出来。
        for (const Access& acc : prepared.accesses)
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

        prepared.bufferAccesses.reserve(pass.bufferAccesses.size());
        for (const BufferAccess& acc : pass.bufferAccesses)
        {
            if (!acc.buffer.valid()) continue;
            auto it = std::find_if(prepared.bufferAccesses.begin(), prepared.bufferAccesses.end(),
                [&](const BufferAccess& a)
                {
                    return a.buffer.id == acc.buffer.id &&
                        a.range.offset == acc.range.offset &&
                        a.range.size == acc.range.size;
                });
            if (it != prepared.bufferAccesses.end())
                *it = acc;
            else
                prepared.bufferAccesses.push_back(acc);
        }

        for (const BufferAccess& acc : prepared.bufferAccesses)
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

        // ---- 组装动态渲染的附件描述 ----
        prepared.raster = !pass.isCompute && (!pass.colors.empty() || !pass.depths.empty());
        if (!prepared.raster)
            return;

        prepared.colorInfos.resize(pass.colors.size());
        for (size_t i = 0; i < pass.colors.size(); ++i)
        {
            const ColorAttachment& ca = pass.colors[i];
            const TextureEntry& te = textures[ca.texture.id];

            prepared.colorInfos[i] = VkRenderingAttachmentInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            prepared.colorInfos[i].imageView = te.view;
            prepared.colorInfos[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            prepared.colorInfos[i].loadOp = ca.loadOp;
            prepared.colorInfos[i].storeOp = ca.storeOp;
            prepared.colorInfos[i].clearValue = ca.clearValue;
        }

        prepared.hasDepth = !pass.depths.empty();
        if (prepared.hasDepth)
        {
            const DepthAttachment& da = pass.depths[0];
            const TextureEntry& te = textures[da.texture.id];
            prepared.depthInfo.imageView = te.view;
            prepared.depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            prepared.depthInfo.loadOp = da.loadOp;
            prepared.depthInfo.storeOp = da.storeOp;
            prepared.depthInfo.clearValue = da.clearValue;
        }

        // 尺寸：优先用显式指定的，否则取第一个附件的
        prepared.extent = pass.extent;
        if (!pass.hasExtent)
        {
            const TextureHandle ref = pass.colors.empty() ? pass.depths[0].texture : pass.colors[0].texture;
            prepared.extent = textures[ref.id].desc.extent;
        }

        prepared.info = VkRenderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        prepared.info.renderArea.offset = { 0, 0 };
        prepared.info.renderArea.extent = prepared.extent;
        prepared.info.layerCount = 1;
        prepared.info.colorAttachmentCount = static_cast<uint32_t>(prepared.colorInfos.size());
        prepared.info.pColorAttachments = prepared.colorInfos.empty() ? nullptr : prepared.colorInfos.data();
        prepared.info.pDepthAttachment = prepared.hasDepth ? &prepared.depthInfo : nullptr;
    }
    catch (const std::exception& e)
    {
        prepared.error = e.what();
    }
}

void RenderGraph::reportPrepareStats() const
{
    if (statsFrames == 0)
        return;

    const double frames = static_cast<double>(statsFrames);
    std::printf("[RenderGraph] 准备 %.2f us/帧 | 录制 %.2f us/帧 | 并行=%s | 本帧 %zu 个 Pass\n",
        static_cast<double>(prepareMicrosTotal) / frames,
        static_cast<double>(recordMicrosTotal) / frames,
        parallelPrepare ? "开" : "关",
        lastFrameExecutedPasses.size());
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

    // 本帧 Pass 执行 / 跳过清单：setPassEnabled 的语义靠它可断言，
    // 调试面板也能直接显示这一帧到底跑了哪些 Pass。
    lastFrameExecutedPasses.clear();
    lastFrameSkippedPasses.clear();

    const auto frameClock = std::chrono::steady_clock::now();

    // 串行：挑出这一帧真正要跑的 Pass，并分配计时序号。
    // 顺序、清单、计时序号必须与「逐个跑」时完全一致 —— setPassEnabled 的语义、
    // [RenderGraph] pass bookkeeping selfTest、TM-08 的生命周期序号都靠这个顺序。
    std::vector<Pass*> activePasses;
    activePasses.reserve(passes.size());
    for (std::unique_ptr<Pass>& passPtr : passes)
    {
        Pass& pass = *passPtr;
        if (!pass.enabled)
        {
            lastFrameSkippedPasses.push_back(pass.name);
            tiny::framestats::countPass(false);
            continue;
        }
        lastFrameExecutedPasses.push_back(pass.name);
        tiny::framestats::countPass(true);
        activePasses.push_back(&pass);
    }

    while (preparedPasses.size() < activePasses.size())
        preparedPasses.push_back(std::make_unique<PreparedPass>());

    uint32_t timedPassIndex = 0;
    for (size_t i = 0; i < activePasses.size(); ++i)
    {
        PreparedPass& prepared = *preparedPasses[i];
        prepared.pass = activePasses[i];
        prepared.cmd = prepared.pass->isCompute ? computeCmd : cmd;
        prepared.passIndex = static_cast<int32_t>(i);
        prepared.timedPassIndex = 0;
        prepared.raster = false;
        prepared.hasDepth = false;
        prepared.error.clear();
        prepared.accesses.clear();
        prepared.bufferAccesses.clear();
        prepared.colorInfos.clear();
        // P0-4：只对 graphics pass 计时；compute 队列的时间戳另算。
        if (!prepared.pass->isCompute)
            prepared.timedPassIndex = timedPassIndex++;
    }

    // 并行（或串行）准备。每个 Pass 只碰自己的声明与 textures / buffers 的
    // 只读字段，互不引用，所以能直接并行；pass 数很少时并行反而更慢，所以
    // 少于 2 个就直接串行。
    if (parallelPrepare && jobSystem != nullptr && activePasses.size() > 1)
    {
        jobSystem->parallelFor(0, static_cast<uint32_t>(activePasses.size()), 1,
            [this](uint32_t begin, uint32_t end)
            {
                for (uint32_t i = begin; i < end; ++i)
                    preparePass(*preparedPasses[i]);
            });
    }
    else
    {
        for (size_t i = 0; i < activePasses.size(); ++i)
            preparePass(*preparedPasses[i]);
    }

    // 错误按 Pass 顺序统一抛。并行跑的时候不能在 worker 里抛 —— 报错顺序会随
    // 线程调度变化，排查时看到的第一个错就不一定是第一个错了。
    for (size_t i = 0; i < activePasses.size(); ++i)
    {
        const std::string& error = preparedPasses[i]->error;
        if (!error.empty())
            throw std::runtime_error(error);
    }

    const auto prepareEnd = std::chrono::steady_clock::now();

    // ---- 串行录制 ----
    for (size_t i = 0; i < activePasses.size(); ++i)
    {
        PreparedPass& prepared = *preparedPasses[i];
        Pass& pass = *prepared.pass;
        const VkCommandBuffer passCmd = prepared.cmd;

        // ②：本 pass 的标记。崩溃后 breadcrumbs 会把「最后到达的那个名字」
        // 打出来 —— 这比对着一个故障地址二分注释代码快得多。
        Breadcrumbs* crumbs = nullptr;
        if (Breadcrumbs* tracked = device.getBreadcrumbs(); tracked != nullptr && tracked->isActive())
        {
            crumbs = tracked;
            crumbs->beginPass(passCmd, Breadcrumbs::slotFor(currentFrameSlot, pass.isCompute), pass.name.c_str());
        }

        // P0-4：本 pass 的 GPU 计时开始（仅 graphics）。
        if (!pass.isCompute)
            onPassTimingBegin(passCmd, prepared.timedPassIndex, pass.name.c_str());

        // P1-1：Tracy GPU zone 覆盖本 pass 的屏障 + 渲染 + 回调。
        {
            TINY_GPU_ZONE_SCOPE(gpuProfiler, passCmd, pass.name.c_str(), !pass.isCompute);

            // ---- 1) 资源屏障：由声明自动推导 ----
            // 这一段必须串行：屏障取决于上一个 Pass 留下的布局。
            for (const Access& acc : prepared.accesses)
                emitBarrierIfNeeded(acc, passCmd, pass.isCompute);
            flushImageBarriers(passCmd);
            for (const BufferAccess& acc : prepared.bufferAccesses)
                emitBufferBarrierIfNeeded(acc, passCmd, pass.isCompute);

            // ---- 2) 动态渲染 ----
            if (prepared.raster)
            {
                vkCmdBeginRendering(passCmd, &prepared.info);

                // viewport / scissor 在所有管线里都设成动态状态，必须显式设置
                cmdSetFullViewportAndScissor(passCmd, prepared.extent);
            }

            // ---- 3) 用户回调（真正的绘制 / dispatch）----
            if (pass.callback)
                pass.callback(passCmd);

            // ---- 4) 结束动态渲染 ----
            if (prepared.raster)
                vkCmdEndRendering(passCmd);

            // P0-4：本 pass 的 GPU 计时结束。
            if (!pass.isCompute)
                onPassTimingEnd(passCmd, prepared.timedPassIndex);

            // ②：pass 结束。放在 vkCmdEndRendering 之后 —— 降级路径用
            // vkCmdUpdateBuffer，而传输命令在渲染通道内非法。
            if (crumbs != nullptr)
                crumbs->endPass(passCmd, Breadcrumbs::slotFor(currentFrameSlot, pass.isCompute), pass.name.c_str());
        }   // P1-1：结束本 pass 的 GPU zone 作用域

        // ---- 5) 记录本 Pass 造成的最终状态，供下一个 Pass 推导屏障 ----
        const int32_t passIndex = prepared.passIndex;
        for (const Access& acc : prepared.accesses)
        {
            TextureEntry& te = textures[acc.texture.id];
            te.externalUsed = true;
            // TM-08：第一次被打上「写」的那一个 Pass 就是生命周期起点。
            if ((acc.access & kWriteBits) != 0 && te.firstWritePass < 0)
                te.firstWritePass = passIndex;
            te.lastUsePass = passIndex;
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
        for (const BufferAccess& acc : prepared.bufferAccesses)
        {
            BufferEntry& be = buffers[acc.buffer.id];
            be.externalUsed = true;
            be.stage = acc.stage;
            be.access = acc.access;
            be.lastAccessQueue = pass.isCompute ? 1 : 0;
        }
    }

    // TM-10：准备 / 录制各自的耗时。只在 --stats-rendergraph 打开时打印，
    // 但计时本身总是累加（两次 steady_clock 读取，远小于一次屏障推导）。
    prepareMicrosTotal += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(prepareEnd - frameClock).count());
    recordMicrosTotal += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - prepareEnd).count());
    ++statsFrames;

    // TM-08：把本帧每个中间目标的字节数与生命周期交给报告模块。
    // 只在 --alias-report 打开时走这一段 —— 关掉时零开销，连 vector 都不建。
    if (tiny::aliasreport::enabled())
    {
        std::vector<tiny::aliasreport::Resource> resources;
        resources.reserve(textures.size());
        for (const TextureEntry& te : textures)
        {
            tiny::aliasreport::Resource r;
            r.name = te.desc.name;
            r.bytes = tiny::aliasreport::estimateBytes(te.desc.format, te.desc.extent.width,
                te.desc.extent.height, te.desc.mipLevels, te.desc.arrayLayers);
            r.firstWrite = te.firstWritePass;
            r.lastUse = te.lastUsePass;
            r.format = te.desc.format;
            r.width = te.desc.extent.width;
            r.height = te.desc.extent.height;
            r.mipLevels = te.desc.mipLevels;
            r.arrayLayers = te.desc.arrayLayers;
            // 导入的外部资源（owned=false）不归渲染图分配，也就谈不上别名；
            // 常驻资源整帧只读，生命周期覆盖全帧，永远不可能和别人错开。
            r.owned = te.owned;
            r.transient = !te.desc.persistent;
            resources.push_back(std::move(r));
        }
        tiny::aliasreport::recordFrame(std::move(resources));
    }

    // T-01：把外部资源交回声明过的 end 状态。
    // 这一段必须在本帧所有 Pass 之后、且在图把 cmd 置空之前 —— 交回的屏障记录在
    // graphics 命令流上（外部持有者通常也在 graphics 侧消费）。
    {
        bool anyReleased = false;
        for (uint32_t i = 0; i < textures.size(); ++i)
        {
            TextureEntry& e = textures[i];
            if (!e.externalDeclared)
                continue;
            Access acc;
            acc.texture = TextureHandle{ i };
            acc.layout = e.externalEnd.layout;
            acc.stage = e.externalEnd.stage;
            acc.access = e.externalEnd.access;
            emitBarrierIfNeeded(acc, cmd, false);
            // 屏障只是把状态发出去；记录的状态由下面的赋值决定（emitBarrierIfNeeded
            // 不改状态，它只读状态推屏障）。
            for (TextureEntry::SubresourceState& state : e.subresources)
            {
                state.layout = e.externalEnd.layout;
                state.stage = e.externalEnd.stage;
                state.access = e.externalEnd.access;
                state.lastAccessQueue = 0;
            }
            e.lastExternalEnd = e.externalEnd;
            e.hasLastExternalEnd = true;
            e.externalDeclared = false;
            anyReleased = true;
        }
        for (uint32_t i = 0; i < buffers.size(); ++i)
        {
            BufferEntry& e = buffers[i];
            if (!e.externalDeclared)
                continue;
            BufferAccess acc;
            acc.buffer = BufferHandle{ i };
            acc.stage = e.externalEnd.stage;
            acc.access = e.externalEnd.access;
            emitBufferBarrierIfNeeded(acc, cmd, false);
            e.stage = e.externalEnd.stage;
            e.access = e.externalEnd.access;
            e.lastAccessQueue = 0;
            e.lastExternalEnd = e.externalEnd;
            e.hasLastExternalEnd = true;
            e.externalDeclared = false;
            anyReleased = true;
        }
        if (anyReleased && cmd != VK_NULL_HANDLE)
            flushImageBarriers(cmd);
    }

    reportUnmatchedPassOverrides();

    // TM-07：这一帧所有 Pass 都写完了，整块只刷新一次（COHERENT 内存直接跳过）。
    device.getScratch().flushIfNeeded();

    // ②：标记两条命令流「跑完了」。读回哨兵值就说明崩溃不在这条流上，
    // 省掉一半的嫌疑范围。
    if (Breadcrumbs* crumbs = device.getBreadcrumbs(); crumbs != nullptr && crumbs->isActive())
    {
        if (cmd != VK_NULL_HANDLE)
            crumbs->finishSlot(cmd, Breadcrumbs::slotFor(currentFrameSlot, false));
        if (computeCmd != VK_NULL_HANDLE)
            crumbs->finishSlot(computeCmd, Breadcrumbs::slotFor(currentFrameSlot, true));
    }

    tiny::framestats::endFrame();
    tiny::resourcestats::endFrame();

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

void PassSwitchboard::set(const std::string& pattern, bool enabled)
{
    if (pattern.empty())
        return;

    for (auto it = entries.begin(); it != entries.end(); ++it)
    {
        if (it->pattern != pattern)
            continue;

        Entry updated = *it;
        updated.enabled = enabled;
        updated.reported = false;
        entries.erase(it);
        entries.push_back(updated);
        return;
    }

    Entry entry;
    entry.pattern = pattern;
    entry.enabled = enabled;
    entries.push_back(entry);
}

void PassSwitchboard::clear()
{
    entries.clear();
}

bool PassSwitchboard::patternMatches(const std::string& pattern, const std::string& passName)
{
    if (pattern.empty())
        return false;
    if (pattern.back() != 0x2a)
        return pattern == passName;

    const std::string prefix = pattern.substr(0, pattern.size() - 1);
    return passName.rfind(prefix, 0) == 0;
}

bool PassSwitchboard::resolve(const std::string& passName)
{
    bool enabled = true;
    for (Entry& entry : entries)
    {
        if (!patternMatches(entry.pattern, passName))
            continue;
        entry.matched = true;
        enabled = entry.enabled;
    }
    return enabled;
}

bool PassSwitchboard::resolveQuiet(const std::string& passName) const
{
    bool enabled = true;
    for (const Entry& entry : entries)
    {
        if (patternMatches(entry.pattern, passName))
            enabled = entry.enabled;
    }
    return enabled;
}

std::vector<std::string> PassSwitchboard::takeUnmatchedPatterns()
{
    std::vector<std::string> unmatched;
    for (Entry& entry : entries)
    {
        if (entry.matched || entry.reported)
            continue;
        entry.reported = true;
        unmatched.push_back(entry.pattern);
    }
    return unmatched;
}

bool renderGraphSwitchboardSelfTest()
{
    const auto fail = [](const char* what)
    {
        std::cout << "[RenderGraph] setPassEnabled selfTest FAIL: " << what << std::endl;
        return false;
    };

    PassSwitchboard board;
    if (!board.resolve("Bloom.Down0"))
        return fail("default should be enabled");

    board.set("Bloom.Down*", false);
    if (board.resolve("Bloom.Down0"))
        return fail("prefix rule did not disable Bloom.Down0");
    if (board.resolve("Bloom.Down12"))
        return fail("prefix rule did not cover Bloom.Down12");
    if (!board.resolve("Bloom.Up0"))
        return fail("prefix rule leaked to Bloom.Up0");

    board.set("Present", false);
    if (board.resolve("Present"))
        return fail("exact rule did not disable Present");
    if (!board.resolve("PresentTransition"))
        return fail("exact rule leaked to PresentTransition");

    board.set("Bloom.*", true);
    board.set("Bloom.Down*", false);
    if (board.resolve("Bloom.Down0"))
        return fail("last set should win for Bloom.Down0");
    if (!board.resolve("Bloom.Up3"))
        return fail("Bloom.Up3 should stay enabled");

    PassSwitchboard diag;
    diag.set("Nope.*", false);
    if (!diag.resolve("Geometry"))
        return fail("unmatched pattern must not disable Geometry");
    const std::vector<std::string> first = diag.takeUnmatchedPatterns();
    if (first.size() != 1 || first.front() != "Nope.*")
        return fail("unmatched pattern should be reported once");
    if (!diag.takeUnmatchedPatterns().empty())
        return fail("unmatched pattern reported twice");

    diag.clear();
    if (!diag.empty())
        return fail("clear should drop all rules");
    if (!diag.resolve("Nope.Match"))
        return fail("clear should restore enabled");

    std::cout << "[RenderGraph] setPassEnabled selfTest PASS | prefix + exact + last-wins + diagnostic" << std::endl;
    return true;
}

RenderGraph::Pass& RenderGraph::addGraphicsPass(const std::string& name)
{
    passes.push_back(std::make_unique<Pass>(name));
    passes.back()->isCompute = false;
    // 跨帧开关在注册时重放：passes 每帧重建，这是唯一可靠的时机。
    passes.back()->enabled = passSwitchboard.resolve(name);
    return *passes.back();
}

RenderGraph::Pass& RenderGraph::addComputePass(const std::string& name)
{
    passes.push_back(std::make_unique<Pass>(name));
    passes.back()->isCompute = true;
    passes.back()->enabled = passSwitchboard.resolve(name);
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
    tiny::resourcestats::countBuffer();

    if (e.desc.hostVisible)
        e.mapped = allocInfo.pMappedData;
}

void RenderGraph::createOwnedTexture(TextureEntry& e)
{
    // T-04：图像与视图都从池里拿。键里带精确尺寸，所以尺寸变了就是另一个键；
    // 尺寸换回旧值时命中，省掉 vkCreateImage + 分配 + vkCreateImageView 三件事。
    const tiny::RenderTargetPool::Desc poolDesc = poolDescFromTexture(e.desc, e.isDepth);
    const tiny::RenderTargetPool::Image image = texturePool->acquire(poolDesc, e.desc.name);
    e.image = image.image;
    e.allocation = image.allocation;
    e.view = image.view;
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

    // T-04：换下来的图交回池，不直接销毁 —— 尺寸来回切换时它们还能被复用。
    // 池内部按帧号退休（kRetireFrames），所以这里不必等 GPU。
    if (e.image != VK_NULL_HANDLE && texturePool)
    {
        texturePool->release(poolDescFromTexture(e.desc, e.isDepth),
            tiny::RenderTargetPool::Image{ e.image, e.allocation, e.view });
        e.image = VK_NULL_HANDLE;
        e.allocation = VK_NULL_HANDLE;
        e.view = VK_NULL_HANDLE;
    }
    else if (e.image != VK_NULL_HANDLE)
    {
        if (e.view != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), e.view, nullptr);
        vmaDestroyImage(device.getVmaAllocator(), e.image, e.allocation);
        e.image = VK_NULL_HANDLE;
        e.allocation = VK_NULL_HANDLE;
        e.view = VK_NULL_HANDLE;
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

namespace
{
    const char* layoutName(VkImageLayout layout)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC_KHR";
        default: return "OTHER";
        }
    }
}

void RenderGraph::declareExternalTexture(TextureHandle handle, const ExternalState& start, const ExternalState& end)
{
    if (handle.id >= textures.size())
        throw std::runtime_error("RenderGraph: declareExternalTexture 收到了越界句柄");

    TextureEntry& e = textures[handle.id];
    if (e.owned)
        throw std::runtime_error("RenderGraph: \"" + e.desc.name +
            "\" 由图自己创建（createTexture），不需要外部状态声明");
    if (e.externalUsed)
        throw std::runtime_error("RenderGraph: \"" + e.desc.name +
            "\" 的状态声明晚于本帧的第一次使用 —— 必须在注册 Pass 之前声明");
    if (e.externalDeclared)
        throw std::runtime_error("RenderGraph: \"" + e.desc.name + "\" 本帧已经声明过外部状态");

    // 跨帧漂移：上一帧交回的状态必须等于这一帧声明的入口状态。
    // 这是外部资源唯一的一致性检查 —— 少了它，"某条路径忘了转回去"只会在
    // 几十帧之后以一条 syncval 报文或一张花屏的形式出现。
    if (e.hasLastExternalEnd && e.lastExternalEnd != start)
    {
        throw std::runtime_error("RenderGraph: 外部纹理 \"" + e.desc.name + "\" 状态漂移：上一帧交回 " +
            layoutName(e.lastExternalEnd.layout) + "，本帧声明入口 " + layoutName(start.layout));
    }

    // 入口状态就是这一段推导的起点：把它写进每个子资源，帧首不需要额外屏障。
    for (TextureEntry::SubresourceState& state : e.subresources)
    {
        state.layout = start.layout;
        state.stage = start.stage;
        state.access = start.access;
        state.lastAccessQueue = -1;
    }

    e.externalDeclared = true;
    e.externalEnd = end;
}

void RenderGraph::declareExternalBuffer(BufferHandle handle, const ExternalState& start, const ExternalState& end)
{
    if (handle.id >= buffers.size())
        throw std::runtime_error("RenderGraph: declareExternalBuffer 收到了越界句柄");

    BufferEntry& e = buffers[handle.id];
    if (e.owned)
        throw std::runtime_error("RenderGraph: \"" + e.desc.name +
            "\" 由图自己创建（createBuffer），不需要外部状态声明");
    if (e.externalUsed)
        throw std::runtime_error("RenderGraph: \"" + e.desc.name +
            "\" 的状态声明晚于本帧的第一次使用 —— 必须在注册 Pass 之前声明");
    if (e.externalDeclared)
        throw std::runtime_error("RenderGraph: \"" + e.desc.name + "\" 本帧已经声明过外部状态");

    if (e.hasLastExternalEnd && e.lastExternalEnd != start)
    {
        throw std::runtime_error("RenderGraph: 外部 buffer \"" + e.desc.name + "\" 状态漂移："
            "上一帧交回的 (stage, access) 与本帧声明的入口不一致");
    }

    e.stage = start.stage;
    e.access = start.access;
    e.lastAccessQueue = -1;

    e.externalDeclared = true;
    e.externalEnd = end;
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
    passSwitchboard.set(passName, enabled);

    // 同一帧内已经注册过的 Pass 立即生效（先注册后调用是常见写法）。
    // 只碰被这条规则命中的 Pass：其余的 enabled 保持它们自己 setEnabled 的结果，
    // 否则 TINY_SIMULATE_SSAO_SKIP 这种「故意关掉某个 Pass」的反例会被悄悄打开。
    for (std::unique_ptr<Pass>& p : passes)
    {
        if (!PassSwitchboard::patternMatches(passName, p->name))
            continue;
        p->enabled = passSwitchboard.resolveQuiet(p->name);
    }
}

void RenderGraph::clearPassOverrides()
{
    passSwitchboard.clear();
}

bool RenderGraph::isPassEnabled(const std::string& passName) const
{
    return passSwitchboard.resolveQuiet(passName);
}

bool RenderGraph::wasPassSkippedLastFrame(const std::string& passName) const
{
    return std::find(lastFrameSkippedPasses.begin(), lastFrameSkippedPasses.end(), passName)
        != lastFrameSkippedPasses.end();
}

void RenderGraph::reportUnmatchedPassOverrides()
{
    for (const std::string& pattern : passSwitchboard.takeUnmatchedPatterns())
    {
        std::cout << "[RenderGraph] setPassEnabled: 模式 " << pattern
                  << " 本帧没有匹配任何 Pass（拼写错误？以 * 结尾表示前缀匹配）" << std::endl;
    }
}

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

    // 深度清屏值由调用方显式给出，这里不再静默改写：
    // 旧版会把 depth==0 && stencil==0 悄悄改成 {1.0, 0}，想清 0 的调用方被坑，
    // 而 VkClearValue{} 的 0.0f 本身是合法取值。需要清 1.0 就自己传。

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
// T-04：--no-rt-pool 的整体回退开关。关掉之后每次 acquire 都新建、每次 release 都销毁，
// 行为与池化之前一致，用来做 A/B 与出问题时的退路。
void RenderGraph::setRenderTargetPoolEnabled(bool enabled)
{
    if (texturePool) texturePool->setEnabled(enabled);
}

void RenderGraph::reportRenderTargetPool() const
{
    if (texturePool) texturePool->report();
}