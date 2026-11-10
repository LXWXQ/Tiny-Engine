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

// F6-06：太阳盘 + 镜头光晕。
//
// 这是一个屏幕空间后效，不参与 G-Buffer/光照：
//   * 方向取自 scene.environment 的方向光，toSun = -normalize(lightDirection)；
//     因此画面里的太阳与照亮场景的光永远是同一方向。
//   * 日盘按世界空间射线与 sunDir 的夹角绘制，所以旋转相机时保持圆形，
//     不受屏幕纵横比影响；盘面亮度写入 HDR，交给 Bloom 产生辉光。
//   * 镜头鬼影沿“太阳 -> 屏幕中心”的连线镜像，柔和圆形散景 + 色散，
//     属于镜头伪影，因此不参与逐像素几何遮挡，但会被太阳整体可见度调制；
//     太阳在相机背后时直接不产生，中心移出画面时淡出，避免鬼影凭空落在屏幕中央。
//
// 为什么放在 TAA 之后、Bloom 之前：
//   TAA 的重投影历史会让快速摇镜时的日盘拖出鬼影；放在它之后就没有历史混合。
//   而 Bloom 必须吃到日盘的高光，否则太阳只是一个白点。
//
// 已知边界：体积云不写深度，太阳会叠在云层上；需要正确的云遮挡得让
// CloudPass 输出深度，属于后续工作（见 known-limits）。
class SunFlarePass : public IRenderPass
{
public:
    SunFlarePass(Device& device, RenderGraph& graph, RenderResources& resources,
                 VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager);
    ~SunFlarePass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    // G-Buffer 深度 view 在 resize 时重建，描述符必须跟着换（td03 教训）。
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "SunFlare"; }

    // 必须与 shaders/sunflare.frag 的 push_constant 逐字节对应。
    struct PushConstants
    {
        glm::mat4 inverseProjectionView;
        glm::vec4 sunDirectionRadius; // xyz=指向太阳的单位向量 w=角半径（弧度）
        glm::vec4 params0;            // x=intensity y=glow z=flareIntensity w=occlusion(0/1)
        glm::vec4 params1;            // x=ghostCount y=aspect z=sunUV.x w=sunUV.y
        glm::vec4 color;              // rgb=色调 a=太阳是否在相机前方(0/1)
    };

    bool isActive() const { return active; }

private:
    void createDescriptorLayout();
    void createSampler();
    void createPipeline();
    void updateDescriptor(uint32_t frame);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;

    VkSampler depthSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;
    std::array<VkDescriptorSet, kFramesInFlight> sets{};

    bool active = false;
    bool loggedDisabled = false;
    bool loggedFirstDraw = false;
};
