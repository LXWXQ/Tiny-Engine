#pragma once

#include "Editor/EditorPanel.h"

#include <string>

namespace editor
{

// 帮助 -> 命令行选项。
//
// 表来自 Core/CliCatalog（与 TinyEngine.exe --help / --list-options 同一份），
// 所以这里不写任何开关文字 —— 加参数只改 CliCatalog.cpp 一处。
// 面板默认隐藏：它是个查阅用的浮动窗，常驻会白占一块屏幕。
class CliReferencePanel : public EditorPanel
{
public:
    CliReferencePanel();

    const char* name() const override { return "Cli"; }
    void draw(EditorState& state) override;

    // 纯逻辑自检：目录非空、搜索过滤大小写折叠、分组筛选不越界。
    static bool selfTest();

private:
    // 子串匹配，只做 ASCII 大小写折叠（开关名全是 ASCII）。
    static bool matches(const std::string& haystack, const char* needle);

    char search_[96] = {};
    int groupFilter_ = 0;          // 0 = 全部分组，其余对应 groupIndex_ 的下标
    std::string lastSearch_;
};

} // namespace editor
