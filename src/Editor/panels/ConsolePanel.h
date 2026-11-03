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

    // B6：过滤判据的纯逻辑自检（级别开关 + 关键字大小写折叠）。
    static bool selfTest();

private:
    bool showInfo = true;
    bool showWarning = true;
    bool showError = true;
    bool autoScroll = true;
    // B6 / UX-11 补充：按关键字过滤（大小写不敏感子串），与「复制」共用同一判据。
    char searchBuffer[64] = {};
};

} // namespace editor
