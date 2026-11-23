#pragma once

#include "Device.h"
#include "GpuTimer.h"   // P0-4：每 pass GPU 计时

class GpuProfiler;   // P1-1：Tracy GPU zone（前置声明，避免头文件污染）
class JobSystem;     // TM-10：Pass 准备阶段并行（前置声明，别把线程池的头拖进来）
#include <vulkan/vulkan.h>
#include <string>
#include <cstddef>
#include <vector>
#include <functional>
#include <memory>

// ============================================================================
// 线性渲染图
//
// 目标：把「手写 vkCmdPipelineBarrier」从每个 Pass 里彻底移除。
//
//   graph.beginFrame(cmd);
//     auto& p = graph.addGraphicsPass("BloomDownsample");
//     p.read(bloomSrc);                              // 声明采样 -> 自动插 SHADER_READ 屏障
//     p.writeColor(bloomDst, LOAD_OP_DONT_CARE);     // 声明渲染目标 -> 自动插 COLOR_ATTACHMENT 屏障
//     p.execute([](VkCommandBuffer c) { /* 绘制 */ });
//   graph.endFrame();
//
// 每个资源会记住「上一次被谁、以什么布局访问」，下次访问时自动插入最小必要的屏障。
//
// 为什么叫「线性」：Pass 严格按声明顺序执行，不做重排 / 别名化 / 自动剔除。
// 这已经能解决 90% 的痛点（不再手写屏障），而复杂度和调试难度远低于完整 DAG 渲染图。
// 将来要加自动剔除 / 内存别名化时，这个结构也留了位置。
//
// 配合 Vulkan 1.3 的 dynamic rendering：不再有 VkRenderPass / VkFramebuffer，
// 每个 Pass 只是 vkCmdBeginRendering + 一组 imageView —— 也就不会再出现
// 「Framebuffer 生命周期」那一类坑。
// ============================================================================
// ---------------------------------------------------------------------------
// 跨帧 Pass 开关。
//
// RenderGraph::passes 每帧重建（endFrame 末尾 reset + clear，录制时再注册），
// 所以「开关」不能只写在 Pass 上，必须按名字记住意图，在注册时重放。
// 旧版 setPassEnabled 只遍历当前帧的 passes —— 帧间调用时列表是空的，
// 于是静默无效（tech-debt #9）。
//
// 规则：
//   - 精确名字匹配；以星号结尾时按前缀匹配（Bloom.Down* 覆盖 Down0..DownN）。
//   - 规则按「最近一次设置」排序，后设置的覆盖先设置的。
//   - 某条规则整帧没有命中任何 Pass 时，endFrame 打印一次诊断，
//     避免名字拼错之后只表现为「开关没反应」。
//   - 不碰 Vulkan，可独立单测：开关匹配错了不该等到跑起来才发现。
// ---------------------------------------------------------------------------
class PassSwitchboard
{
public:
    void set(const std::string& pattern, bool enabled);
    void clear();
    bool empty() const { return entries.empty(); }
    std::size_t size() const { return entries.size(); }

    static bool patternMatches(const std::string& pattern, const std::string& passName);

    // 按「最近一次设置」的顺序遍历，最后命中的规则生效；命中会记录到规则上。
    bool resolve(const std::string& passName);

    // 只查询，不改变任何状态。
    bool resolveQuiet(const std::string& passName) const;

    // 取走「从未命中过任何 Pass」的规则（每条规则只报一次）。
    std::vector<std::string> takeUnmatchedPatterns();

private:
    struct Entry
    {
        std::string pattern;
        bool enabled = true;
        bool matched = false;
        bool reported = false;
    };
    std::vector<Entry> entries;
};

// 纯逻辑自检：不创建设备、不录命令。覆盖精确/前缀/最近优先/诊断去重。
bool renderGraphSwitchboardSelfTest();
class RenderGraph
{
public:
    struct TextureHandle
    {
        uint32_t id = UINT32_MAX;
        bool valid() const { return id != UINT32_MAX; }
    };

