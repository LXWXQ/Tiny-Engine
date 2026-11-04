#include "Editor/panels/HierarchyPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
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

// E1：大小写不敏感子串匹配。
bool matchesFilter(const std::string& text, const std::string& filter)
{
    if (filter.empty())
        return true;
    std::string haystack = text;
    std::string needle = filter;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

// 按"根 + 深度优先子节点"的顺序展平，供 Shift 区间选择使用。
void collectOrder(entt::registry& registry, entt::entity entity,
                  std::vector<entt::entity>& out)
{
    if (!registry.valid(entity) || registry.any_of<TerrainChunkComponent>(entity))
        return;
    out.push_back(entity);
    const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
    if (hierarchy == nullptr)
        return;
    const std::vector<entt::entity> children = hierarchy->children;
    for (entt::entity child : children)
        collectOrder(registry, child, out);
}

int indexOf(const std::vector<entt::entity>& order, entt::entity entity)
{
    const auto it = std::find(order.begin(), order.end(), entity);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
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

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search...", filterBuffer, IM_ARRAYSIZE(filterBuffer));

    ImGui::Separator();

    // UX-01：空树也要说话。冷启动时编辑器不加载 Sponza，Hierarchy 是空的，
    // 用户需要知道这是「还没有东西」而不是「加载失败了」。
    if (filterBuffer[0] == 0 && registry.view<NameComponent>().size() == 0)
    {
        ImGui::TextDisabled("场景里还没有对象。");
        ImGui::TextWrapped("用上面的 Create Terrain / Create Empty，或双击左侧 Assets 里的模型开始。");
        ImGui::Spacing();
    }
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

    const std::string filter(filterBuffer);
    visibleOrder.clear();
    if (filter.empty())
    {
        for (entt::entity entity : roots)
            collectOrder(registry, entity, visibleOrder);
        for (entt::entity entity : roots)
            drawEntityNode(state, entity);
    }
    else
    {
        // 过滤时拍平成一行一个匹配项：树结构在"只看命名"的场景下反而更难读。
        for (entt::entity entity : registry.view<TransformComponent>())
        {
            if (registry.any_of<TerrainChunkComponent>(entity) || !registry.valid(entity))
                continue;
            if (!matchesFilter(displayName(registry, entity), filter))
                continue;
            visibleOrder.push_back(entity);

            const bool selected = state.selection.contains(entity);
            ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));
            if (ImGui::Selectable(displayName(registry, entity).c_str(), selected))
            if (state.allowsSelection())
            {
                ImGuiIO& io = ImGui::GetIO();
                if (io.KeyShift && selectionAnchor >= 0)
                {
                    const int clicked = indexOf(visibleOrder, entity);
                    const int first = std::min(selectionAnchor, clicked);
                    const int last = std::max(selectionAnchor, clicked);
                    state.selection.clear();
                    for (int i = first; i <= last; ++i)
                        state.selection.add(visibleOrder[static_cast<size_t>(i)]);
                }
                else if (io.KeyCtrl)
                {
                    state.selection.toggle(entity);
                }
                else
                {
                    state.selection.select(entity);
                    selectionAnchor = indexOf(visibleOrder, entity);
                }
            }
            drawContextMenu(state, entity);
            ImGui::PopID();
        }
    }

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

    // ---- 选中（E1：Ctrl 切换、Shift 连续区间）----
    // E8：运行模式下层级面板只负责导航/展开，不能改变选中。
    if (state.allowsSelection() && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        ImGuiIO& io = ImGui::GetIO();
        const int clickedIndex = indexOf(visibleOrder, entity);
        if (io.KeyShift && selectionAnchor >= 0 && clickedIndex >= 0)
        {
            const int first = std::min(selectionAnchor, clickedIndex);
            const int last = std::max(selectionAnchor, clickedIndex);
            state.selection.clear();
            for (int i = first; i <= last; ++i)
                state.selection.add(visibleOrder[static_cast<size_t>(i)]);
        }
        else if (io.KeyCtrl)
        {
            state.selection.toggle(entity);
        }
        else
        {
            state.selection.select(entity);
            selectionAnchor = clickedIndex;
        }
    }

    // ---- 拖拽重挂 ----
    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("TINY_ENTITY", &entity, sizeof(entt::entity));
        ImGui::TextUnformatted(label.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TINY_ENTITY"))
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

    const bool inSelection = state.selection.contains(entity);
    const bool deleteWholeSelection = inSelection && state.selection.size() > 1;
    if (ImGui::MenuItem(deleteWholeSelection ? "Delete Selected" : "Delete"))
    {
        if (deleteWholeSelection)
        {
            std::vector<std::unique_ptr<Command>> children;
            const entt::entity cameraEntity = state.services.scene->getCameraEntity();
            for (entt::entity selected : state.selection.entities)
            {
                if (selected == cameraEntity || !state.services.registry->valid(selected))
                    continue;
                if (state.services.registry->any_of<TerrainChunkComponent>(selected) ||
                    state.services.registry->any_of<TerrainStreamingComponent>(selected))
                    continue;
                children.push_back(std::make_unique<DeleteEntityCommand>(
                    *state.services.registry, selected, state.services.scene));
            }
            if (!children.empty())
            {
                state.selection.clear();
                state.commands.execute(std::make_unique<CompositeCommand>(
                    "Delete Entities", std::move(children)));
                state.markDirty();
            }
        }
        else
        {
            state.selection.clear();
            state.commands.execute(std::make_unique<DeleteEntityCommand>(
                *state.services.registry, entity, state.services.scene));
            state.markDirty();
        }
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
    {
        if (DuplicateEntityCommand::canDuplicate(*state.services.registry, entity))
        {
            auto command = std::make_unique<DuplicateEntityCommand>(
                *state.services.registry, state.services.scene, entity);
            DuplicateEntityCommand* raw = command.get();
            state.commands.execute(std::move(command));
            if (raw->duplicatedRoot() != entt::null)
                state.selection.select(raw->duplicatedRoot());
            state.markDirty();
        }
    }
    if (ImGui::MenuItem("Unparent"))
    {
        state.services.scene->setParent(entity, entt::null);
        state.markDirty();
    }

    ImGui::EndPopup();
}

} // namespace editor
