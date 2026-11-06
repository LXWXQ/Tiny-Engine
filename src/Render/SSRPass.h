#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"

#include <array>
#include <glm/glm.hpp>
#include <memory>

// F6-01：水面近场屏幕空间反射（SSR）。
//
// 为什么是「水面专用」而不是通用 deferred SSR：
//   本引擎唯一有真实镜面反射的表面是水面；玻璃/不透明 PBR 在默认场景里没有
//   可见的镜面反射内容，做通用 deferred SSR 需要额外拷一份 HDR、或多一个 HDR
//   目标并在 Lighting 里形成反馈环，成本远高于收益。远场/屏幕外由 C1 的
//   平面反射覆盖，SSR 只负责平面反射看不到的近场（岸线、桥墩、近处地形）。
//
// 输出是半分辨率的反射颜色 + alpha 置信度；WaterPass 采样后与平面反射/IBL
// 结果混合。alpha=0 表示没有命中，水面回退到原有反射。
class SSRPass : public IRenderPass
{
public:
    SSRPass(Device& device, RenderGraph& graph, RenderResources& resources,
            VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager);
    ~SSRPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "SSR"; }

    // 必须与 shaders/ssr.frag 的 push_constant 布局逐一对应。
    struct PushConstants
    {
        glm::mat4 inverseProjectionView;
        glm::vec4 params;   // x=maxDistance y=thickness z=stepCount w=enabled
        glm::vec4 water;    // x=waterLevel y=1/width z=1/height w=unused
    };

    static constexpr uint32_t kStepCount = 48;

    bool isActive() const { return active; }
    RenderGraph::TextureHandle getColorHandle(uint32_t frame) const { return resources.ssrColor[frame]; }
    VkImageView getColorView(uint32_t frame) const;
    VkSampler getColorSampler() const { return sampler; }

private:
    void createTextures();
    void createDescriptorLayout();
    void createSampler();
    void createPipeline();
    void updateDescriptor(uint32_t frame);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;
    std::array<VkDescriptorSet, kFramesInFlight> sets{};

    bool active = false;
    bool loggedDisabled = false;
    bool loggedFirstDraw = false;
};
