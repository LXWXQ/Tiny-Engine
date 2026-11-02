#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "RHI/Swapchain.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include "Core/ImGuiSystem.h"
#include <memory>
#include <array>
#include "Core/FrameInFlight.h"

// Final post-processing pass:
// HDR scene + Bloom -> camera motion blur -> exposure -> ACES -> gamma ->
// swapchain. ImGui is drawn after the scene so UI never gets blurred.
class PresentPass : public IRenderPass
{
public:
    PresentPass(Device& device, Swapchain& swapchain, RenderGraph& graph,
                RenderResources& resources, DescriptorManager* descMgr,
                VkDescriptorSetLayout globalSetLayout);
    ~PresentPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "Present"; }

    void setImGuiSystem(ImGuiSystem* imgui) { imguiSystem = imgui; }

    // TD-09：输出目标由 RenderResources::editorViewportEnabled 决定，
    // 不再单独存一个 handle —— 视口纹理是**按 frame slot 各一张**的，
    // 存单个 handle 必然会在两帧在飞时写错槽位（渲染图的语义校验会当场报出来）。
    // 离屏模式下 ImGui 由 UiCompositePass 绘制，这里必须关掉，
    // 否则同一帧会有两份 ImGui 绘制列表提交到不同的渲染目标上。
    void setRenderImGui(bool value) { renderImGui = value; }

private:
    struct PresentPush
    {
        float exposure;
        float bloomIntensity;
        float bloomEnabled;
        float motionBlurEnabled;
        float motionBlurStrength;
        float motionBlurMaxRadius;
        float motionBlurFrame;
        float pad;
    };
    static_assert(sizeof(PresentPush) == 32,
        "PresentPush must match present.frag/present_no_bloom.frag");

    void createDescriptorLayout();
    void createPipeline();
    void updateBloomDescriptor(uint32_t frame);
    void updateNoBloomDescriptor(uint32_t frame);

    Device& device;
    Swapchain& swapchain;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager;

    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler positionSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout noBloomSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> sets{};
    std::array<VkDescriptorSet, kFramesInFlight> noBloomSets{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout noBloomPipelineLayout = VK_NULL_HANDLE;
    VkFormat pipelineSwapchainFormat = VK_FORMAT_UNDEFINED;
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<Pipeline> noBloomPipeline;

    ImGuiSystem* imguiSystem = nullptr;

    // 编辑器模式下 ImGui 由 UiCompositePass 绘制。
    bool renderImGui = true;
};