    struct BufferHandle
    {
        uint32_t id = UINT32_MAX;
        bool valid() const { return id != UINT32_MAX; }
    };

    // TD-05: image access range. levelCount/layerCount == 0 means "all
    // remaining subresources"; callers normally leave the default for full-image
    // access, and only specify a range for mip/layer-aware work.
    struct SubresourceRange
    {
        uint32_t baseMipLevel = 0;
        uint32_t levelCount = 0;
        uint32_t baseArrayLayer = 0;
        uint32_t layerCount = 0;
    };

    // Buffer access interval. size == VK_WHOLE_SIZE means to the end of buffer.
    struct BufferRange
    {
        VkDeviceSize offset = 0;
        VkDeviceSize size = VK_WHOLE_SIZE;
    };

    struct TextureDesc
    {
        std::string name;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkImageUsageFlags usage = 0;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        VkImageCreateFlags flags = 0;

        // true = 该资源会被 graphics / compute 两条队列共同访问。
        // 当前实现走 Vulkan 的简单路径：创建时使用 VK_SHARING_MODE_CONCURRENT，
        // 因此不需要 queue family ownership transfer；跨队列执行顺序由 Renderer
        // 根据本帧检测出的依赖方向用 timeline semaphore 串起来。
        // 导入的外部资源必须在调用方额外保证底层资源本身也是 CONCURRENT sharing。
        bool crossQueue = false;

        // 每帧内容都会被重新生成，还是跨帧常驻？
        //
        // 这个标记是渲染图"语义校验"的依据（见 endFrame 里的检查）：
        //   瞬时资源（false）—— 每帧都必须至少有一个 Pass 写它。内容不会被保留，
        //                      所以"没人写"就等于"下游读到未定义内容"。
        //   常驻资源（true） —— 初始化时写一次，之后每帧只读（IBL 烘焙结果就是
        //                      这种），允许整帧没有写入者。
        //
        // 为什么不能靠"有没有被写"自动判断：那样只能事后发现，而这里要的是
        // 在错误发生的那一帧直接报出来。
        bool persistent = false;
    };

    // 一次资源访问：目标布局 + 在哪个阶段 + 什么访问类型
    struct Access
    {
        TextureHandle texture;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 stage = 0;
        VkAccessFlags2 access = 0;
        SubresourceRange range{};
    };

    struct BufferDesc
    {
        std::string name;
        VkDeviceSize size = 0;
        VkBufferUsageFlags usage = 0;
        // true = 跨帧常驻（初始化写一次，之后只读）；false = 每帧重写。
        bool persistent = false;
        // true = VMA 分配 Host-Visible 映射，用于 GPU 写、CPU 读回的调试/统计缓冲。
        // 大块几何数据不应打开这个选项。
        bool hostVisible = false;
        // true = graphics / compute 两条队列都会访问。创建时改用 CONCURRENT sharing；
        // 导入 buffer 时由调用方保证原 buffer 的 sharing mode 正确。
        bool crossQueue = false;
    };

    // Buffer 没有 layout，只需要记录上一次的 stage/access 推导 hazard。
    struct BufferAccess
    {
        BufferHandle buffer;
        VkPipelineStageFlags2 stage = 0;
        VkAccessFlags2 access = 0;
        BufferRange range{};
    };

    struct ColorAttachment
    {
        TextureHandle texture;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkClearValue clearValue{};
    };

    struct DepthAttachment
    {
        TextureHandle texture;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkClearValue clearValue{};
    };

    class Pass
    {
    public:
        Pass(std::string name) : name(std::move(name)) {}

        // ---- 资源访问声明（会被翻译成屏障）----

        // 作为纹理采样（片元着色器）
        Pass& read(TextureHandle t,
                   VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VkAccessFlags2 access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   const SubresourceRange& range = {});

