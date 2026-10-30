#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/FrameInFlight.h"

#include <array>
#include <memory>

// ===========================================================================
// 编辑器线框叠加层：把选中实体的世界 AABB 画成 12 条线，**带深度测试**。
//
// 为什么不用 ImGui 画（之前就是这么做的）：
//   ImGui 的 draw list 是纯 2D 叠加，没有深度概念 —— 包围盒会穿透墙体
//   永远画在最前面，物体被挡住时框还看得见，完全误导人。
//
// 做法：把它当真正的 3D 线段画，深度附件直接复用 G-Buffer 的深度图
//（LOAD_OP_LOAD + 深度写关闭，只测不写）。
//
// 顺序要求：必须在 PresentPass 之后（那时 editorViewport 已经有色调映射结果）、
// UiCompositePass 之前（ImGui 采样 editorViewport 时框已经画进去了）。
// ===========================================================================
class EditorOverlayPass : public IRenderPass
{
public:
    EditorOverlayPass(Device& device, RenderGraph& graph, RenderResources& resources);
    ~EditorOverlayPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D /*extent*/) override {}
    std::string getName() const override { return "EditorOverlay"; }

private:
    struct Push
    {
        glm::mat4 viewProjection{ 1.0f };
        glm::vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    };

    void createPipeline(VkFormat colorFormat, VkFormat depthFormat);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;

    std::unique_ptr<Pipeline> pipeline;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    // 每帧槽位一条 24 顶点的小缓冲（12 条边 × 2 端点）。
    // 只有 288 字节，每帧重写，不需要更精细的同步策略。
    std::array<VkBuffer, kFramesInFlight> vertexBuffers{};
    std::array<VmaAllocation, kFramesInFlight> vertexAllocations{};
    std::array<void*, kFramesInFlight> vertexMapped{};
};
