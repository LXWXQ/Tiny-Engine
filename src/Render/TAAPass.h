#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

// TAA（时域抗锯齿）。
//
// 每帧把当前 HDR 颜色与上一帧历史按重投影结果混合：
//   - 相机投影加 Halton(2,3) 亚像素抖动，让边缘在时域上采样到不同位置；
//   - 用 G-Buffer 世界位置 + 上一帧未抖动 VP 重投影到历史 UV；
//   - 邻域 3x3 颜色 AABB 钳制历史，世界半径不一致时直接拒斥。
//
// 输出有两份：taaOutput[frame] 供 Bloom/Present 静态采样，taaHistory[frameNumber&1]
// 作为下一帧的输入。历史图必须是 persistent（跨帧保留布局/内容）；输出图是
// 瞬时资源，但即使 TAA 关闭也要照常写，否则下游会读到没人写的图。
class TAAPass : public IRenderPass
{
public:
    TAAPass(Device& device, RenderGraph& graph, RenderResources& resources,
            VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager);
    ~TAAPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "TAA"; }

    // 在写 UBO 之前调用。返回本帧的抖动像素偏移（供 Application 抖相机投影）。
    // previousJitter 会在这里翻到上一帧，因此 addToGraph 能拿到正确的历史偏移。
    glm::vec2 prepareFrame(uint64_t frameNumber, VkExtent2D extent,
                           bool enabled, float jitterScale);

private:
    struct Push
    {
        glm::vec2 previousJitterUv;
        glm::vec2 texelSize;
        float historyWeight;
        float historyValid;
        float pad0;
        float pad1;
    };
    static_assert(sizeof(Push) == 32, "TAA push constant must stay 32 bytes");

    void createDescriptorLayout();
    void createSampler();
    void createPipeline();
    void updateDescriptors();
    static float halton(uint32_t index, uint32_t base);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager;

    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    // [frame * 2 + historyReadIndex]
    std::array<VkDescriptorSet, kFramesInFlight * 2> sets{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;

    glm::vec2 currentJitterPixels{ 0.0f };
    glm::vec2 previousJitterPixels{ 0.0f };
    bool historyValid = false;
    bool resetHistory = false;
    bool lastEnabled = true;
    float lastJitterScale = 1.0f;
};
