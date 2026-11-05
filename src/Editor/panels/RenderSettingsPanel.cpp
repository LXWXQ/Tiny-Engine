#include "Editor/panels/RenderSettingsPanel.h"
#include "Editor/EditorState.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <cmath>
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
    // 浮动工具窗：默认贴右栏左侧，第一次出现时给个大小，之后由用户拖动
    // （位置记在 imgui.ini，不参与 drawPanels() 的固定布局 —— 它没有
    // 参与"工作区划分"的必要，压着视口也看得见改动的效果）。
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 760.0f,
                                   viewport->WorkPos.y + 80.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 620.0f), ImGuiCond_FirstUseEver);

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

    if (!baselineCaptured)
    {
        baseline = *settings;
        baselineCaptured = true;
    }

    ImGui::TextDisabled("改动立即生效；要留到下次启动按 Save to engine.json");

    // 两个按钮的语义要分清楚：Revert 回"本次启动时"，不是回代码默认值。
    if (ImGui::Button("Revert"))
    {
        *settings = baseline;
        state.pushLog(LogEntry::Level::Info, "[Render] settings reverted to session baseline");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("回到本次启动时的值（不是代码里的默认值）");

    ImGui::SameLine();
    ImGui::BeginDisabled(!state.services.saveConfig);
    if (ImGui::Button("Save to engine.json"))
    {
        state.services.saveConfig();
        state.pushLog(LogEntry::Level::Info, "[Render] settings saved to config/engine.json");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("写进 config/engine.json，下次启动继续用这套值");


    ImGui::Separator();

    // 统一宽度：标签在右、滑杆占满其余空间。不设的话 ImGui 用默认 ItemWidth，
    // 一串滑杆会挤在左边一条，数值和标签各自为政，很难扫读。
    ImGui::PushItemWidth(-88.0f);

    if (ImGui::CollapsingHeader("曝光 / 分辨率", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Exposure", &settings->exposure, 0.1f, 6.0f, "%.2f");
        ImGui::SetItemTooltip("整体亮度。Bloom 在曝光之后，所以调这里不会让辉光跟着跳");
        ImGui::SliderFloat("Render Scale", &settings->renderScale, 0.5f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("动态分辨率：0.5 = 一半像素渲染再上采样，掉帧时先降这个");
    }

    if (ImGui::CollapsingHeader("Bloom 辉光", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enabled##bloom", &settings->bloomEnabled);
        ImGui::SliderFloat("Threshold", &settings->bloomThreshold, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Knee", &settings->bloomKnee, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Intensity", &settings->bloomIntensity, 0.0f, 2.0f, "%.3f");
    }

    if (ImGui::CollapsingHeader("SSAO 环境遮蔽", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enabled##ssao", &settings->ssaoEnabled);
        ImGui::SliderFloat("Radius", &settings->ssaoRadius, 0.05f, 2.0f, "%.2f");
        ImGui::SliderFloat("Bias", &settings->ssaoBias, 0.0f, 0.2f, "%.3f");
        int kernel = settings->ssaoKernelSize;
        if (ImGui::SliderInt("Kernel", &kernel, 4, 64))
        {
            settings->ssaoKernelSize = clampKernelSize(kernel);
        }
    }

    if (ImGui::CollapsingHeader("运动模糊"))
    {
        ImGui::Checkbox("Enabled##motionblur", &settings->motionBlurEnabled);
        ImGui::SliderFloat("Strength", &settings->motionBlurStrength, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Max Radius", &settings->motionBlurMaxRadius, 0.0f, 0.2f, "%.3f");
    }

    if (ImGui::CollapsingHeader("TAA 时域抗锯齿", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enabled##taa", &settings->taaEnabled);
        ImGui::SetItemTooltip("亚像素抖动 + 历史帧混合；远处几何边缘和树叶闪烁主要靠它");
        ImGui::SliderFloat("History Blend", &settings->taaBlend, 0.0f, 0.98f, "%.2f");
        ImGui::SliderFloat("Jitter Scale", &settings->taaJitterScale, 0.0f, 1.5f, "%.2f");
        ImGui::TextDisabled("Halton(2,3) | 开关或缩放窗口会重置历史");
    }

    // F6：阴影（3 级 CSM）。strength 是"阴影方向的衰减比例"，
    // 0 = 完全无阴影，1 = 全黑；默认 0.65 是有意留一点环境光，纯黑看着像贴纸。
    if (ImGui::CollapsingHeader("阴影", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enabled##shadows", &settings->shadowEnabled);
        ImGui::SliderFloat("Distance", &settings->shadowDistance, 20.0f, 200.0f, "%.0f");
        ImGui::SliderFloat("Strength", &settings->shadowStrength, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("三级联 | 2048x2048 | 方向光越斜阴影越长");
    }

    drawLight(*settings);
    drawAdvanced(*settings);

    ImGui::End();
}

void RenderSettingsPanel::drawLight(EngineSettings& settings)
{
    if (!ImGui::CollapsingHeader("方向光", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // EngineSettings 里方向光是 vec4（w=0 表示方向），颜色是 vec4（w=1）。
    // 编辑时用 3 分量视图，回写时保住 w，避免把方向误当点。
    float dir[3] = { settings.directionalLightDir.x, settings.directionalLightDir.y,
                     settings.directionalLightDir.z };
    if (ImGui::SliderFloat3("Direction", dir, -1.0f, 1.0f))
    {
        glm::vec3 v(dir[0], dir[1], dir[2]);
        const float length = glm::length(v);
        if (length > 1e-4f)
            v /= length;
        settings.directionalLightDir = glm::vec4(v.x, v.y, v.z, 0.0f);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("写回前会归一化；全零会被忽略（保持上一次的方向）");

    float color[3] = { settings.directionalLightColor.x, settings.directionalLightColor.y,
                       settings.directionalLightColor.z };
    if (ImGui::ColorEdit3("Color", color))
    {
        settings.directionalLightColor = glm::vec4(color[0], color[1], color[2],
                                                   settings.directionalLightColor.w);
    }
    ImGui::SliderFloat("Intensity", &settings.directionalLightColor.w, 0.0f, 4.0f, "%.2f");
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