        // 作为颜色附件写入
        Pass& writeColor(TextureHandle t,
                         VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                         VkClearValue clear = {},
                         VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                         const SubresourceRange& range = {});

        // 作为深度附件写入。clear 原样生效（默认 {} 就是深度 0.0），
        // 不做"depth==0 视为未设置"的改写 —— 想清 1.0 请显式传入。
        Pass& writeDepth(TextureHandle t,
                         VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                         VkClearValue clear = {},
                         VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                         const SubresourceRange& range = {});

        // 作为存储图像（compute / 计算写入）
        Pass& writeStorage(TextureHandle t,
                           VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VkAccessFlags2 access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           const SubresourceRange& range = {});

        /// 计算着色器读取存储图像
        Pass& readStorage(TextureHandle t,
                          VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VkAccessFlags2 access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                          const SubresourceRange& range = {});

        // 存储缓冲读 / 写。D-5 先覆盖 SSBO 场景（compute 剔除、体素生成等）。
        Pass& readBuffer(BufferHandle b,
                         VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VkAccessFlags2 access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                         const BufferRange& range = {});
        Pass& writeBuffer(BufferHandle b,
                          VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VkAccessFlags2 access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          const BufferRange& range = {});

        // 只做布局转换，不作为渲染目标。
        // 典型用途：把交换链图像转到 PRESENT_SRC_KHR 交给 present。
        Pass& transitionTo(TextureHandle t, VkImageLayout layout,
                           VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE,
                           VkAccessFlags2 access = VK_ACCESS_2_NONE,
                           const SubresourceRange& range = {});

        Pass& execute(std::function<void(VkCommandBuffer)> fn) { callback = std::move(fn); return *this; }

        Pass& setEnabled(bool e) { enabled = e; return *this; }
        bool isEnabled() const { return enabled; }
        const std::string& getName() const { return name; }

        // 不指定时，渲染图会用第一个颜色附件的尺寸自动设置 viewport / scissor
        Pass& setExtent(VkExtent2D e) { extent = e; hasExtent = true; return *this; }

    private:
        friend class RenderGraph;
        std::string name;
        std::vector<Access> accesses;
        std::vector<BufferAccess> bufferAccesses;
        std::vector<ColorAttachment> colors;
        std::vector<DepthAttachment> depths;
        std::function<void(VkCommandBuffer)> callback;
        bool enabled = true;
        bool isCompute = false;
        bool hasExtent = false;
        VkExtent2D extent{};
    };

    explicit RenderGraph(Device& device);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // ---- 帧生命周期 ----
    // D-5：graphics / compute 两条命令流。compute 为 VK_NULL_HANDLE 时，
    // 所有 pass 仍按原来的单队列路径记录（便于旧调用方/测试降级）。
    void beginFrame(VkCommandBuffer graphicsCommandBuffer,
                    VkCommandBuffer computeCommandBuffer = VK_NULL_HANDLE,
                    uint32_t frameSlot = 0,
                    uint64_t frameNumber = 0);
    void endFrame();     // 按顺序执行所有 Pass

    // TM-10：把每个 Pass 的「准备」（访问去重、声明校验、组装附件描述）与
    // 「录制」（屏障 + vkCmdBeginRendering + 回调）分开。准备阶段只读写 Pass
    // 自己的声明和 textures / buffers 的只读字段，不碰 VkCommandBuffer，可以并行；
    // 录制阶段依赖上一个 Pass 留下的布局，必须串行。默认关闭（--parallel-prepare）。
    void setJobSystem(JobSystem* jobs) { jobSystem = jobs; }
    void setParallelPrepare(bool enabled) { parallelPrepare = enabled; }
    bool isParallelPrepare() const { return parallelPrepare; }
    // 准备与录制各自的每帧耗时（--stats-rendergraph）。
    void reportPrepareStats() const;

