#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/GBuffer.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "GeometrySystem.h"
#include <memory>
#include <array>
#include "Core/FrameInFlight.h"

// 几何通道：把场景写进 G-Buffer。
// D-1(b) 起不再持有 VkRenderPass / VkFramebuffer，改为把工作声明进渲染图。
class GeometryPass : public IRenderPass
{
public:
    GeometryPass(Device& device, const std::array<GBuffer*, kFramesInFlight>& gBuffers,
                 RenderGraph& graph, RenderResources& resources,
                 VkDescriptorSetLayout globalSetLayout,
                 VkDescriptorSetLayout materialSetLayout,
                 VkDescriptorSetLayout terrainSetLayout);
    ~GeometryPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "Geometry"; }

private:
    Device& vulkanDevice;
    std::array<GBuffer*, kFramesInFlight> gBuffers{};
    RenderGraph& renderGraph;
    RenderResources& resources;

    VkDescriptorSetLayout globalSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorSetLayout terrainSetLayout;

    std::unique_ptr<GeometrySystem> geometrySystem;
};