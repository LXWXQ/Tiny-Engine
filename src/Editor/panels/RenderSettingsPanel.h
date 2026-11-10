#pragma once

#include "Editor/EditorPanel.h"
#include "Core/FrameInfo.h"

namespace editor
{

// 渲染设置面板：场景渲染功能已经全部迁移到“场景组件”
// （Inspector 没有选中对象时显示，或从这里一键跳过去）。
//
// 这个面板现在只负责两件事：
//   1. EngineSettings 的引擎级默认值（config/engine.json 的保存 / 重载）；
//   2. 不属于场景内容的提交路径调试开关（GPU batch / Hi-Z / terrain batch）。
//
// 场景里实际启用哪些功能、每个功能怎么调参，全部由 SceneComponents 决定；
// 不要在别处再加一套曝光/阴影/Bloom 控件，否则两套 UI 会写同一份值。
class RenderSettingsPanel : public EditorPanel
{
public:
    const char* name() const override { return "Render"; }
    void draw(EditorState& state) override;

    // SSAO 核大小必须是 4..64 的偶数：场景 SSAO 组件共用这条规则，
    // 这里保留纯函数回归，避免夹取逻辑被改坏。
    static int clampKernelSize(int value);

    static bool selfTest();

private:
    void drawAdvanced(EngineSettings& settings);
};

} // namespace editor
