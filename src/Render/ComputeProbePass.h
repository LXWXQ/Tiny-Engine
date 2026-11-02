#pragma once

#include "IRenderPass.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"
#include "RHI/Pipeline.h"
#include "RHI/Device.h"
#include "RHI/RenderGraph.h"
#include <array>
#include <cstdint>
#include <memory>

// D-5 的异步计算验收探针。
//
// 生产用途的 GPU culling / 体素生成会替换掉它，但它们需要的底层能力与这里相同：
//   * RenderGraph 的 storage buffer handle / read-write 声明 / buffer barrier；
//   * compute pipeline / descriptor set / push constant；
//   * 独立 compute command buffer + compute timeline semaphore；
//   * GPU 写、CPU 读回的 host-visible buffer 验证路径。
class ComputeProbePass : public IRenderPass
{
public:
    ComputeProbePass(Device& device, RenderGraph& graph, DescriptorManager* descriptorManager);
    ~ComputeProbePass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "ComputeProbe"; }

    // TD-07: disabled by default. Enable explicitly with --compute-test so the
    // production frame pays neither the dispatches nor the host readback.
    void setEnabled(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }

private:
    struct ProbeResult
    {
        uint32_t magic = 0;
        uint32_t frameNumber = 0;
        uint32_t checksum = 0;
        uint32_t count = 0;
    };

    struct ProbePush
    {
        uint32_t mode;
        uint32_t frameNumber;
        uint32_t count;
        uint32_t pad;
    };

    static constexpr uint32_t kProbeValueCount = 1024;
    static constexpr uint32_t kProbeMagic = 0x5E471234u;

    Device& device;
    RenderGraph& renderGraph;
    DescriptorManager* descriptorManager;

    std::array<RenderGraph::BufferHandle, kFramesInFlight> workBuffers{};
    std::array<RenderGraph::BufferHandle, kFramesInFlight> resultBuffers{};

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> descriptorSets{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> pipeline;

    bool enabled = false;
    bool reportedFirstValidResult = false;
};
