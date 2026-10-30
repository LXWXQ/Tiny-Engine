#pragma once

#include "Editor/EditorPanel.h"

#include <entt/entt.hpp>

namespace editor
{

// 属性面板：显示并编辑选中实体的各个组件。
// 所有修改都走 CommandStack，保证可撤销。
class InspectorPanel : public EditorPanel
{
public:
    const char* name() const override { return "Inspector"; }
    void draw(EditorState& state) override;

private:
    void drawTransform(EditorState& state, entt::entity entity);
    void drawMesh(EditorState& state, entt::entity entity);
    void drawMaterial(EditorState& state, entt::entity entity);
    void drawRenderTag(EditorState& state, entt::entity entity);
};

} // namespace editor
