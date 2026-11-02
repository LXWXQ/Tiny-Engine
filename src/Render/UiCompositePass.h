#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/FrameInFlight.h"

class ImGuiSystem;

// ===========================================================================
// 编辑器模式的最后一步：把 ImGui 画到交换链上。
//
// 为什么需要单独一个 Pass：
//   运行时模式里 PresentPass 既做 tonemap 又叠 ImGui，一步到位。
//   但编辑器要的是"3D 画面是 ImGui 里的一张图"，于是同一条链上出现顺序依赖：
//       PresentPass  写离屏视口纹理
//       ImGui        采样这张纹理（作为 ImGui::Image）
//   同一张图不能在一个 Pass 里既当渲染目标又被采样（布局冲突），
//   所以必须拆成两个 Pass。
//
// 这个 Pass 只做两件事：清交换链、提交 ImGui 绘制列表。
// 它不读任何场景资源 —— 3D 内容已经在视口纹理里，由 ImGui 负责贴上去。
// ===========================================================================
class UiCompositePass : public IRenderPass
{
public:
    UiCompositePass(Device& device, RenderGraph& graph, RenderResources& resources);

    void init() override {}
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D /*extent*/) override {}
    std::string getName() const override { return "UiComposite"; }

    void setImGuiSystem(ImGuiSystem* imgui) { imguiSystem = imgui; }

private:
    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    ImGuiSystem* imguiSystem = nullptr;
};
