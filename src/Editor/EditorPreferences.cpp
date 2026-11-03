#include "Editor/EditorPreferences.h"

#include <json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace editor
{
namespace
{

std::string toJson(const EditorPreferences& prefs)
{
    nlohmann::json panels = nlohmann::json::object();
    for (const auto& [name, visible] : prefs.panelVisibility)
        panels[name] = visible;

    nlohmann::json root;
    root["version"] = 1;
    root["panels"] = panels;
    root["gizmoMode"] = prefs.gizmoMode;
    root["recentScenes"] = prefs.recentScenes;
    return root.dump(2);
}

bool readBool(const nlohmann::json& object, const char* key, bool fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean())
        return fallback;
    return it->get<bool>();
}

bool fromJson(const std::string& text, EditorPreferences& out, std::string& error)
{
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(text);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    if (!root.is_object())
    {
        error = "root is not an object";
        return false;
    }

    if (const auto panels = root.find("panels"); panels != root.end() && panels->is_object())
    {
        for (const auto& [name, value] : panels->items())
        {
            if (name.empty() || !value.is_boolean())
                continue;
            out.panelVisibility[name] = value.get<bool>();
        }
    }

    if (const auto mode = root.find("gizmoMode"); mode != root.end() && mode->is_number_integer())
        out.gizmoMode = std::clamp(mode->get<int>(), 0, 2);

    if (const auto recent = root.find("recentScenes"); recent != root.end() && recent->is_array())
    {
        out.recentScenes.clear();
        // 文件里是"队首最新"，pushRecent 又是插到队首，
        // 所以必须倒序喂进去，否则每次读写都会把顺序翻一次。
        for (auto entry = recent->rbegin(); entry != recent->rend(); ++entry)
        {
            if (entry->is_string())
                out.pushRecent(entry->get<std::string>());
        }
    }
    return true;
}

} // namespace

bool EditorPreferences::panelVisible(const std::string& name) const
{
    const auto it = panelVisibility.find(name);
    return it == panelVisibility.end() ? true : it->second;
}

void EditorPreferences::setPanelVisible(const std::string& name, bool visible)
{
    if (!name.empty())
        panelVisibility[name] = visible;
}

void EditorPreferences::pushRecent(const std::string& scenePath)
{
    if (scenePath.empty())
        return;

    // 去重用路径字符串本身：同一文件的不同写法会各留一条，但这比"猜规范化"安全。
    recentScenes.erase(std::remove(recentScenes.begin(), recentScenes.end(), scenePath), recentScenes.end());
    recentScenes.insert(recentScenes.begin(), scenePath);
    if (recentScenes.size() > kMaxRecentScenes)
        recentScenes.resize(kMaxRecentScenes);
}

bool EditorPreferences::load(const std::string& path, std::string& error)
{
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        error = "no preferences file";
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return fromJson(buffer.str(), *this, error);
}

bool EditorPreferences::save(const std::string& path, std::string& error) const
{
    std::error_code code;
    const std::filesystem::path target(path);
    if (target.has_parent_path())
        std::filesystem::create_directories(target.parent_path(), code);

    std::ofstream stream(path, std::ios::trunc);
    if (!stream.is_open())
    {
        error = "cannot open preferences for writing";
        return false;
    }
    stream << toJson(*this);
    if (!stream.good())
    {
        error = "write failed";
        return false;
    }
    return true;
}

bool EditorPreferences::selfTest()
{
    EditorPreferences prefs;
    prefs.setPanelVisible("Console", false);
    prefs.gizmoMode = 2;
    prefs.pushRecent("a.scene.json");
    prefs.pushRecent("b.scene.json");
    prefs.pushRecent("a.scene.json");          // 重复 -> 置顶且不产生第二条

    std::string error;
    EditorPreferences loaded;
    if (!fromJson(toJson(prefs), loaded, error))
    {
        std::cerr << "[Editor] preferences selfTest FAIL: round trip parse: " << error << std::endl;
        return false;
    }
    if (loaded.panelVisible("Console") || !loaded.panelVisible("Inspector") ||
        loaded.gizmoMode != 2 || loaded.recentScenes.size() != 2 ||
        loaded.recentScenes[0] != "a.scene.json")
    {
        std::cerr << "[Editor] preferences selfTest FAIL: values lost in round trip" << std::endl;
        return false;
    }

    // 上限：超出 kMaxRecentScenes 必须截断，且队首是最新的。
    for (int i = 0; i < 12; ++i)
        prefs.pushRecent("scene" + std::to_string(i) + ".scene.json");
    if (prefs.recentScenes.size() != kMaxRecentScenes || prefs.recentScenes.front() != "scene11.scene.json")
    {
        std::cerr << "[Editor] preferences selfTest FAIL: recent list is not capped" << std::endl;
        return false;
    }

    // 坏文件不能让编辑器起不来：解析失败只返回 false 并给说明。
    EditorPreferences broken;
    if (fromJson("{ not json", broken, error) || error.empty())
    {
        std::cerr << "[Editor] preferences selfTest FAIL: broken json accepted" << std::endl;
        return false;
    }

    std::cout << "[Editor] preferences selfTest PASS | round trip + recent dedupe/cap + broken json" << std::endl;
    return true;
}

} // namespace editor
