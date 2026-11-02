#include "Editor/panels/HierarchyPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <cstring>

namespace editor
{

namespace
{
// 取实体显示名。没有 NameComponent 时退化成一个稳定的占位名（用 ID）。
std::string displayName(entt::registry& registry, entt::entity entity)
{
    if (const auto* name = registry.try_get<NameComponent>(entity))
        return name->name;
    return "Entity " + std::to_string(static_cast<uint32_t>(entity));
}

// 判断 entity 是否是 ancestor 的后代（拖拽重挂时需要拒绝成环）
bool isDescendantOf(entt::registry& registry, entt::entity entity, entt::entity ancestor)
{
    entt::entity current = entity;
    while (current != entt::null && registry.valid(current))
    {
        if (current == ancestor) return true;
        const auto* h = registry.try_get<HierarchyComponent>(current);
        current = h ? h->parent : entt::null;
    }
    return false;
}
} // namespace

void HierarchyPanel::draw(EditorState& state)
{
    if (!ImGui::Begin("Hierarchy", &visible))
    {
        ImGui::End();
        return;
    }

    entt::registry& registry = *state.services.registry;

    if (ImGui::Button("Create Empty"))
    {
        const entt::entity entity = state.services.scene->createEntity();
        registry.emplace_or_replace<NameComponent>(entity, NameComponent{ "Empty" });
        state.selection.select(entity);
        state.markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Create Terrain"))
    {
        terrain::TerrainConfig config;
        const entt::entity entity = createTerrainThroughCommand(
            registry, *state.services.scene, state.commands, config, "Terrain");
        if (entity != entt::null)
        {
            state.selection.select(entity);
            state.markDirty();
        }
        else
        {
            state.pushLog(LogEntry::Level::Error, "[Editor] create terrain failed");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d entities)", static_cast<int>(registry.view<TransformComponent>().size() - registry.view<TerrainChunkComponent>().size()));

    ImGui::Separator();

    // 从根开始递归。用 roots 列表而不是遍历全部实体，
    // 是为了让顺序稳定（按创建顺序），否则树会随 EnTT 内部存储顺序跳动。
    std::vector<entt::entity> roots;
    for (entt::entity entity : registry.view<TransformComponent>())
    {
        if (registry.any_of<TerrainChunkComponent>(entity))
            continue;
        const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
        const bool isRoot = !hierarchy || hierarchy->parent == entt::null ||
                            !registry.valid(hierarchy->parent);
        if (isRoot)
            roots.push_back(entity);
    }

    for (entt::entity entity : roots)
        drawEntityNode(state, entity);

    // 点空白处取消选中
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered())
    {
        state.selection.clear();
    }

    ImGui::End();
}

void HierarchyPanel::drawEntityNode(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    if (!registry.valid(entity) || registry.any_of<TerrainChunkComponent>(entity))
        return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (state.selection.contains(entity))
        flags |= ImGuiTreeNodeFlags_Selected;

    const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
    const bool hasChildren = hierarchy && !hierarchy->children.empty();
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    // 用实体 ID 作为 ImGui ID，避免同名实体互相干扰
    ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));

    const std::string label = displayName(registry, entity);
    const bool opened = ImGui::TreeNodeEx(label.c_str(), flags);

    // ---- 选中 ----
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        state.selection.select(entity);

    // ---- 拖拽重挂 ----
    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("SENTINEL_ENTITY", &entity, sizeof(entt::entity));
        ImGui::TextUnformatted(label.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SENTINEL_ENTITY"))
        {
            const entt::entity dragged = *static_cast<const entt::entity*>(payload->Data);
            // 拒绝成环：不能把父节点拖到自己的后代下面
            if (dragged != entity && registry.valid(dragged) &&
                !isDescendantOf(registry, entity, dragged))
            {
                state.services.scene->setParent(dragged, entity);
                state.markDirty();
            }
        }
        ImGui::EndDragDropTarget();
    }

    drawContextMenu(state, entity);

    if (opened && hasChildren)
    {
        // 拷贝 children：绘制过程中可能因为删除/重挂而修改数组
        const std::vector<entt::entity> children = hierarchy->children;
        for (entt::entity child : children)
            drawEntityNode(state, child);
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void HierarchyPanel::drawContextMenu(EditorState& state, entt::entity entity)
{
    if (!ImGui::BeginPopupContextItem("entity_context"))
        return;

    if (ImGui::MenuItem("Delete"))
    {
        state.selection.clear();
        state.commands.execute(std::make_unique<DeleteEntityCommand>(
            *state.services.registry, entity, state.services.scene));
        state.markDirty();
    }
    if (ImGui::MenuItem("Unparent"))
    {
        state.services.scene->setParent(entity, entt::null);
        state.markDirty();
    }

    ImGui::EndPopup();
}

} // namespace editor
