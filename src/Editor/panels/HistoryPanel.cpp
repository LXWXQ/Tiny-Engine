#include "Editor/panels/HistoryPanel.h"

#include "Editor/EditorState.h"

#include <imgui.h>

namespace editor
{

namespace
{

// 一行 = 一个可跳转的历史状态。depth 是"撤销到这个深度"的意思：
//   depth == undoCount  -> 当前状态
//   depth <  undoCount  -> 需要连续撤销几条
//   depth >  undoCount  -> 需要连续重做几条
bool drawRow(size_t depth, const char* label, bool isCurrent, bool highlighted, bool enabled)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (highlighted)
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "%zu", depth);
    else
        ImGui::TextDisabled("%zu", depth);

    ImGui::TableNextColumn();
    if (isCurrent)
    {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "%s  <- 当前", label);
        return false;
    }

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns);
    ImGui::EndDisabled();
    return clicked;
}

} // namespace

void HistoryPanel::draw(EditorState& state)
{
    // 面板必须自己开关窗口（见 EditorPanel.h 的约定）。
    if (!ImGui::Begin("History", &visible))
    {
        ImGui::End();
        return;
    }

    const auto& undoEntries = state.commands.undoEntries();
    const auto& redoEntries = state.commands.redoEntries();
    const size_t undoCount = undoEntries.size();
    const size_t redoCount = redoEntries.size();

    ImGui::TextDisabled("已执行 %zu / 可重做 %zu", undoCount, redoCount);

    const bool writable = state.documentWritable();
    if (!writable)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "播放中：先按 F7 停止再跳转历史");

    size_t requestedDepth = undoCount;
    bool requested = false;

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("history", 2, flags))
    {
        ImGui::TableSetupColumn("深度", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("命令", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // 当前状态占一行，越靠下的行越"旧"。
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "%zu", undoCount);
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "<当前状态>");

        // 已执行：从最新一条往下画（和 Ctrl+Z 的直觉一致）。
        for (size_t i = undoCount; i > 0; --i)
        {
            const Command& command = *undoEntries[i - 1];
            if (drawRow(i - 1, command.name(), false, false, writable))
            {
                requestedDepth = i - 1;
                requested = true;
            }
        }

        // redo 栈是 LIFO：back() 是下一条会被重做的命令，所以倒着遍历。
        for (size_t i = 0; i < redoCount; ++i)
        {
            const Command& command = *redoEntries[redoCount - 1 - i];
            if (drawRow(undoCount + i + 1, command.name(), false, false, writable))
            {
                requestedDepth = undoCount + i + 1;
                requested = true;
            }
        }

        ImGui::EndTable();
    }

    if (undoCount == 0 && redoCount == 0)
        ImGui::TextDisabled("还没有可撤销的操作。");

    if (requested && writable)
    {
        state.commands.jumpTo(requestedDepth);
        state.markDirty();
        state.transformRefreshPending = true;
    }

    ImGui::End();
}

} // namespace editor
