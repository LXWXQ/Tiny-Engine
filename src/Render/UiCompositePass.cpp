#include "UiCompositePass.h"
#include "Core/ImGuiSystem.h"

UiCompositePass::UiCompositePass(Device& device, RenderGraph& graph, RenderResources& resources)
    : device(device), renderGraph(graph), resources(resources)
{
}

void UiCompositePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    // 非编辑器模式：什么都不注册。
    // 这一步必须放在 addGraphicsPass 之前 —— Pass 一旦创建就会进渲染图的执行序列，
    // 那时它和 PresentPass 会同时声明写交换链，渲染图会直接判定冲突。
    if (!resources.editorViewportEnabled)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    auto& pass = graph.addGraphicsPass("UiComposite");

    // 声明读视口纹理：渲染图会把它从 COLOR_ATTACHMENT_OPTIMAL 转到
    // SHADER_READ_ONLY_OPTIMAL。ImGui 的 Vulkan 后端在注册纹理时就把
    // 布局写进了描述符，所以这里的布局必须与注册时传的一致。
    pass.read(resources.editorViewport[frame]);

    // 清成不透明黑：视口纹理只覆盖中央区域，四周留白应该是编辑器底色而不是花屏。
    VkClearValue clear{};
    clear.color = { { 0.06f, 0.06f, 0.07f, 1.0f } };
    pass.writeColor(resources.swapchainImages[frameInfo.swapchainImageIndex],
        VK_ATTACHMENT_LOAD_OP_CLEAR, clear);

    pass.execute([this](VkCommandBuffer cmd)
    {
        // 3D 画面不是在这里画的 —— 它是 ImGui 绘制列表里的一张图片。
        // 这里只提交整个 ImGui 绘制列表（含视口图片与所有面板）。
        if (imguiSystem != nullptr)
            imguiSystem->renderDrawData(cmd);
    });
}
