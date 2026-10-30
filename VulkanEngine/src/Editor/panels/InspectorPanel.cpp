#include "Editor/panels/InspectorPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

namespace editor
{

void InspectorPanel::draw(EditorState& state)
{
    if (!ImGui::Begin("Inspector", &visible))
    {
        ImGui::End();
        return;
    }

    entt::registry& registry = *state.services.registry;
    const entt::entity entity = state.selection.primary();

    if (entity == entt::null || !registry.valid(entity))
    {
        ImGui::TextDisabled("No entity selected.");
        ImGui::TextDisabled("Select one in the Hierarchy panel.");
        ImGui::End();
        return;
    }

    // ---- 名字（可编辑，走命令栈）----
    {
        const auto* nameComp = registry.try_get<NameComponent>(entity);
        std::string current = nameComp ? nameComp->name : std::string("Entity");

        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());

        ImGui::TextDisabled("Entity");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##name", buffer, sizeof(buffer),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            const std::string newName(buffer);
            if (newName != current)
            {
                state.commands.execute(std::make_unique<RenameEntityCommand>(
                    registry, entity, current, newName));
                state.markDirty();
            }
        }
    }

    ImGui::Separator();

    drawTransform(state, entity);
    drawMesh(state, entity);
    drawMaterial(state, entity);
    drawRenderTag(state, entity);

    ImGui::End();
}

void InspectorPanel::drawTransform(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    auto* transform = registry.try_get<TransformComponent>(entity);
    if (!transform)
        return;

    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // 先保存旧值，编辑结束后如果有变化就提交一条命令。
    // 为什么用 DragFloat 的返回值而不是 IsItemDeactivatedAfterEdit：
    // 拖拽过程中每帧都要看到变化，所以每帧都可能提交 —— 依靠 CommandStack
    // 的合并窗口把连续拖拽压成一条命令。
    const glm::vec3 oldTranslation = transform->translation;
    const glm::vec3 oldRotation = transform->rotation;
    const glm::vec3 oldScale = transform->scale;

    glm::vec3 translation = oldTranslation;
    glm::vec3 rotationDegrees = glm::degrees(oldRotation);
    glm::vec3 scale = oldScale;

    bool changed = false;
    changed |= ImGui::DragFloat3("Translation", glm::value_ptr(translation), 0.05f);
    changed |= ImGui::DragFloat3("Rotation", glm::value_ptr(rotationDegrees), 0.5f);
    changed |= ImGui::DragFloat3("Scale", glm::value_ptr(scale), 0.05f, 0.001f, 1000.0f);

    if (changed)
    {
        if (scale.x < 0.001f) scale.x = 0.001f;
        if (scale.y < 0.001f) scale.y = 0.001f;
        if (scale.z < 0.001f) scale.z = 0.001f;

        state.commands.execute(std::make_unique<SetTransformCommand>(
            registry, entity,
            oldTranslation, translation,
            oldRotation, glm::radians(rotationDegrees),
            oldScale, scale));
        state.markDirty();
        state.transformRefreshPending = true;
    }
}

void InspectorPanel::drawMesh(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    const auto* mesh = registry.try_get<MeshComponent>(entity);
    if (!ImGui::CollapsingHeader("Mesh"))
        return;

    if (mesh && mesh->model)
    {
        ImGui::Text("Path: %s", mesh->model->getSourcePath().c_str());
        ImGui::Text("Vertices: %u", mesh->model->getVertexCount());
        ImGui::Text("Indices : %u", mesh->model->getIndexCount());
        ImGui::Text("SubMeshes: %d", static_cast<int>(mesh->model->getSubMeshes().size()));
    }
    else
    {
        ImGui::TextDisabled("(no mesh)");
    }
}

void InspectorPanel::drawMaterial(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    auto* materialComp = registry.try_get<MaterialComponent>(entity);
    if (!materialComp || !materialComp->material)
        return;

    if (!ImGui::CollapsingHeader("Material"))
        return;

    Material& m = *materialComp->material;

    ImGui::Text("Name: %s", m.name.c_str());
    ImGui::Text("Slot: %u", materialComp->materialSetIndex);

    // 材质参数修改不经过命令栈：它们是"外观调参"，撤销的需求弱，
    // 而每帧写回磁盘/重建描述符的成本更高。这是一个有意的取舍。
    ImGui::ColorEdit4("Base Color", glm::value_ptr(m.baseColorFactor));
    ImGui::SliderFloat("Metallic", &m.metallicFactor, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &m.roughnessFactor, 0.0f, 1.0f);

    const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
    int alphaMode = static_cast<int>(m.alphaMode);
    if (ImGui::Combo("Alpha Mode", &alphaMode, alphaModes, 3))
    {
        m.alphaMode = static_cast<AlphaMode>(alphaMode);
        state.markDirty();
    }

    ImGui::TextDisabled("Textures:");
    auto showTex = [](const char* label, const std::shared_ptr<Texture>& tex)
    {
        if (tex)
            ImGui::Text("  %s: %s", label, tex->getPath().c_str());
        else
            ImGui::TextDisabled("  %s: (none)", label);
    };
    showTex("Albedo", m.albedoMap);
    showTex("Normal", m.normalMap);
    showTex("MetalRough", m.metallicRoughnessMap);
}

void InspectorPanel::drawRenderTag(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    auto* tag = registry.try_get<RenderTagComponent>(entity);
    if (!tag)
        return;

    if (!ImGui::CollapsingHeader("Render Tag"))
        return;

    bool changed = false;
    changed |= ImGui::Checkbox("Skybox", &tag->isSkybox);
    changed |= ImGui::Checkbox("Grid", &tag->isGrid);
    if (changed)
        state.markDirty();
}

} // namespace editor