    // P0-4：每 pass GPU 计时挂钩。TINY_ENABLE_GPU_TIMING 关闭时是空实现，
    // 默认构建为此一处也不产生（零分支、零命令）。
    void setGpuTimer(GpuTimer* timer) { gpuTimer = timer; }
    void setGpuProfiler(GpuProfiler* profiler) { gpuProfiler = profiler; }
    inline void onPassTimingBegin(VkCommandBuffer cmd, uint32_t passIndex, const char* name)
    {
#ifdef TINY_ENABLE_GPU_TIMING
        if (gpuTimer) gpuTimer->beginPass(cmd, currentFrameSlot, passIndex, name);
#else
        (void)cmd; (void)passIndex; (void)name;
#endif
    }
    inline void onPassTimingEnd(VkCommandBuffer cmd, uint32_t passIndex)
    {
#ifdef TINY_ENABLE_GPU_TIMING
        if (gpuTimer) gpuTimer->endPass(cmd, currentFrameSlot, passIndex);
#else
        (void)cmd; (void)passIndex;
#endif
    }

    // ---- Pass 注册 ----
    Pass& addGraphicsPass(const std::string& name);
    Pass& addComputePass(const std::string& name);

    // ---- 资源 ----
    // 由渲染图创建并持有（用于 Bloom 之类需要中间目标的场景）
    TextureHandle createTexture(const TextureDesc& desc);

