#pragma once

// ===========================================================================
// 编辑器偏好（跨会话状态）
//
// 只放"用户习惯"，不放文档内容：
//   * 面板可见性 / Gizmo 模式  -> 下次启动还应该是这样
//   * 最近打开的场景           -> File -> Open Recent
//
// 为什么不写 config/engine.json：那是引擎运行参数（进版本库当默认预设），
// 编辑器偏好是本机习惯，生命周期不同。存 config/editor.json，不进版本库。
//
// 面板用名字而不是枚举做键：加面板只需要 registerPanel + View 菜单 +1 行，
// 偏好文件自然跟着走，不用改这里的结构。
// ===========================================================================

#include <map>
#include <string>
#include <vector>

namespace editor
{

struct EditorPreferences
{
    std::map<std::string, bool> panelVisibility;   // 面板名 -> 可见；缺失按 true 处理
    int gizmoMode = 0;                             // 0=Translate 1=Rotate 2=Scale，与 GizmoMode 枚举顺序一致

    std::vector<std::string> recentScenes;         // 队首最新
    static constexpr size_t kMaxRecentScenes = 8;

    bool panelVisible(const std::string& name) const;
    void setPanelVisible(const std::string& name, bool visible);

    // 读不到文件不算失败路径（首次运行），返回 false 时 error 只是说明。
    bool load(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error) const;

    // 最近场景：去重 + 置顶 + 截断。空路径忽略。
    void pushRecent(const std::string& scenePath);

    static bool selfTest();
};

} // namespace editor
