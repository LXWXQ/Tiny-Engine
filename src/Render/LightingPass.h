#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/GBuffer.h"
#include "RHI/Swapchain.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Render/LightingSystem.h"
#include <memory>

// 光照合成通道：全屏三角形，采样 G-Buffer + SSAO + IBL，输出到线性 HDR 中间目标。
//
// D-1(b)：不再持有 RenderPass / Framebuffer；交换链图像被导入渲染图。
// D-2   ：输出从交换链改成 R16G16B16A16_SFLOAT 的 HDR 图，
//         tonemap / gamma / ImGui 全部移交 PresentPass。
//         这里同时承担「导入交换链图像」的职责（在其他 Pass 之前 init）。
class LightingPass : public IRenderPass
{
public:
    LightingPass(Device& device, GBuffer& gBuffer, Swapchain& swapchain,
                 RenderGraph& graph, RenderResources& resources,
                 VkDescriptorSetLayout globalLayout);
    ~LightingPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "Lighting"; }

private:
    Device& vulkanDevice;
    GBuffer& gBuffer;
    Swapchain& vulkanSwapchain;
    RenderGraph& renderGraph;
    RenderResources& resources;
    VkDescriptorSetLayout globalSetLayout;

    std::unique_ptr<LightingSystem> lightingSystem;
};