    // 导入外部已有的资源（交换链图像、G-Buffer 等），生命周期由调用方管理
    TextureHandle importTexture(const TextureDesc& desc, VkImage image, VkImageView view,
                                VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    BufferHandle createBuffer(const BufferDesc& desc);
    BufferHandle importBuffer(const BufferDesc& desc, VkBuffer buffer);

    // TD-03: resize in place so existing handles stored by passes stay valid.
    // Caller must wait for the GPU to be idle before calling these methods.
    void resizeOwnedTexture(TextureHandle handle, VkExtent2D extent);
    void updateImportedTexture(TextureHandle handle, const TextureDesc& desc,
                               VkImage image, VkImageView view,
                               VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    VkImage     getImage(TextureHandle h) const;
    VkImageView getView(TextureHandle h) const;
    VkFormat    getFormat(TextureHandle h) const;
    VkExtent2D  getExtent(TextureHandle h) const;
    const TextureDesc& getDesc(TextureHandle h) const;

    VkBuffer    getBuffer(BufferHandle h) const;
    const BufferDesc& getBufferDesc(BufferHandle h) const;
    void*       getBufferMapped(BufferHandle h) const;
    void        invalidateBuffer(BufferHandle h) const;

    // TD-02：RenderGraph 在 endFrame() 中根据资源访问声明算出两条队列之间的
    // producer/consumer 方向。Renderer 用这两个标志决定 submit 顺序和 timeline wait。
    // 若同一帧检测到双向依赖，endFrame() 会直接抛异常（单条命令缓冲无法表达环）。
    struct CrossQueueDependencies
    {
        bool computeToGraphics = false;
        bool graphicsToCompute = false;
    };
    const CrossQueueDependencies& getCrossQueueDependencies() const { return crossQueueDependencies; }

    // 供 ImGui 面板 / 配置开关某个 Pass。语义见 PassSwitchboard：
    // 跨帧保存，注册时重放；同一帧内已注册的 Pass 立即生效。
    // 名字以星号结尾时按前缀匹配（Bloom.Up*）。
    // 注意：禁用整个 Pass 会把它的屏障声明一起跳过 —— 后面还有 Pass 读它写的
    // 瞬时资源时，validateWriteCoverage 会在提交前报「该子资源这一帧没人写」，
    // 并指出它本来由哪个 Pass 写。成组关闭要连消费者一起关。
    void setPassEnabled(const std::string& passName, bool enabled);
    // 清空全部开关（测试与恢复默认用）。
    void clearPassOverrides();
    // 当前意图（没有规则命中时为 true）；不改变任何状态。
    bool isPassEnabled(const std::string& passName) const;
    const PassSwitchboard& getPassSwitchboard() const { return passSwitchboard; }

    // 上一帧实际录制 / 被跳过的 Pass 名（登记顺序）。
    // 自检用它断言关掉的 Pass 真的没录，调试面板可以直接显示。
    const std::vector<std::string>& getLastFrameExecutedPasses() const { return lastFrameExecutedPasses; }
    const std::vector<std::string>& getLastFrameSkippedPasses() const { return lastFrameSkippedPasses; }
    bool wasPassSkippedLastFrame(const std::string& passName) const;

    // 把某个纹理转到 PRESENT_SRC_KHR，必须在本帧最后一个 Pass 之后调用。
    // 渲染图不应该知道「哪张图要被 present」，所以由应用显式声明。
    void presentTransition(TextureHandle handle);

    // ---- T-01：外部资源的状态声明 ----
    // 有些资源由图外的代码创建并持有（暂存区的簇表、阴影图、截图缓冲、外部池子
    // 分配的 RT）。图能看见它们的 VkImage / VkBuffer，但不知道两件事：
    //   1) 这一帧开始前，外部世界把它留在了什么状态 —— 于是第一次访问时推导不出
    //      正确的 srcStage / srcAccess（只能假定 UNDEFINED + NONE）；
    //   2) 帧尾要交回什么状态 —— 于是外部代码只能自己手写屏障。
    //
    // declareExternalTexture / declareExternalBuffer 就是补这两个未知：
    //   start = 外面交给图时的状态（图据此给第一次访问推导屏障，帧首不额外发屏障）；
    //   end   = 图交回外面时的状态（帧尾补一条屏障，然后记住它）。
    //
    // 三条不变量（违反即抛异常，这是它比"手写屏障"值钱的地方）：
    //   - 每个资源每帧最多声明一次；
    //   - 声明必须早于本帧对该资源的第一次使用（endFrame 之前）；
    //   - 本帧的 start 必须等于上一帧交回的 end，否则报"状态漂移"并指出两边。
    struct ExternalState
    {
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // buffer 忽略此字段
        VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 access = VK_ACCESS_2_NONE;

        bool operator==(const ExternalState& other) const
        {
            return layout == other.layout && stage == other.stage && access == other.access;
        }
        bool operator!=(const ExternalState& other) const { return !(*this == other); }
    };

    void declareExternalTexture(TextureHandle handle, const ExternalState& start, const ExternalState& end);
    void declareExternalBuffer(BufferHandle handle, const ExternalState& start, const ExternalState& end);

private:
    PassSwitchboard passSwitchboard;
    std::vector<std::string> lastFrameExecutedPasses;
    std::vector<std::string> lastFrameSkippedPasses;
    void reportUnmatchedPassOverrides();

    struct TextureEntry
    {
        TextureDesc desc;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        bool owned = false;
        bool isDepth = false;
        // true = 每帧开始把布局记录复位成 UNDEFINED（瞬时资源）；
        // false = 跨帧常驻的只读资源（如 IBL cubemap），复位会被当成
        //         「内容未定义」而丢弃。
        bool resetEachFrame = true;

        // TM-08：本帧的生命周期边界（Pass 序号，取 lastFrameExecutedPasses 的下标）。
        // 跳过的 Pass 不占号，所以这里和调试面板看到的顺序一致。
        // firstWritePass < 0 表示整帧没人写 —— 这种资源不能参与别名（内容无意义）。
        int32_t firstWritePass = -1;
        int32_t lastUsePass = -1;

        // T-01：外部持有的资源在这一帧的状态声明（见 declareExternalTexture）。
        bool externalDeclared = false;
        bool externalUsed = false;
        ExternalState externalEnd;
        ExternalState lastExternalEnd;
        bool hasLastExternalEnd = false;

        // 每个 mip/layer 独立记录上一次访问后的状态。
        struct SubresourceState
        {
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 access = VK_ACCESS_2_NONE;
            int lastAccessQueue = -1;
        };
        std::vector<SubresourceState> subresources;
    };

    struct BufferEntry
    {
        BufferDesc desc;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool owned = false;

        VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 access = VK_ACCESS_2_NONE;
        int lastAccessQueue = -1;

        // T-01：外部持有的资源在这一帧的状态声明（见 declareExternalBuffer）。
        bool externalDeclared = false;
        bool externalUsed = false;
        ExternalState externalEnd;
        ExternalState lastExternalEnd;
        bool hasLastExternalEnd = false;
    };

    void createOwnedTexture(TextureEntry& entry);
    void createOwnedBuffer(BufferEntry& entry);
    SubresourceRange normalizeTextureRange(const TextureEntry& entry, const SubresourceRange& range) const;
    BufferRange normalizeBufferRange(const BufferEntry& entry, const BufferRange& range) const;
    void emitBarrierIfNeeded(const Access& acc, VkCommandBuffer target, bool currentQueueIsCompute);
    void emitBufferBarrierIfNeeded(const BufferAccess& acc, VkCommandBuffer target, bool currentQueueIsCompute);
    // T-03：图像屏障先进队列，由 flushImageBarriers 在 Pass 命令之前按桶合并发射。
    void flushImageBarriers(VkCommandBuffer target);

    // TM-10：一个 Pass 的准备工作结果。全部是纯数据 —— 指针指向自己的 vector，
    // 所以对象必须放在堆上（preparedPasses 存 unique_ptr）：vector 一旦搬家，
    // VkRenderingInfo 里的 pColorAttachments 就成了悬空指针。
    struct PreparedPass
    {
        Pass* pass = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t timedPassIndex = 0;   // 只有 graphics pass 有（P0-4 的序号）
        int32_t passIndex = -1;        // 本帧执行序号（TM-08 的生命周期口径）
        bool raster = false;
        VkExtent2D extent{ 0, 0 };
        std::vector<Access> accesses;              // 去重后的纹理访问
        std::vector<BufferAccess> bufferAccesses;  // 去重后的 buffer 访问
        std::vector<VkRenderingAttachmentInfo> colorInfos;
        VkRenderingAttachmentInfo depthInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        bool hasDepth = false;
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        // 并行阶段不能抛异常（抛出会随线程调度改变报错顺序），错误先记在这里，
        // 回到串行阶段按 Pass 顺序抛。
        std::string error;
    };

    void preparePass(PreparedPass& prepared);
    // 语义校验：瞬时资源每帧必须有人写。
    // 与屏障推导（同步正确性）是两件不同的事 —— 这个查的是"语义正确性"，
    // 验证层对此一言不发（从 UNDEFINED 布局转出来本身完全合法）。
    void validateWriteCoverage();

    // P0-4：GPU 计时（nullptr = 关闭；编译期关闭时该指针恒为 nullptr）。
    GpuTimer* gpuTimer = nullptr;
    GpuProfiler* gpuProfiler = nullptr;   // P1-1
    uint32_t currentFrameSlot = 0;

    // TM-10：准备阶段并行。jobSystem 为空时无条件走串行路径（headless 测试也走）。
    JobSystem* jobSystem = nullptr;
    bool parallelPrepare = false;
    // 每帧复用，避免每个 Pass 都重新分配 vector（沿用容量）。
    std::vector<std::unique_ptr<PreparedPass>> preparedPasses;
    uint64_t statsFrames = 0;
    uint64_t prepareMicrosTotal = 0;
    uint64_t recordMicrosTotal = 0;

    Device& device;
    VkCommandBuffer cmd = VK_NULL_HANDLE;          // graphics command buffer
    VkCommandBuffer computeCmd = VK_NULL_HANDLE;   // async compute command buffer
    std::vector<TextureEntry> textures;
    std::vector<BufferEntry> buffers;
    std::vector<std::unique_ptr<Pass>> passes;
    std::vector<VkImageMemoryBarrier2> pendingImageBarriers;
    CrossQueueDependencies crossQueueDependencies{};
};