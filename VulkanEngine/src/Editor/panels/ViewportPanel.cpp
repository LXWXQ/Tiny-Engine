#include "Editor/panels/ViewportPanel.h"
#include "Editor/EditorState.h"

#include <imgui.h>

namespace editor
{

void ViewportPanel::draw(EditorState& state)
{
    // 视口窗口不要标题栏、不要滚动条、内边距为 0：
    // 任何一点内边距都会让图片和面板边缘之间露出底色，看起来像"画面没铺满"。
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (!ImGui::Begin("Viewport", nullptr, flags))
    {
        ImGui::PopStyleVar(2);
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar(2);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t width = (uint32_t)(avail.x > 1.0f ? avail.x : 1.0f);
    const uint32_t height = (uint32_t)(avail.y > 1.0f ? avail.y : 1.0f);

    // 尺寸变了就请求引擎重建离屏纹理。
    // 这里只"报数"，不自己建 —— 重建需要 vkDeviceWaitIdle，属于引擎职责，
    // 而且必须在帧首做（现在命令缓冲已在录制）。
    if (width != state.requestedViewportWidth || height != state.requestedViewportHeight)
    {
        state.requestedViewportWidth = width;
        state.requestedViewportHeight = height;
        if (state.services.requestViewportSize)
            state.services.requestViewportSize(width, height);
    }

    const ImTextureID textureId = state.services.getViewportTextureId
        ? state.services.getViewportTextureId(state.frameSlot)
        : (ImTextureID)0;

    if (textureId == (ImTextureID)0)
    {
        ImGui::TextDisabled("Viewport texture not ready.");
    }
    else
    {
        // 用 uv 翻转：Vulkan 的纹理原点在左上，ImGui 的 uv (0,0) 也在左上，
        // 但引擎渲染时 y 轴方向与 ImGui 一致，所以这里不需要翻转。
        ImGui::Image(textureId, avail, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    }

    // 左上角叠一行小字，提示这是视口而不是静态图片（也方便截图核对）。
    ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.55f), "%ux%u", width, height);

    // 让相机控制知道"鼠标在视口里"，避免在面板上拖拽时把相机也转了。
    state.viewportHovered = ImGui::IsWindowHovered();

    ImGui::End();
}

} // namespace editor
