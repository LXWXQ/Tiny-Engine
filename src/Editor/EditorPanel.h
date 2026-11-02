#pragma once

#include <string>

namespace editor
{

struct EditorState;

// ===========================================================================
// 面板基类
//
// 加一个新面板的步骤：
//   1. 继承 EditorPanel，实现 draw()
//   2. 在 EditorLayer::createDefaultPanels() 里 registerPanel<YourPanel>("唯一名字")
//   3. 在 EditorLayer::drawPanels() 里按名字调用，或在 View 菜单里加开关
//
// 约定：
//   * 面板**不得**自己创建 GPU 资源。需要渲染就通过 EngineServices 请求引擎做。
//   * 面板**不得**直接改 registry，必须通过 EditorState::commands 提交命令，
//     否则改动不可撤销（见 CommandStack.h 的说明）。
//   * ImGui 可见文字必须是 ASCII —— 内置字体没有中文字形。
// ===========================================================================

class EditorPanel
{
public:
    virtual ~EditorPanel() = default;

    virtual const char* name() const = 0;

    // 每帧调用。实现里必须自己调用 ImGui::Begin/End。
    virtual void draw(EditorState& state) = 0;

    // 面板初始化（在第一批 draw 之前调用一次）。默认空实现。
    virtual void onAttach(EditorState& /*state*/) {}

    bool visible = true;
};

} // namespace editor
