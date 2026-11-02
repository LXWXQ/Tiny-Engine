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

    bool reportedComputeToGraphics = false;
    bool reportedGraphicsToCompute = false;
};