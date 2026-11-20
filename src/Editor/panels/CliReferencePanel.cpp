#include "Editor/panels/CliReferencePanel.h"

#include "Core/CliCatalog.h"

#include <imgui.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace editor
{

namespace
{

// 分组名（去重后保序）只算一次：目录是编译期常量表，运行期不会变。
const std::vector<const char*>& groupNames()
{
    static const std::vector<const char*> names = []()
    {
        std::vector<const char*> result;
        const auto& lines = tiny::cli::catalog();
        for (const auto& line : lines)
        {
            bool seen = false;
            for (const char* existing : result)
            {
                if (std::strcmp(existing, line.group) == 0)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                result.push_back(line.group);
        }
        return result;
    }();
    return names;
}

} // namespace

CliReferencePanel::CliReferencePanel()
{
    // 查阅型面板：默认不占屏幕，从「帮助」菜单打开。
    visible = false;
}

bool CliReferencePanel::matches(const std::string& haystack, const char* needle)
{
    if (needle == nullptr || needle[0] == 0)
        return true;

    const size_t needleLength = std::strlen(needle);
    if (needleLength > haystack.size())
        return false;

    for (size_t i = 0; i + needleLength <= haystack.size(); ++i)
    {
        size_t j = 0;
        for (; j < needleLength; ++j)
        {
            const unsigned char a = static_cast<unsigned char>(haystack[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            const int lowerA = (a >= 'A' && a <= 'Z') ? a + ('a' - 'A') : a;
            const int lowerB = (b >= 'A' && b <= 'Z') ? b + ('a' - 'A') : b;
            if (lowerA != lowerB)
                break;
        }
        if (j == needleLength)
            return true;
    }
    return false;
}

void CliReferencePanel::draw(EditorState& /*state*/)
{
    if (!visible)
        return;

    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("命令行选项", &visible))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("与 TinyEngine.exe --list-options 同源；改选项只改 Core/CliCatalog.cpp 一处。");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##cli-search", "搜索开关 / 说明", search_, sizeof(search_));

    ImGui::SameLine();
    const auto& groups = groupNames();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("##cli-group", groupFilter_ == 0 ? "全部分组" : groups[static_cast<size_t>(groupFilter_ - 1)]))
    {
        if (ImGui::Selectable("全部分组", groupFilter_ == 0))
            groupFilter_ = 0;
        for (size_t i = 0; i < groups.size(); ++i)
        {
            const bool selected = groupFilter_ == static_cast<int>(i) + 1;
            if (ImGui::Selectable(groups[i], selected))
                groupFilter_ = static_cast<int>(i) + 1;
        }
        ImGui::EndCombo();
    }

    const char* activeGroup = groupFilter_ == 0 ? nullptr : groups[static_cast<size_t>(groupFilter_ - 1)];

    int shown = 0;
    const auto& lines = tiny::cli::catalog();
    if (ImGui::BeginTable("##cli-table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("分组", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("开关", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& line : lines)
        {
            if (activeGroup != nullptr && std::strcmp(activeGroup, line.group) != 0)
                continue;
            if (!matches(line.flags, search_) && !matches(line.text, search_) &&
                !matches(line.group, search_))
                continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", line.group);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(line.flags);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", line.text);
            ++shown;
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("显示 %d / %zu 条；都传给 TinyEngine.exe，编辑器自身只认极少数开关。",
        shown, lines.size());
    ImGui::TextDisabled("想直接看命令行版本：TinyEngine.exe --list-options");

    ImGui::End();
}

bool CliReferencePanel::selfTest()
{
    const auto& lines = tiny::cli::catalog();
    if (lines.size() < 70)
    {
        std::printf("[Editor] cli reference selfTest FAIL: catalog too small (%zu)\n", lines.size());
        return false;
    }

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        if (line.group == nullptr || line.group[0] == 0 || line.flags == nullptr ||
            line.flags[0] == 0 || line.text == nullptr || line.text[0] == 0)
        {
            std::printf("[Editor] cli reference selfTest FAIL: empty field at %zu\n", i);
            return false;
        }
    }

    // 搜索：大小写折叠、子串命中、过长关键词不越界。
    if (!matches("--light-stress-radius", "RADIUS"))
    {
        std::printf("[Editor] cli reference selfTest FAIL: case folding\n");
        return false;
    }
    if (matches("--selftest", "lightstress"))
    {
        std::printf("[Editor] cli reference selfTest FAIL: false positive\n");
        return false;
    }
    if (matches("--lights", "--light-stress-radius"))
    {
        std::printf("[Editor] cli reference selfTest FAIL: needle longer than haystack\n");
        return false;
    }
    if (!matches("--light-stress", ""))
    {
        std::printf("[Editor] cli reference selfTest FAIL: empty needle must match\n");
        return false;
    }

    // 分组去重后必须覆盖每一条的 group（否则下拉框会漏项）。
    const auto& groups = groupNames();
    for (const auto& line : lines)
    {
        bool found = false;
        for (const char* group : groups)
        {
            if (std::strcmp(group, line.group) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            std::printf("[Editor] cli reference selfTest FAIL: group missing %s\n", line.group);
            return false;
        }
    }

    std::printf("[Editor] cli reference selfTest PASS | %zu options in %zu groups, filter stable\n",
        lines.size(), groups.size());
    return true;
}

} // namespace editor
