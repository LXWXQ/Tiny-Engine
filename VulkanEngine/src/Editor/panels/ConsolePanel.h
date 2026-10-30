#pragma once

#include "Editor/EditorPanel.h"

namespace editor
{

// 控制台：显示引擎日志。支持按级别过滤与清空。
class ConsolePanel : public EditorPanel
{
public:
    const char* name() const override { return "Console"; }
    void draw(EditorState& state) override;

private:
    bool showInfo = true;
    bool showWarning = true;
    bool showError = true;
    bool autoScroll = true;
};

} // namespace editor
