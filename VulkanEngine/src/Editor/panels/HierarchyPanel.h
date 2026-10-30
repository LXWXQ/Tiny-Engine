#pragma once

#include "Editor/EditorPanel.h"

#include <entt/entt.hpp>

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
};

} // namespace editor
