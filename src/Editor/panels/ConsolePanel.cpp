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
        state.log.clear();

    ImGui::Separator();

    ImGui::BeginChild("console_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    for (const LogEntry& entry : state.log)
    {
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

        ImGui::TextColored(color, "%s", entry.text.c_str());
    }

    if (autoScroll && state.scrollLogToBottom)
    {
        ImGui::SetScrollHereY(1.0f);
        state.scrollLogToBottom = false;
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace editor
