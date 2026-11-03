#pragma once

#include "Editor/EditorPanel.h"
#include "Editor/Commands.h"
#include "Editor/TextureLibrary.h"

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
    void drawCharacter(EditorState& state, entt::entity entity);
    void drawTerrain(EditorState& state, entt::entity entity);
    void drawStreamingTerrain(EditorState& state, entt::entity entity);
    void generateTerrain(EditorState& state, entt::entity entity);
    void drawMesh(EditorState& state, entt::entity entity);
    void drawMaterial(EditorState& state, entt::entity entity);
    void drawRenderTag(EditorState& state, entt::entity entity);

    // E6：一行贴图（当前文件名 + 选择按钮 + 弹窗列表）。
    // srgb 由贴图语义决定：颜色类 true，法线/金属粗糙度/AO 必须 false。
    void drawTextureRow(EditorState& state, entt::entity entity, Material& material,
                        const char* label, SetMaterialTextureCommand::Slot slot, bool srgb);

    TextureLibrary textures;
    bool texturesScanned = false;
    char textureFilter[64] = {};
};

} // namespace editor
