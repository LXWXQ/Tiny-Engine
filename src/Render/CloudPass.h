#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "RHI/Texture3D.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"

#include <array>
#include <glm/glm.hpp>
#include <memory>

// F6-05：体积云。
//
// 全屏光线步进一层位于 [baseHeight, baseHeight + thickness] 的云层。每条射线
// 先读 G-Buffer 的世界坐标，把步进区间截到最近的不透明表面之前，因此云会被
// 地形/建筑正确遮挡，也不会在近处物体后面被“补画”出一层雾。
//
// 输出是预乘 alpha 的云颜色，直接混合到已经完成光照的 HDR 图上：
//   outColor = cloudColor * alpha, outAlpha = 1 - transmittance
// 混合方程是 src * 1 + dst * (1 - alpha)；alpha 通道的 src 因子为 0、dst 因子为 1，
// 因此 HDR 原有的 alpha 保持不变。
//
// 为什么放在 Lighting 之后、Reflection/SSR/Glass/Water 之前：
//   云属于“大气/远景”，要参与后续所有透明通道（水/玻璃/SSR）读取的 HDR；
//   而它自身不需要进入 G-Buffer，也不能被 SSAO/阴影当作几何。
//   平面反射有独立的第二遍 Lighting，不包含云 —— 这是已知边界（见 known-limits）。
class CloudPass : public IRenderPass
{
public:
    CloudPass(Device& device, RenderGraph& graph, RenderResources& resources,
              VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
              VkCommandPool commandPool);
    ~CloudPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    // G-Buffer / HDR 的 image view 在 resize 时会重建，描述符必须跟着换，否则
    // 下一帧仍采样已销毁的旧 view（td03 的 swapchain/render-scale 路径会崩溃）。
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "Cloud"; }

    // 必须与 shaders/cloud.frag 的 push_constant 逐字节对应。
    struct PushConstants
    {
        glm::mat4 inverseProjectionView;
        glm::vec4 params0;   // x=coverage y=density z=baseHeight w=thickness
        glm::vec4 params1;   // x=windSpeed y=absorption z=steps w=lightSteps
        glm::vec4 params2;   // x=time y=maxDistance z=ambient w=enabled
        glm::vec4 color;     // rgb=云/环境散射色调 a=unused
    };

    bool isActive() const { return active; }

private:
    void createNoiseTexture();
    void createDescriptorLayout();
    void createSampler();
    void createPipeline();
    void updateDescriptor(uint32_t frame);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    std::unique_ptr<Texture3D> noiseTexture;
    VkSampler positionSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;
    std::array<VkDescriptorSet, kFramesInFlight> sets{};

    float timeSeconds = 0.0f;
    bool active = false;
    bool loggedDisabled = false;
    bool loggedFirstDraw = false;
};
