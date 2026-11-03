#pragma once

#include "Editor/EditorPanel.h"

#include <entt/entt.hpp>
#include <string>
#include <vector>

namespace editor
{

// 场景层级树：显示实体父子关系，支持选中、新建、删除、拖拽重挂。
class HierarchyPanel : public EditorPanel
{
public:
    const char* name() const override { return "Hierarchy"; }
    void draw(EditorState& state) override;

private:
    void drawEntityNode(EditorState& state, entt::entity entity);
    void drawContextMenu(EditorState& state, entt::entity entity);

    // E1：多选锚点。Shift 点击按这个锚点选一段，Ctrl 点击切换单个。
    int selectionAnchor = -1;
    // 本帧的绘制顺序（树展平），Shift 区间选择依赖它。
    std::vector<entt::entity> visibleOrder;
    // 搜索过滤（大小写不敏感的子串匹配）。
    char filterBuffer[128] = {};
};

} // namespace editor
