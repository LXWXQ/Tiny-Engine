#include "Editor/panels/RenderSettingsPanel.h"
#include "Editor/EditorState.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <cstdio>

namespace editor
{

int RenderSettingsPanel::clampKernelSize(int value)
{
    if (value < 4)
        value = 4;
    if (value > 64)
        value = 64;
    return (value % 2 == 0) ? value : value + 1;
}

bool RenderSettingsPanel::selfTest()
{
    if (clampKernelSize(3) != 4 || clampKernelSize(65) != 64 ||
        clampKernelSize(16) != 16 || clampKernelSize(17) != 18)
    {
        std::fprintf(stderr, "[Editor] render settings selfTest FAIL: kernel clamp\n");
        return false;
    }
    std::printf("[Editor] render settings selfTest PASS | kernel clamped to 4..64 even\n");
    return true;
}

void RenderSettingsPanel::draw(EditorState& state)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 浮动工具窗：默认贴右栏左侧，第一次出现时给个大小，之后由用户拖动。
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 760.0f,
                                   viewport->WorkPos.y + 80.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 320.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Render", &visible))
    {
        ImGui::End();
        return;
    }

    EngineSettings* settings = state.services.settings;
    if (settings == nullptr)
    {
        ImGui::TextDisabled("引擎设置不可用（编辑器没有拿到 EngineSettings）");
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "曝光、阴影、Bloom、SSAO、雾、水面、TAA、运动模糊、SSR、方向光已改为"
        "场景级组件。在 Inspector 没有选中对象时（或点下面的按钮）可以添加、"
        "启用、调参、移除：移除组件即关闭该功能，并随场景文档保存与撤销。");

    ImGui::Spacing();
    if (ImGui::Button("在 Inspector 中编辑场景组件"))
    {
        state.selection.clear();
        state.notify(LogEntry::Level::Info,
                     "[Render] 场景组件已显示在 Inspector（未选中任何对象）");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("清空选择后，Inspector 显示当前场景的功能组件");

    // 只统计，不在这里改：所有场景功能都走组件 UI，避免两套控件写同一份值。
    if (Scene* scene = state.services.scene)
    {
        entt::registry* registry = state.services.registry;
        if (registry != nullptr && registry->valid(scene->getSceneSettingsEntity()))
        {
            int mounted = 0;
            for (const scene::ComponentDescriptor& descriptor :
                 scene->getComponentRegistry().descriptors())
            {
                if (descriptor.scope != scene::ComponentScope::Scene)
                    continue;
                if (descriptor.has(*registry, scene->getSceneSettingsEntity()))
                    ++mounted;
            }
            ImGui::TextDisabled("场景组件：%d 个已挂载（启用开关在 Inspector 中编辑）",
                                mounted);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("下面只保留引擎级默认值：写入/读取 config/engine.json。");
    ImGui::TextDisabled("场景组件是场景文档的一部分；engine.json 只决定新场景和旧场景的初始值。");

    ImGui::Spacing();
    ImGui::BeginDisabled(!state.services.saveConfig);
    if (ImGui::Button("Save to engine.json"))
    {
        state.services.saveConfig();
        state.pushLog(LogEntry::Level::Info,
                      "[Render] settings saved to config/engine.json");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("把当前生效值写成引擎默认；不会改当前场景的组件列表");

    ImGui::SameLine();
    ImGui::BeginDisabled(!state.services.reloadConfig);
    if (ImGui::Button("Reload engine.json"))
    {
        state.services.reloadConfig();
        state.pushLog(LogEntry::Level::Info,
                      "[Render] engine.json reloaded; scene components rebuilt");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("重新读取 engine.json，并用它重建当前场景的功能组件");

    ImGui::Separator();
    ImGui::PushItemWidth(-88.0f);
    drawAdvanced(*settings);
    ImGui::PopItemWidth();

    ImGui::End();
}

void RenderSettingsPanel::drawAdvanced(EngineSettings& settings)
{
    if (!ImGui::CollapsingHeader("提交路径（调试）"))
        return;

    ImGui::TextDisabled("关掉用来对比 CPU 路径与 GPU 批的差异");
    ImGui::Checkbox("GPU scene batch", &settings.gpuSceneEnabled);
    ImGui::Checkbox("Merged vertex buffers", &settings.gpuMergedBuffers);
    ImGui::Checkbox("Hi-Z occlusion", &settings.hizEnabled);
    ImGui::Checkbox("Terrain batch", &settings.terrainBatchEnabled);

    int minInstances = static_cast<int>(settings.gpuSceneMinInstances);
    if (ImGui::SliderInt("GPU min instances", &minInstances, 1, 2048))
        settings.gpuSceneMinInstances = static_cast<uint32_t>(minInstances);

    ImGui::SliderFloat("Min screen px", &settings.gpuSceneMinScreenPixels, 0.0f, 64.0f, "%.1f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("屏幕空间误差剔除：投影半径小于该像素数的组直接跳过（0 = 关）");
}

} // namespace editor
