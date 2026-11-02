#pragma once

#include "Device.h"
#include "GpuTimer.h"   // P0-4：每 pass GPU 计时

class GpuProfiler;   // P1-1：Tracy GPU zone（前置声明，避免头文件污染）
#include <vulkan/vulkan.h>
#include <string>
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

        // 作为深度附件写入
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

    // P0-4：每 pass GPU 计时挂钩。SENTINEL_ENABLE_GPU_TIMING 关闭时是空实现，
    // 默认构建为此一处也不产生（零分支、零命令）。
    void setGpuTimer(GpuTimer* timer) { gpuTimer = timer; }
    void setGpuProfiler(GpuProfiler* profiler) { gpuProfiler = profiler; }
    inline void onPassTimingBegin(VkCommandBuffer cmd, uint32_t passIndex, const char* name)
    {
#ifdef SENTINEL_ENABLE_GPU_TIMING
        if (gpuTimer) gpuTimer->beginPass(cmd, currentFrameSlot, passIndex, name);
#else
        (void)cmd; (void)passIndex; (void)name;
#endif
    }
    inline void onPassTimingEnd(VkCommandBuffer cmd, uint32_t passIndex)
    {
#ifdef SENTINEL_ENABLE_GPU_TIMING
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

    // 供 ImGui 面板开关某个 Pass
    void setPassEnabled(const std::string& passName, bool enabled);

    // 把某个纹理转到 PRESENT_SRC_KHR，必须在本帧最后一个 Pass 之后调用。
    // 渲染图不应该知道「哪张图要被 present」，所以由应用显式声明。
    void presentTransition(TextureHandle handle);

private:
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
    };

    void createOwnedTexture(TextureEntry& entry);
    void createOwnedBuffer(BufferEntry& entry);
    SubresourceRange normalizeTextureRange(const TextureEntry& entry, const SubresourceRange& range) const;
    BufferRange normalizeBufferRange(const BufferEntry& entry, const BufferRange& range) const;
    void emitBarrierIfNeeded(const Access& acc, VkCommandBuffer target, bool currentQueueIsCompute);
    void emitBufferBarrierIfNeeded(const BufferAccess& acc, VkCommandBuffer target, bool currentQueueIsCompute);

    // 语义校验：瞬时资源每帧必须有人写。
    // 与屏障推导（同步正确性）是两件不同的事 —— 这个查的是"语义正确性"，
    // 验证层对此一言不发（从 UNDEFINED 布局转出来本身完全合法）。
    void validateWriteCoverage();

    // P0-4：GPU 计时（nullptr = 关闭；编译期关闭时该指针恒为 nullptr）。
    GpuTimer* gpuTimer = nullptr;
    GpuProfiler* gpuProfiler = nullptr;   // P1-1
    uint32_t currentFrameSlot = 0;

    Device& device;
    VkCommandBuffer cmd = VK_NULL_HANDLE;          // graphics command buffer
    VkCommandBuffer computeCmd = VK_NULL_HANDLE;   // async compute command buffer
    std::vector<TextureEntry> textures;
    std::vector<BufferEntry> buffers;
    std::vector<std::unique_ptr<Pass>> passes;
    CrossQueueDependencies crossQueueDependencies{};
};