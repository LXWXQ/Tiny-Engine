#include "Editor/panels/ConsolePanel.h"
#include "Editor/EditorState.h"

#include <imgui.h>

namespace editor
{

void ConsolePanel::draw(EditorState& state)
{
    if (!ImGui::Begin("Console", &visible))
    {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &showWarning);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &showError);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        state.log.clear();
        state.markErrorsRead();
        state.lastErrorLogIndex = 0;
        state.lastErrorSummary.clear();
    }

    ImGui::Separator();

    ImGui::BeginChild("console_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // B3：循环带下标，才能把「最近一次错误」那一行高亮出来 ——
    // 用户从状态栏/Toast 点进来时，要能一眼看到是哪一行。
    for (size_t index = 0; index < state.log.size(); ++index)
    {
        const LogEntry& entry = state.log[index];
        bool visibleEntry = false;
        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);

        switch (entry.level)
        {
        case LogEntry::Level::Info:
            visibleEntry = showInfo;
            break;
        case LogEntry::Level::Warning:
            visibleEntry = showWarning;
            color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            break;
        case LogEntry::Level::Error:
            visibleEntry = showError;
            color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            break;
        }

        if (!visibleEntry)
            continue;

        if (state.focusConsoleRequest && index == state.lastErrorLogIndex)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.75f, 0.2f, 0.2f, 1.0f));
            ImGui::Selectable(entry.text.c_str(), true);
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::TextColored(color, "%s", entry.text.c_str());
        }
    }

    if (autoScroll && state.scrollLogToBottom)
    {
        ImGui::SetScrollHereY(1.0f);
        state.scrollLogToBottom = false;
    }

    // 请求来自状态栏 / Toast / 工具栏徽标：滚到最近一次错误那一行并保持高亮，
    // 直到用户下一次主动操作（Clear 或新的错误）为止。
    if (state.focusConsoleRequest)
    {
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float target = static_cast<float>(state.lastErrorLogIndex) * lineHeight;
        const float maxScroll = ImGui::GetScrollMaxY();
        ImGui::SetScrollY(target < maxScroll ? target : maxScroll);
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace editor
