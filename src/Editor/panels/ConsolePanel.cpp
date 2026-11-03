#include "Editor/panels/ConsolePanel.h"
#include "Editor/EditorState.h"

#include <imgui.h>

#include <string>

namespace editor
{

namespace
{

// B5：过滤状态集中在一个地方 —— 逐行显示和"复制"用的是同一套判据，
// 否则复制出来的内容和屏幕上看到的不一致。
bool levelVisible(const LogEntry::Level level, bool showInfo, bool showWarning, bool showError)
{
    switch (level)
    {
    case LogEntry::Level::Info:
        return showInfo;
    case LogEntry::Level::Warning:
        return showWarning;
    case LogEntry::Level::Error:
        return showError;
    }
    return true;
}

} // namespace

void ConsolePanel::draw(EditorState& state)
{
    if (!ImGui::Begin("Console", &visible))
    {
        ImGui::End();
        return;
    }

    size_t infoCount = 0;
    size_t warningCount = 0;
    size_t errorCount = 0;
    for (const LogEntry& entry : state.log)
    {
        switch (entry.level)
        {
        case LogEntry::Level::Info:    ++infoCount; break;
        case LogEntry::Level::Warning: ++warningCount; break;
        case LogEntry::Level::Error:   ++errorCount; break;
        }
    }

    // UX-11：数量直接贴在开关后面，用户不用数行就知道"是不是只有一条错误"。
    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", infoCount);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Checkbox("Warn", &showWarning);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", warningCount);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Checkbox("Error", &showError);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", errorCount);

    ImGui::SameLine(0.0f, 16.0f);
    if (ImGui::Button("仅错误"))
    {
        showInfo = false;
        showWarning = false;
        showError = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("只看错误：排查时最常用的视图");
    ImGui::SameLine();
    if (ImGui::Button("全部"))
    {
        showInfo = true;
        showWarning = true;
        showError = true;
    }

    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &autoScroll);

    ImGui::SameLine();
    if (ImGui::Button("复制"))
    {
        // 复制的是"屏幕上能看到的那些行"，带时间戳 —— 报 bug 时直接粘。
        std::string text;
        for (const LogEntry& entry : state.log)
        {
            if (!levelVisible(entry.level, showInfo, showWarning, showError))
                continue;
            text += entry.time;
            text += ' ';
            text += entry.text;
            text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
        state.notifySuccess("控制台内容已复制到剪贴板");
    }

    ImGui::SameLine();
    if (ImGui::Button("清空"))
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
        if (!levelVisible(entry.level, showInfo, showWarning, showError))
            continue;

        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        if (entry.level == LogEntry::Level::Warning)
            color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        else if (entry.level == LogEntry::Level::Error)
            color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);

        // UX-11：时间列固定宽度，正文左边缘对齐，扫读时不跳。
        ImGui::TextDisabled("%s", entry.time.c_str());
        ImGui::SameLine(76.0f);

        if (state.focusConsoleRequest && index == state.lastErrorLogIndex)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.75f, 0.2f, 0.2f, 1.0f));
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
