#pragma once

#include "IRenderPass.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include <array>
#include <cstdint>
#include <memory>

// TD-02 的跨队列 producer/consumer 验收探针。
//
// 每帧交替验证一个方向，避免同一帧出现双向依赖环：
//   偶数帧：compute 写 SSBO -> graphics 读 SSBO 并写 host-visible 结果（C->G）
//   奇数帧：graphics 写 storage image -> compute 读图/写回 + 写结果（G->C）
//
// 资源创建时显式设置 crossQueue=true，RenderGraph 会使用
// VK_SHARING_MODE_CONCURRENT；Renderer 根据依赖方向插入 timeline semaphore wait。
class CrossQueueProbePass : public IRenderPass
{
public:
    CrossQueueProbePass(Device& device, RenderGraph& graph, DescriptorManager* descriptorManager);
    ~CrossQueueProbePass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "CrossQueueProbe"; }

    void setEnabled(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }

private:
    struct ProbeResult
    {
        uint32_t magic;
        uint32_t frameNumber;
        uint32_t direction;   // 0 = C->G，1 = G->C
        uint32_t checksum;
        uint32_t count;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
    };

    struct ProbePush
    {
        uint32_t mode;
        uint32_t frameNumber;
        uint32_t count;
        uint32_t pad;
    };

    static constexpr uint32_t kValueCount = 256;
    static constexpr uint32_t kImageSize = 64;
    static constexpr uint32_t kComputeToGraphicsMagic = 0x54D02000u;
    static constexpr uint32_t kGraphicsToComputeMagic = 0x54D02001u;

    bool enabled = false;
    Device& device;
    RenderGraph& renderGraph;
    DescriptorManager* descriptorManager;

    std::array<RenderGraph::BufferHandle, kFramesInFlight> inputBuffers{};
    std::array<RenderGraph::BufferHandle, kFramesInFlight> graphicsResults{};
    std::array<RenderGraph::BufferHandle, kFramesInFlight> computeResults{};
    std::array<RenderGraph::TextureHandle, kFramesInFlight> storageImages{};
    std::array<RenderGraph::TextureHandle, kFramesInFlight> colorTargets{};

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout graphicsPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> graphicsSets{};
    std::array<VkDescriptorSet, kFramesInFlight> computeSets{};

    std::unique_ptr<Pipeline> graphicsPipeline;
    std::unique_ptr<ComputePipeline> computePipeline;

    // ---- T-01：图外持有的 buffer ----
    // 这个 buffer 由本探针自己用 VMA 分配，生命周期不归渲染图管；图只拿到
    // VkBuffer 的句柄包装。入口 / 出口状态必须由 declareExternalBuffer 声明，
    // 否则图第一次访问它时只能假定 UNDEFINED + NONE。
    static constexpr uint32_t kExternalValueCount = 64;   // 256B，够验证内容
    static constexpr uint32_t kExternalPattern = 0xA5A5A5A5u;
    RenderGraph::BufferHandle externalBuffer{};
    VkBuffer externalBufferVk = VK_NULL_HANDLE;
    VmaAllocation externalAllocation = VK_NULL_HANDLE;
    bool externalImported = false;
    bool externalDriftTested = false;
    // 负向验收要等「声明 + 交回」跑满一轮：hasLastExternalEnd 才有值。
    uint32_t externalDeclareCount = 0;
    // 每个槽位上一次排队「copy 到回读缓冲」的帧号（0 = 还没排过）。帧号从 1 起，
    // 所以不能靠 frameNumber - kFramesInFlight 反推 —— 那个式子会指到不存在的第 0 帧。
    std::array<uint64_t, kFramesInFlight> externalCopyFrames{};
    bool reportedExternalAccess = false;
    uint32_t externalVerifiedFrames = 0;
    std::array<RenderGraph::BufferHandle, kFramesInFlight> externalReadback{};

    bool reportedComputeToGraphics = false;
    bool reportedGraphicsToCompute = false;
};