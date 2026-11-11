#include "Editor/panels/InspectorPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <random>
#include "Voxel/VoxelTypes.h"

#include <string>

namespace editor
{

namespace
{
void drawVoxelBrushControls(EditorState& state)
{
    if (!ImGui::CollapsingHeader("Voxel Brush"))
        return;

    // E8：体素操作不再是"笔刷开关"，而是运行模式的默认鼠标语义。
    ImGui::TextDisabled("运行模式：左键挖、右键放（F8 切换）");

    if (state.voxelWorld == entt::null)
    {
        ImGui::TextDisabled("还没有体素世界 —— 用「创建 -> 体素世界」或 --voxel-world 启动。");
        return;
    }

    int block = std::clamp(state.voxelBrushBlock, 0, static_cast<int>(voxel::kBlockCount) - 1);
    const char* names[voxel::kBlockCount] = {};
    for (uint32_t i = 0; i < voxel::kBlockCount; ++i)
        names[i] = voxel::blockName(static_cast<uint8_t>(i));
    if (ImGui::Combo("Block", &block, names, static_cast<int>(voxel::kBlockCount)))
        state.voxelBrushBlock = block;
    ImGui::TextDisabled("Runtime mode: LMB digs, RMB places on the hit face. Edit mode: LMB only selects.");
}

void drawTerrainBrushControls(EditorState& state)
{
    if (!ImGui::CollapsingHeader("Sculpt Brush"))
        return;

    ImGui::Checkbox("Enable", &state.terrainBrushEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("runtime mode, LMB");

    int mode = static_cast<int>(state.terrainBrush.mode);
    const char* modes[] = {
        terrain::terrainBrushModeLabel(terrain::TerrainBrushMode::Raise),
        terrain::terrainBrushModeLabel(terrain::TerrainBrushMode::Lower),
        terrain::terrainBrushModeLabel(terrain::TerrainBrushMode::Smooth),
        terrain::terrainBrushModeLabel(terrain::TerrainBrushMode::Flatten)
    };
    if (ImGui::Combo("Mode", &mode, modes, 4))
        state.terrainBrush.mode = static_cast<terrain::TerrainBrushMode>(mode);

    ImGui::DragFloat("Radius", &state.terrainBrush.radius, 0.05f, 0.25f, 64.0f, "%.2f");
    ImGui::DragFloat("Strength", &state.terrainBrush.strength, 0.01f, 0.01f, 2.0f, "%.2f");
    ImGui::TextDisabled("World-space center: %.2f, %.2f, %.2f",
                        state.terrainBrush.center.x,
                        state.terrainBrush.center.y,
                        state.terrainBrush.center.z);
    ImGui::TextDisabled("Ctrl+left click terrain in the viewport to apply.");
}

// 通用属性控件。drawComponents 与原生 Character 面板共用，避免两套 switch。
// defaults 是组件默认值快照：右键菜单用它做“重置为默认值”，没有默认值时
// 不弹菜单。属性元数据（范围/拖动速度/提示）来自 ComponentRegistry。
bool drawPropertyWidgets(scene::ComponentRecord& record,
                         const scene::ComponentDescriptor& descriptor,
                         const scene::ComponentRecord* defaults = nullptr)
{
    bool changed = false;
    for (const scene::PropertyDescriptor& property : descriptor.properties)
    {
        scene::NamedProperty* value = record.find(property.name);
        if (value == nullptr)
            continue;
        // 贴图等资源字段由组件自己的专用控件绘制，通用控件不重复出现，
        // 也不允许手输绝对路径。
        if (property.hidden)
            continue;

        ImGui::PushID(property.name.c_str());
        const scene::NamedProperty* defaultValue =
            defaults != nullptr ? defaults->find(property.name) : nullptr;
        const bool hasRange = property.hasRange && property.maximum > property.minimum;
        const float fallbackSpeed =
            property.type == scene::PropertyType::Int ? 1.0f : 0.01f;
        const float dragSpeed = property.dragSpeed > 0.0f ? property.dragSpeed : fallbackSpeed;

        switch (property.type)
        {
        case scene::PropertyType::Bool:
        {
            bool v = value->value.asBool();
            if (ImGui::Checkbox(property.label.c_str(), &v))
            {
                value->value = scene::PropertyValue::fromBool(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Int:
        {
            int v = value->value.asInt();
            const bool edited = hasRange
                ? ImGui::DragInt(property.label.c_str(), &v, dragSpeed,
                                 static_cast<int>(property.minimum),
                                 static_cast<int>(property.maximum), "%d")
                : ImGui::DragInt(property.label.c_str(), &v, dragSpeed);
            if (edited)
            {
                value->value = scene::PropertyValue::fromInt(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Float:
        {
            float v = value->value.asFloat();
            const bool edited = hasRange
                ? ImGui::DragFloat(property.label.c_str(), &v, dragSpeed,
                                   property.minimum, property.maximum, "%.3f")
                : ImGui::DragFloat(property.label.c_str(), &v, dragSpeed, 0.0f, 0.0f, "%.3f");
            if (edited)
            {
                value->value = scene::PropertyValue::fromFloat(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Vec3:
        {
            glm::vec3 v = value->value.asVec3();
            const bool edited = hasRange
                ? ImGui::DragFloat3(property.label.c_str(), glm::value_ptr(v), dragSpeed,
                                    property.minimum, property.maximum, "%.3f")
                : ImGui::DragFloat3(property.label.c_str(), glm::value_ptr(v),
                                    dragSpeed, 0.0f, 0.0f, "%.3f");
            if (edited)
            {
                value->value = scene::PropertyValue::fromVec3(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Color:
        {
            glm::vec4 v = value->value.asColor();
            if (ImGui::ColorEdit4(property.label.c_str(), glm::value_ptr(v)))
            {
                value->value = scene::PropertyValue::fromColor(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::String:
        {
            char buffer[256] = {};
            std::snprintf(buffer, sizeof(buffer), "%s", value->value.asString().c_str());
            if (ImGui::InputText(property.label.c_str(), buffer, sizeof(buffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                value->value = scene::PropertyValue::fromString(buffer);
                changed = true;
            }
            break;
        }
        }

        // 提示里同时给“这个字段是什么”和“怎么操作”，避免只有一句泛泛的
        // “拖动修改”。范围由注册表提供，UI 不再自己猜。
        if (ImGui::IsItemHovered())
        {
            std::string tip = property.tooltip;
            if (hasRange)
            {
                char range[96] = {};
                std::snprintf(range, sizeof(range), "范围 [%.3f, %.3f]",
                              property.minimum, property.maximum);
                if (!tip.empty())
                    tip += "\n";
                tip += range;
            }
            if (!tip.empty())
                tip += "\n";
            tip += property.type == scene::PropertyType::Bool
                ? "点击切换，右键重置"
                : "拖动修改，Ctrl+点击输入数值，右键重置";
            ImGui::SetTooltip("%s", tip.c_str());
        }

        if (defaultValue != nullptr && ImGui::BeginPopupContextItem("##property_menu"))
        {
            if (ImGui::MenuItem("重置为默认值"))
            {
                value->value = defaultValue->value;
                changed = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    return changed;
}

// 从完整组件描述里裁出一个分组（只保留指定属性），复用同一套控件与默认值
// 重置逻辑。体积雾的专用面板用它把 14 个字段分成 Placement/Appearance/...
scene::ComponentDescriptor makePropertyGroup(const scene::ComponentDescriptor& descriptor,
                                             std::initializer_list<const char*> names)
{
    scene::ComponentDescriptor group = descriptor;
    group.properties.clear();
    for (const char* name : names)
    {
        for (const scene::PropertyDescriptor& property : descriptor.properties)
        {
            if (property.name == name)
            {
                group.properties.push_back(property);
                break;
            }
        }
    }
    return group;
}

void setVec3Property(scene::ComponentRecord& record, const char* name, const glm::vec3& value)
{
    if (scene::NamedProperty* property = record.find(name))
        property->value = scene::PropertyValue::fromVec3(value);
}

// 组件剪贴板是纯数据快照，不绑定实体。复制不算场景修改，粘贴/重置才进命令栈。
void copyComponent(EditorState& state, entt::entity entity,
                   const scene::ComponentDescriptor& descriptor)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr)
        return;
    std::optional<scene::ComponentRecord> record =
        scene->readComponent(entity, descriptor.type);
    if (!record)
        return;

    state.componentClipboard = std::move(*record);
    state.componentClipboardLabel = descriptor.label;
    state.notifySuccess("已复制组件：" + descriptor.label);
}

bool canPasteComponent(const EditorState& state, const Scene& scene, entt::entity entity,
                       const scene::ComponentDescriptor& descriptor)
{
    if (!state.componentClipboard || state.componentClipboard->type != descriptor.type)
        return false;
    if (!descriptor.has(*state.services.registry, entity) &&
        !scene.canAddComponent(entity, descriptor.type))
        return false;
    const std::optional<scene::ComponentRecord> before =
        scene.readComponent(entity, descriptor.type);
    return !before || !before->equivalentTo(*state.componentClipboard);
}

bool pasteComponent(EditorState& state, entt::entity entity,
                    const scene::ComponentDescriptor& descriptor)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || !state.componentClipboard ||
        state.componentClipboard->type != descriptor.type)
        return false;

    std::optional<scene::ComponentRecord> before =
        scene->readComponent(entity, descriptor.type);
    if (!before && !scene->canAddComponent(entity, descriptor.type))
    {
        const std::string reason = scene->componentAddBlockReason(entity, descriptor.type);
        state.notify(LogEntry::Level::Warning,
                     "不能添加 " + descriptor.label + "：" + reason);
        return false;
    }
    if (before && before->equivalentTo(*state.componentClipboard))
        return false;

    state.commands.execute(std::make_unique<SetComponentRecordCommand>(
        *scene, entity, descriptor.type, std::move(before), *state.componentClipboard,
        "Paste Component"));
    state.markDirty();
    state.notifySuccess("已粘贴组件：" + descriptor.label);
    return true;
}

bool canResetComponent(const Scene& scene, entt::entity entity,
                       const scene::ComponentDescriptor& descriptor)
{
    const std::optional<scene::ComponentRecord> before =
        scene.readComponent(entity, descriptor.type);
    const std::optional<scene::ComponentRecord> defaults =
        scene.defaultComponentRecord(descriptor.type);
    return before && defaults && !before->equivalentTo(*defaults);
}

bool resetComponent(EditorState& state, entt::entity entity,
                    const scene::ComponentDescriptor& descriptor)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr)
        return false;

    std::optional<scene::ComponentRecord> before =
        scene->readComponent(entity, descriptor.type);
    std::optional<scene::ComponentRecord> defaults =
        scene->defaultComponentRecord(descriptor.type);
    if (!before || !defaults || before->equivalentTo(*defaults))
        return false;

    state.commands.execute(std::make_unique<SetComponentRecordCommand>(
        *scene, entity, descriptor.type, std::move(before), std::move(defaults),
        "Reset Component"));
    state.markDirty();
    state.notifySuccess("已重置组件：" + descriptor.label);
    return true;
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool matchesComponentFilter(const scene::ComponentDescriptor& descriptor,
                            const std::string& query)
{
    if (query.empty())
        return true;
    return lowercase(descriptor.label).find(query) != std::string::npos ||
           lowercase(descriptor.type).find(query) != std::string::npos ||
           lowercase(descriptor.category).find(query) != std::string::npos;
}

// 原生组件由固定面板绘制，这里补统一的复制 / 重置 / 移除入口（都走命令栈）。
void drawNativeComponentFooter(EditorState& state, entt::entity entity, const char* type,
                               const char* label)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || state.services.registry == nullptr)
        return;

    entt::registry& registry = *state.services.registry;
    const scene::ComponentDescriptor* descriptor = scene->getComponentRegistry().find(type);
    if (descriptor == nullptr || !descriptor->has(registry, entity))
        return;

    const bool canRemove = descriptor->removable &&
        (!descriptor->canRemove || descriptor->canRemove(registry, entity));
    const bool resetAvailable = canResetComponent(*scene, entity, *descriptor);

    ImGui::Separator();
    ImGui::PushID(type);
    if (ImGui::Button("Copy"))
        copyComponent(state, entity, *descriptor);

    ImGui::SameLine();
    ImGui::BeginDisabled(!resetAvailable);
    if (ImGui::Button("Reset"))
        resetComponent(state, entity, *descriptor);
    ImGui::EndDisabled();
    if (!resetAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("当前值已经是默认值");

    if (descriptor->removable && !canRemove)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s is managed by its creation flow.", label);
    }
    else if (canRemove)
    {
        ImGui::SameLine();
        if (ImGui::Button("Remove Component"))
        {
            std::optional<scene::ComponentRecord> before =
                scene->readComponent(entity, type);
            if (before)
            {
                state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                    *scene, entity, type, std::move(before), std::nullopt,
                    "Remove Component"));
                state.markDirty();
            }
        }
    }
    ImGui::PopID();
}
} // namespace

void InspectorPanel::draw(EditorState& state)
{
    if (!ImGui::Begin("Inspector", &visible))
    {
        ImGui::End();
        return;
    }

    entt::registry& registry = *state.services.registry;

    // E1：多选时只给摘要与批量操作，单实体编辑器留到后续版本。
    if (state.selection.size() > 1)
    {
        ImGui::Text("%d entities selected", static_cast<int>(state.selection.size()));
        ImGui::Separator();

        if (ImGui::Button("Duplicate"))
        {
            std::vector<std::unique_ptr<Command>> children;
            std::vector<DuplicateEntityCommand*> raw;
            for (entt::entity selected : state.selection.entities)
            {
                if (!registry.valid(selected) ||
                    !DuplicateEntityCommand::canDuplicate(registry, selected))
                    continue;
                children.push_back(std::make_unique<DuplicateEntityCommand>(
                    registry, state.services.scene, selected));
                raw.push_back(static_cast<DuplicateEntityCommand*>(children.back().get()));
            }
            if (!children.empty())
            {
                state.commands.execute(std::make_unique<CompositeCommand>(
                    "Duplicate Entities", std::move(children)));
                state.selection.clear();
                for (DuplicateEntityCommand* command : raw)
                {
                    if (command->duplicatedRoot() != entt::null)
                        state.selection.add(command->duplicatedRoot());
                }
                state.markDirty();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
        {
            std::vector<std::unique_ptr<Command>> children;
            const entt::entity cameraEntity = state.services.scene->getCameraEntity();
            for (entt::entity selected : state.selection.entities)
            {
                if (!registry.valid(selected) || selected == cameraEntity)
                    continue;
                if (registry.any_of<TerrainChunkComponent>(selected) ||
                    registry.any_of<TerrainStreamingComponent>(selected))
                    continue;
                children.push_back(std::make_unique<DeleteEntityCommand>(
                    registry, selected, state.services.scene));
            }
            if (!children.empty())
            {
                state.selection.clear();
                state.commands.execute(std::make_unique<CompositeCommand>(
                    "Delete Entities", std::move(children)));
                state.markDirty();
            }
        }

        // UX-09：这里原来是两行英文播报，和中文界面混排，也没告诉用户下一步做什么。
        ImGui::TextDisabled("已选中 %d 个对象，Inspector 一次只编辑一个。",
            static_cast<int>(state.selection.size()));
        ImGui::TextDisabled("在 Hierarchy 里点单个对象查看属性；批量删除/复制用上面的按钮。");
        ImGui::End();
        return;
    }

    const entt::entity entity = state.selection.primary();

    if (entity == entt::null || !registry.valid(entity))
    {
        // 没有选中对象时显示“场景级功能组件”：阴影 / Bloom / 雾 / 水面…
        // 它们和实体组件共用同一套组件控件，只是宿主是 Scene 内部的设置实体。
        Scene* scene = state.services.scene;
        if (scene != nullptr && scene->getSceneSettingsEntity() != entt::null)
        {
            drawComponents(state, scene->getSceneSettingsEntity(),
                           scene::ComponentScope::Scene);
        }
        else
        {
            ImGui::TextDisabled("没有选中对象。");
            ImGui::TextDisabled("在 Hierarchy 里点一个对象，或用 Assets 双击模型加进来。");
        }
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
    drawFogVolume(state, entity);
    drawCharacter(state, entity);
    drawTerrain(state, entity);
    drawStreamingTerrain(state, entity);
    drawMesh(state, entity);
    drawMaterial(state, entity);
    drawRenderTag(state, entity);
    drawComponents(state, entity, scene::ComponentScope::Entity);

    ImGui::End();
}

void InspectorPanel::drawComponents(EditorState& state, entt::entity entity,
                                    scene::ComponentScope scope)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || state.services.registry == nullptr)
        return;

    entt::registry& registry = *state.services.registry;
    scene::ComponentRegistry& components = scene->getComponentRegistry();
    if (components.empty())
        return;

    const bool sceneScope = scope == scene::ComponentScope::Scene;
    ImGui::Separator();
    if (sceneScope)
    {
        ImGui::TextDisabled("Scene Components");
        ImGui::TextDisabled("场景级功能：添加 / 启用 / 调参 / 移除。移除即关闭该功能。");
    }
    else
    {
        ImGui::TextDisabled("ECS Components");
    }

    // 地形/体素对象由专用面板管理：它们不是“自由实体”，Add Component
    // 里出现旋转、体积雾等通用效果只会让用户误操作。已有组件仍然列出，
    // 方便移除旧场景里遗留的挂载。
    const bool terrainManaged = !sceneScope && scene->isTerrainManagedEntity(entity);
    if (terrainManaged)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                           "地形 / 体素对象由专用面板管理");
        ImGui::TextDisabled("这里不能添加通用组件；请在 Terrain / Voxel 面板里编辑。");
        ImGui::TextDisabled("需要体积雾、旋转等效果时，先创建一个空实体再挂组件。");
    }
    else
    {
    // 添加入口放在列表顶部：组件越多，越不应该滚到底部才能加。
    if (ImGui::Button(sceneScope ? "+ Add Scene Component" : "+ Add Component"))
    {
        componentFilter[0] = '\0';
        ImGui::OpenPopup("##add_component");
    }

    const scene::ComponentDescriptor* clipboardDescriptor = nullptr;
    if (state.componentClipboard)
    {
        clipboardDescriptor = components.find(state.componentClipboard->type);
        if (clipboardDescriptor != nullptr && clipboardDescriptor->scope != scope)
            clipboardDescriptor = nullptr;
    }
    if (clipboardDescriptor != nullptr)
    {
        ImGui::SameLine();
        const bool canPaste =
            canPasteComponent(state, *scene, entity, *clipboardDescriptor);
        const std::string label = "Paste " + clipboardDescriptor->label;
        ImGui::BeginDisabled(!canPaste);
        if (ImGui::Button(label.c_str()))
            pasteComponent(state, entity, *clipboardDescriptor);
        ImGui::EndDisabled();
        if (!canPaste && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("剪贴板内容与当前值相同，或该组件不能挂到此实体");
    }

    if (ImGui::BeginPopup("##add_component"))
    {
        ImGui::TextDisabled("添加组件");
        ImGui::SetNextItemWidth(300.0f);
        // 打开就聚焦搜索框：组件多时直接输入比滚动更快。
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##component_search", "搜索名称 / 分类 / id",
                                 componentFilter, IM_ARRAYSIZE(componentFilter));
        ImGui::Separator();

        const std::string query = lowercase(componentFilter);
        std::string lastCategory;
        bool first = true;
        bool any = false;

        for (const scene::ComponentDescriptor& descriptor : components.descriptors())
        {
            if (descriptor.scope != scope)
                continue;
            if (!descriptor.exposed || descriptor.has(registry, entity))
                continue;
            if (!matchesComponentFilter(descriptor, query))
                continue;

            if (first || descriptor.category != lastCategory)
            {
                if (!first)
                    ImGui::Separator();
                ImGui::TextDisabled("%s", descriptor.category.c_str());
                lastCategory = descriptor.category;
                first = false;
            }

            // 不只检查 addable：还要检查 Transform / 地形兼容性。禁用时
            // tooltip 直接说明“为什么不能加”，而不是只置灰。
            const std::string blockReason =
                scene->componentAddBlockReason(entity, descriptor.type);
            const bool addable = blockReason.empty();
            if (ImGui::MenuItem(descriptor.label.c_str(), nullptr, false, addable))
            {
                std::optional<scene::ComponentRecord> after =
                    scene->defaultComponentRecord(descriptor.type);
                if (after)
                {
                    state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                        *scene, entity, descriptor.type, std::nullopt, std::move(after),
                        "Add Component"));
                    state.markDirty();
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (!blockReason.empty())
                    ImGui::SetTooltip("%s", blockReason.c_str());
                else if (!descriptor.description.empty())
                    ImGui::SetTooltip("%s", descriptor.description.c_str());
            }
            any = true;
        }

        if (!any)
        {
            ImGui::TextDisabled(query.empty() ? "没有可添加的组件。"
                                              : "没有匹配的组件。");
        }
        ImGui::EndPopup();
    }
    } // !terrainManaged

    bool drewAny = false;
    for (const scene::ComponentDescriptor& descriptor : components.descriptors())
    {
        if (descriptor.scope != scope)
            continue;
        if (!descriptor.has(registry, entity))
            continue;
        drewAny = true;
        // 原生组件由固定面板绘制（Transform / Character / Render Tag…），
        // 这里只负责元数据扩展组件，避免同一份字段出现两次。
        if (!descriptor.drawProperties)
            continue;

        ImGui::PushID(descriptor.type.c_str());
        const bool open =
            ImGui::CollapsingHeader(descriptor.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

        // 右键菜单同时承担复制 / 粘贴 / 重置 / 移除：都是对整份属性快照操作，
        // 因此和属性拖拽一样走 SetComponentRecordCommand，可撤销。
        if (ImGui::BeginPopupContextItem("##component_menu"))
        {
            if (ImGui::MenuItem("复制组件"))
                copyComponent(state, entity, descriptor);

            const bool pasteAvailable =
                canPasteComponent(state, *scene, entity, descriptor);
            if (ImGui::MenuItem("粘贴组件", nullptr, false, pasteAvailable))
                pasteComponent(state, entity, descriptor);
            if (!pasteAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("剪贴板没有同类型组件，或当前值与剪贴板相同");

            const bool resetAvailable =
                canResetComponent(*scene, entity, descriptor);
            if (ImGui::MenuItem("重置为默认值", nullptr, false, resetAvailable))
                resetComponent(state, entity, descriptor);
            if (!resetAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("当前值已经是默认值");

            ImGui::Separator();
            const bool canRemove = descriptor.removable &&
                (!descriptor.canRemove || descriptor.canRemove(registry, entity));
            if (canRemove && ImGui::MenuItem("移除组件"))
            {
                std::optional<scene::ComponentRecord> before =
                    scene->readComponent(entity, descriptor.type);
                if (before)
                {
                    state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                        *scene, entity, descriptor.type, std::move(before), std::nullopt,
                        "Remove Component"));
                    state.markDirty();
                }
                ImGui::EndPopup();
                ImGui::PopID();
                continue;
            }
            ImGui::EndPopup();
        }

        if (open)
        {
            std::optional<scene::ComponentRecord> beforeRecord =
                scene->readComponent(entity, descriptor.type);
            if (beforeRecord)
            {
                scene::ComponentRecord afterRecord = *beforeRecord;
                const std::optional<scene::ComponentRecord> defaults =
                    scene->defaultComponentRecord(descriptor.type);
                bool changed = false;
                // 粒子贴图是资源选择器，不是普通字符串输入；放在参数最上面。
                if (descriptor.type == "effects.particles")
                    changed |= drawParticleTextureWidget(afterRecord);

                changed |= drawPropertyWidgets(afterRecord, descriptor,
                                               defaults ? &*defaults : nullptr);

                if (changed && !afterRecord.equivalentTo(*beforeRecord))
                {
                    state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                        *scene, entity, descriptor.type, std::move(beforeRecord),
                        std::move(afterRecord), "Modify Component"));
                    state.markDirty();
                }
            }
        }

        ImGui::PopID();
    }

    if (!drewAny && sceneScope)
    {
        ImGui::TextDisabled("当前场景没有功能组件，渲染使用引擎默认。");
        ImGui::TextDisabled("点上面的 + Add Scene Component 添加阴影、Bloom、雾等。");
    }
}

void InspectorPanel::drawFogVolume(EditorState& state, entt::entity entity)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || state.services.registry == nullptr)
        return;

    entt::registry& registry = *state.services.registry;
    const scene::ComponentDescriptor* descriptor =
        scene->getComponentRegistry().find("effects.fogVolume");
    if (descriptor == nullptr || !descriptor->has(registry, entity))
        return;

    if (!ImGui::CollapsingHeader("Fog Volume", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    std::optional<scene::ComponentRecord> before =
        scene->readComponent(entity, descriptor->type);
    if (!before)
        return;
    scene::ComponentRecord after = *before;
    const std::optional<scene::ComponentRecord> defaults =
        scene->defaultComponentRecord(descriptor->type);
    const scene::ComponentRecord* defaultRecord = defaults ? &*defaults : nullptr;

    bool changed = false;

    // 顶部只放启用开关，避免和 Placement 的大段参数混在一起。
    const scene::ComponentDescriptor enabledGroup =
        makePropertyGroup(*descriptor, { "enabled" });
    changed |= drawPropertyWidgets(after, enabledGroup, defaultRecord);
    ImGui::SameLine();
    ImGui::TextDisabled("局部体积雾盒");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", descriptor->description.c_str());

    ImGui::TextWrapped("%s", descriptor->description.c_str());

    if (ImGui::CollapsingHeader("Placement", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const scene::ComponentDescriptor placement =
            makePropertyGroup(*descriptor, { "center", "size", "rotation" });
        changed |= drawPropertyWidgets(after, placement, defaultRecord);

        ImGui::TextDisabled("Size presets");
        struct SizePreset { const char* label; glm::vec3 size; };
        static const SizePreset presets[] = {
            { "20 x 8 x 20", glm::vec3(20.0f, 8.0f, 20.0f) },
            { "40 x 12 x 40", glm::vec3(40.0f, 12.0f, 40.0f) },
            { "80 x 20 x 80", glm::vec3(80.0f, 20.0f, 80.0f) },
        };
        for (int i = 0; i < 3; ++i)
        {
            if (i > 0)
                ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::Button(presets[i].label))
            {
                setVec3Property(after, "size", presets[i].size);
                changed = true;
            }
            ImGui::PopID();
        }

        ImGui::TextDisabled("Center / Size / Rotation 是相对实体的局部空间；");
        ImGui::TextDisabled("整体移动或旋转请用上面的 Transform。视口里青色线框就是实际盒体。");
    }

    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const scene::ComponentDescriptor appearance = makePropertyGroup(
            *descriptor,
            { "density", "color", "heightFalloff", "noiseStrength",
              "noiseScale", "wind", "edgeSoftness" });
        changed |= drawPropertyWidgets(after, appearance, defaultRecord);
    }

    if (ImGui::CollapsingHeader("Lighting"))
    {
        const scene::ComponentDescriptor lighting =
            makePropertyGroup(*descriptor, { "anisotropy", "ambient" });
        changed |= drawPropertyWidgets(after, lighting, defaultRecord);
    }

    if (ImGui::CollapsingHeader("Quality"))
    {
        const scene::ComponentDescriptor quality =
            makePropertyGroup(*descriptor, { "steps" });
        changed |= drawPropertyWidgets(after, quality, defaultRecord);
    }

    if (changed && !after.equivalentTo(*before))
    {
        state.commands.execute(std::make_unique<SetComponentRecordCommand>(
            *scene, entity, descriptor->type, std::move(before), std::move(after),
            "Modify Fog Volume"));
        state.markDirty();
    }

    drawNativeComponentFooter(state, entity, descriptor->type.c_str(), "Fog Volume");
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
    const glm::vec3 oldTranslation = transform->getTranslation();
    const glm::vec3 oldRotation = transform->getEuler();
    const glm::vec3 oldScale = transform->getScale();
    // 欧拉角在万向节附近可能不是唯一表示；撤销要回到**原来的四元数**，
    // 不能从 oldRotation 反算（反算会丢掉一个自由度）。
    const glm::quat oldOrientation = transform->getOrientation();

    glm::vec3 translation = oldTranslation;
    glm::vec3 rotationDegrees = glm::degrees(oldRotation);
    glm::vec3 scale = oldScale;

    // E6：数值行统一成"拖动修改 + Ctrl 点击输入精确值 + 右键重置"。
    // 之前只有 DragFloat3 本体，用户想回默认值只能手敲三个 0/1。
    auto dragRow = [](const char* label, glm::vec3& value, float speed, const char* format,
                      const glm::vec3& resetValue, const char* tip) -> bool
    {
        bool moved = ImGui::DragFloat3(label, glm::value_ptr(value), speed, 0.0f, 0.0f, format);
        if (ImGui::BeginPopupContextItem(label))
        {
            if (ImGui::MenuItem("重置"))
            {
                value = resetValue;
                moved = true;
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s | 拖动修改 Ctrl+点击输入精确值 右键重置", tip);
        return moved;
    };

    bool changed = false;
    changed |= dragRow("Translation", translation, 0.05f, "%.3f", glm::vec3(0.0f),
                       "世界坐标（父级旋转时是父级空间）");
    changed |= dragRow("Rotation", rotationDegrees, 0.5f, "%.1f", glm::vec3(0.0f),
                       "欧拉角，单位度");
    changed |= dragRow("Scale", scale, 0.05f, "%.3f", glm::vec3(1.0f), "局部缩放，最小 0.001");

    if (changed)
    {
        if (scale.x < 0.001f) scale.x = 0.001f;
        if (scale.y < 0.001f) scale.y = 0.001f;
        if (scale.z < 0.001f) scale.z = 0.001f;

        const glm::vec3 newRotation = glm::radians(rotationDegrees);
        state.commands.execute(std::make_unique<SetTransformCommand>(
            registry, entity,
            oldTranslation, translation,
            oldRotation, newRotation,
            oldScale, scale,
            oldOrientation, TransformComponent::quatFromEuler(newRotation),
            state.services.scene));
        state.markDirty();
        state.transformRefreshPending = true;
    }
}

void InspectorPanel::drawCharacter(EditorState& state, entt::entity entity)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr)
        return;

    entt::registry& registry = *state.services.registry;
    const auto* controller = registry.try_get<CharacterControllerComponent>(entity);
    if (!controller)
        return;

    if (!ImGui::CollapsingHeader("Character Controller", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled("Kinematic heightfield controller");

    const scene::ComponentDescriptor* descriptor =
        scene->getComponentRegistry().find("native.characterController");
    if (descriptor != nullptr)
    {
        std::optional<scene::ComponentRecord> before =
            scene->readComponent(entity, "native.characterController");
        if (before)
        {
            scene::ComponentRecord after = *before;
            const std::optional<scene::ComponentRecord> defaults =
                scene->defaultComponentRecord("native.characterController");
            if (drawPropertyWidgets(after, *descriptor, defaults ? &*defaults : nullptr) &&
                !after.equivalentTo(*before))
            {
                state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                    *scene, entity, "native.characterController", std::move(before),
                    std::move(after), "Modify Character"));
                state.markDirty();
            }
        }
    }

    ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f),
                       "%s | ground %s",
                       controller->grounded ? "Grounded" : "Airborne",
                       controller->groundEntity == entt::null ? "none" : "yes");
    ImGui::TextDisabled("Play mode: WASD move, Space jump.");
    drawNativeComponentFooter(state, entity, "native.characterController",
                              "Character Controller");
}

void InspectorPanel::drawTerrain(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    auto* terrainComp = registry.try_get<TerrainComponent>(entity);
    if (!terrainComp)
        return;

    if (!ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const terrain::TerrainConfig oldConfig = terrainComp->config;
    terrain::TerrainConfig edited = oldConfig;
    bool changed = false;

    ImGui::SetNextItemWidth(-110.0f);
    changed |= ImGui::InputScalar("Seed", ImGuiDataType_U32, &edited.seed);
    ImGui::SameLine();
    if (ImGui::Button("Randomize"))
    {
        std::random_device randomDevice;
        std::uniform_int_distribution<uint32_t> distribution;
        edited.seed = distribution(randomDevice);
        changed = true;
    }

    int resolutionX = static_cast<int>(edited.resolutionX);
    int resolutionZ = static_cast<int>(edited.resolutionZ);
    changed |= ImGui::DragInt("Resolution X", &resolutionX, 1.0f, 2, 257);
    changed |= ImGui::DragInt("Resolution Z", &resolutionZ, 1.0f, 2, 257);
    edited.resolutionX = static_cast<uint32_t>(resolutionX);
    edited.resolutionZ = static_cast<uint32_t>(resolutionZ);
    if (ImGui::Button("High Detail 257"))
    {
        resolutionX = 257;
        resolutionZ = 257;
        edited.resolutionX = 257u;
        edited.resolutionZ = 257u;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Use before Generate for near-view geometry.");

    changed |= ImGui::DragFloat("Size X", &edited.sizeX, 0.5f, 0.01f, 10000.0f);
    changed |= ImGui::DragFloat("Size Z", &edited.sizeZ, 0.5f, 0.01f, 10000.0f);
    changed |= ImGui::DragFloat("Base Height", &edited.baseHeight, 0.1f, -10000.0f, 10000.0f);
    changed |= ImGui::DragFloat("Height Scale", &edited.heightScale, 0.1f, 0.0f, 1000.0f);

    if (ImGui::TreeNode("Noise"))
    {
        changed |= ImGui::DragFloat("Frequency", &edited.noiseFrequency, 0.0001f,
                                    0.0001f, 10.0f, "%.4f");
        int octaves = static_cast<int>(edited.octaveCount);
        changed |= ImGui::SliderInt("Octaves", &octaves, 1, 8);
        edited.octaveCount = static_cast<uint32_t>(octaves);
        changed |= ImGui::SliderFloat("Persistence", &edited.persistence, 0.05f, 0.95f);
        changed |= ImGui::DragFloat("Lacunarity", &edited.lacunarity, 0.01f, 1.1f, 4.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Shape"))
    {
        changed |= ImGui::DragFloat("Height Curve", &edited.heightCurve, 0.01f, 0.25f, 4.0f);
        changed |= ImGui::SliderFloat("Island Weight", &edited.islandWeight, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Island Radius", &edited.islandRadius, 0.1f, 1.0f);
        changed |= ImGui::DragFloat("Ridge Sharpness", &edited.ridgeSharpness, 0.01f, 0.25f, 4.0f);
        changed |= ImGui::DragFloat("Detail Frequency", &edited.detailFrequency, 0.01f, 1.0f, 8.0f);
        changed |= ImGui::SliderFloat("Ridge Weight", &edited.ridgeWeight, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Detail Weight", &edited.detailWeight, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Edge Falloff", &edited.edgeFalloff, 0.0f, 1.0f);
        int chunkGridX = static_cast<int>(edited.chunkGridX);
        int chunkGridZ = static_cast<int>(edited.chunkGridZ);
        changed |= ImGui::DragInt("Chunk Grid X", &chunkGridX, 0.1f, 1, 4);
        changed |= ImGui::DragInt("Chunk Grid Z", &chunkGridZ, 0.1f, 1, 4);
        edited.chunkGridX = static_cast<uint32_t>(chunkGridX);
        edited.chunkGridZ = static_cast<uint32_t>(chunkGridZ);
        int lodLevelCount = static_cast<int>(edited.lodLevelCount);
        changed |= ImGui::DragInt("LOD Levels", &lodLevelCount, 0.05f, 1, 3);
        edited.lodLevelCount = static_cast<uint32_t>(lodLevelCount);
        changed |= ImGui::SliderFloat("LOD Distance", &edited.lodDistanceScale, 0.25f, 4.0f);
        changed |= ImGui::Checkbox("Debug Skirts (fallback)", &edited.skirtsEnabled);
        changed |= ImGui::SliderFloat("Surface Detail", &edited.materialDetailStrength, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("UV Tiling", &edited.uvTiling, 0.01f, 0.01f, 100.0f);
        int surfaceStyle = static_cast<int>(edited.surfaceStyle);
        const char* surfaceStyles[] = {
            terrain::terrainSurfaceStyleLabel(0),
            terrain::terrainSurfaceStyleLabel(1),
            terrain::terrainSurfaceStyleLabel(2),
            terrain::terrainSurfaceStyleLabel(3)
        };
        changed |= ImGui::Combo("Surface Style", &surfaceStyle, surfaceStyles, 4);
        edited.surfaceStyle = static_cast<terrain::TerrainSurfaceStyle>(surfaceStyle);

        int materialSet = static_cast<int>(edited.materialSetIndex);
        const char* materialSets[] = {
            "Set 0 (default)", "Set 1", "Set 2", "Set 3"
        };
        if (ImGui::Combo("Material Set", &materialSet, materialSets, 4))
        {
            std::string materialError;
            Scene* scene = state.services.scene;
            if (scene && scene->ensureTerrainMaterialSet(
                    static_cast<uint32_t>(materialSet), materialError))
            {
                edited.materialSetIndex = static_cast<uint32_t>(materialSet);
                changed = true;
            }
            else
            {
                state.pushLog(LogEntry::Level::Error,
                              "[Editor] terrain material set failed: " + materialError);
            }
        }
        ImGui::TreePop();
    }

    ImGui::TextDisabled("Near view: raise Resolution X/Z to 193-257.");

    // Parameter edits are undoable but do not rebuild the mesh. The command
    // stack's merge window collapses a drag into one logical undo step.
    if (changed && edited != oldConfig)
    {
        state.commands.execute(std::make_unique<SetTerrainConfigCommand>(
            registry, entity, oldConfig, edited));
        state.markDirty();
    }

    if (ImGui::Button("Generate"))
        generateTerrain(state, entity);
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
    {
        const terrain::TerrainConfig defaults;
        if (terrainComp->config != defaults)
        {
            state.commands.execute(std::make_unique<SetTerrainConfigCommand>(
                registry, entity, terrainComp->config, defaults));
            state.markDirty();
        }
    }

    if (terrainComp->generating)
    {
        ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                           "Generating on JobSystem... press Generate to replace.");
    }
    else if (!terrainComp->config.geometryEquals(terrainComp->generatedConfig))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f),
                           "Parameters changed, press Generate.");
    }

    if (!terrainComp->lastError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Error: %s",
                           terrainComp->lastError.c_str());
    }

    if (terrainComp->generated)
    {
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f),
                           "Ready | generated in %.2f ms | revision %llu",
                           terrainComp->lastGenerationMs,
                           static_cast<unsigned long long>(terrainComp->revision));
        ImGui::Text("Chunks: %ux%u | LODs: %u",
                    terrainComp->config.chunkGridX, terrainComp->config.chunkGridZ,
                    terrainComp->config.lodLevelCount);
        ImGui::Text("Vertices (%s): %u | Indices: %u",
                    terrainComp->config.skirtsEnabled ? "all LODs + skirts" : "all LODs, stitched",
                    terrainComp->vertexCount, terrainComp->indexCount);
    }
    else
    {
        ImGui::TextDisabled("Not generated yet.");
    }

    drawTerrainBrushControls(state);
    drawVoxelBrushControls(state);
}

void InspectorPanel::drawStreamingTerrain(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    const auto* streaming = registry.try_get<TerrainStreamingComponent>(entity);
    if (!streaming)
        return;

    if (!ImGui::CollapsingHeader("Streamed Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled("Runtime chunks are hidden from Hierarchy.");
    ImGui::Text("Load radius: %d | Unload padding: %d",
                streaming->loadRadius, streaming->unloadPadding);
    ImGui::Text("Max loads/frame: %u", streaming->maxLoadsPerFrame);
    if (state.services.scene)
    {
        ImGui::Text("Mapped chunks: %zu | GPU height in flight: %u",
                    state.services.scene->getStreamingTerrainChunkCount(),
                    state.services.scene->getInFlightTerrainGpuRequests());
        ImGui::Text("Streaming update: %.3f ms (max %.3f ms)",
                    state.services.scene->getLastStreamingUpdateMs(),
                    state.services.scene->getMaxStreamingUpdateMs());
    }
    ImGui::TextDisabled("Delete removes the whole streamed world and is undoable.");
    drawTerrainBrushControls(state);
}

void InspectorPanel::generateTerrain(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    Scene& scene = *state.services.scene;
    auto* terrain = registry.try_get<TerrainComponent>(entity);
    auto* oldMesh = registry.try_get<MeshComponent>(entity);
    auto* oldBounds = registry.try_get<BoundsComponent>(entity);
    auto* oldTag = registry.try_get<RenderTagComponent>(entity);
    if (!terrain || !oldMesh || !oldBounds || !oldTag)
    {
        state.pushLog(LogEntry::Level::Error,
                      "[Editor] terrain is missing Mesh/Bounds/RenderTag components");
        return;
    }

    // Snapshots must survive until the worker finishes: the old mesh is still
    // bound by the current TerrainComponent while CPU generation runs.
    const TerrainComponent oldTerrain = *terrain;
    const MeshComponent oldMeshSnapshot = *oldMesh;
    const BoundsComponent oldBoundsSnapshot = *oldBounds;
    const RenderTagComponent oldTagSnapshot = *oldTag;

    EditorState* statePtr = &state;
    const bool submitted = scene.requestRegenerateTerrain(
        entity,
        [statePtr, entity, oldTerrain, oldMeshSnapshot,
         oldBoundsSnapshot, oldTagSnapshot](bool success, const std::string& errorMessage)
        {
            entt::registry& registry = *statePtr->services.registry;
            if (!registry.valid(entity))
                return;

            if (!success)
            {
                statePtr->pushLog(LogEntry::Level::Error,
                                  "[Editor] terrain generate failed: " + errorMessage);
                return;
            }

            auto* newTerrain = registry.try_get<TerrainComponent>(entity);
            auto* newMesh = registry.try_get<MeshComponent>(entity);
            auto* newBounds = registry.try_get<BoundsComponent>(entity);
            auto* newTag = registry.try_get<RenderTagComponent>(entity);
            if (!newTerrain || !newMesh || !newBounds || !newTag)
            {
                statePtr->pushLog(LogEntry::Level::Error,
                                  "[Editor] async terrain completed without expected components");
                return;
            }

            statePtr->commands.execute(std::make_unique<RegenerateTerrainCommand>(
                registry, *statePtr->services.scene, entity,
                oldTerrain, *newTerrain,
                oldMeshSnapshot, *newMesh,
                oldBoundsSnapshot, *newBounds,
                oldTagSnapshot, *newTag));
            statePtr->markDirty();

            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "[Editor] terrain generated async | %ux%u, vertices=%u, indices=%u, %.2f ms",
                          newTerrain->config.resolutionX, newTerrain->config.resolutionZ,
                          newTerrain->vertexCount, newTerrain->indexCount,
                          newTerrain->lastGenerationMs);
            statePtr->pushLog(LogEntry::Level::Info, buffer);
        });

    if (!submitted)
    {
        const std::string error = terrain->lastError.empty()
            ? "could not submit terrain generation job"
            : terrain->lastError;
        state.pushLog(LogEntry::Level::Error, "[Editor] terrain generate failed: " + error);
        return;
    }

    state.pushLog(LogEntry::Level::Info,
                  "[Editor] terrain generation submitted to JobSystem");
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

    // E6：材质默认展开 —— 「做出效果」十次有八次是从这里开始的。
    if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Material& m = *materialComp->material;

    ImGui::Text("Name: %s", m.name.c_str());
    ImGui::Text("Slot: %u", materialComp->materialSetIndex);

    // E3：材质参数走命令栈。
    //
    // 先拍快照，拖动的是局部副本；只有真的变了才提交一条 SetMaterialCommand。
    // CommandStack 的合并窗口（按实体）会把拖滑杆期间的多帧命令压成一条撤销，
    // 所以既不会污染历史，也不会每帧写盘。
    MaterialSnapshot before;
    before.baseColor = m.baseColorFactor;
    before.metallic = m.metallicFactor;
    before.roughness = m.roughnessFactor;
    before.alphaCutoff = m.alphaCutoff;
    before.alphaMode = static_cast<int>(m.alphaMode);

    MaterialSnapshot after = before;
    ImGui::ColorEdit4("Base Color", glm::value_ptr(after.baseColor));
    ImGui::SliderFloat("Metallic", &after.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &after.roughness, 0.0f, 1.0f);

    const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
    int alphaModeIndex = after.alphaMode;
    if (ImGui::Combo("Alpha Mode", &alphaModeIndex, alphaModes, 3))
        after.alphaMode = alphaModeIndex;
    if (after.alphaMode == static_cast<int>(AlphaMode::MASK_MODE))
        ImGui::SliderFloat("Alpha Cutoff", &after.alphaCutoff, 0.0f, 1.0f);

    const bool colorChanged = !glm::all(glm::equal(after.baseColor, before.baseColor));
    if (colorChanged || after.metallic != before.metallic ||
        after.roughness != before.roughness || after.alphaCutoff != before.alphaCutoff ||
        after.alphaMode != before.alphaMode)
    {
        state.commands.execute(std::make_unique<SetMaterialCommand>(
            registry, entity, before, after, state.services.scene));
        state.markDirty();
    }

    // E6：贴图可以换 —— 之前只显示路径，等于告诉用户"这里有什么"却不让改。
    // 颜色贴图走 sRGB，法线/金属粗糙度/AO 是数据贴图，必须保持 UNORM。
    ImGui::SeparatorText("贴图");
    drawTextureRow(state, entity, m, "Albedo", SetMaterialTextureCommand::Slot::Albedo, true);
    drawTextureRow(state, entity, m, "Normal", SetMaterialTextureCommand::Slot::Normal, false);
    drawTextureRow(state, entity, m, "MetalRough",
                   SetMaterialTextureCommand::Slot::MetallicRoughness, false);
    drawTextureRow(state, entity, m, "Emissive", SetMaterialTextureCommand::Slot::Emissive, true);
    drawTextureRow(state, entity, m, "AO", SetMaterialTextureCommand::Slot::Ao, false);
}

void InspectorPanel::drawTextureRow(EditorState& state, entt::entity entity, Material& material,
                                    const char* label, SetMaterialTextureCommand::Slot slot,
                                    bool srgb)
{
    ImGui::PushID(label);

    const std::shared_ptr<Texture>& current = SetMaterialTextureCommand::slotTexture(material, slot);
    const std::string currentName = current ? std::filesystem::path(current->getPath()).filename().string()
                                            : std::string("(none)");

    ImGui::TextUnformatted(label);
    ImGui::SameLine(92.0f);
    if (current)
        ImGui::TextDisabled("%s", currentName.c_str());
    else
        ImGui::TextDisabled("(none)");

    ImGui::SameLine();
    const bool pickClicked = ImGui::SmallButton("选择…");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("从 Resources/Texture 里选一张%s；当前：%s", srgb ? "颜色贴图" : "数据贴图", current ? current->getPath().c_str() : "未设置");

    if (pickClicked)
    {
        if (!texturesScanned)
        {
            textures.refresh();
            texturesScanned = true;
        }
        ImGui::OpenPopup("##texture_picker");
    }

    bool clearRequested = false;
    std::string chosenPath;
    if (ImGui::BeginPopup("##texture_picker"))
    {
        ImGui::TextDisabled("%s · %zu 张", textures.scannedRoot().c_str(), textures.entries().size());
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##tex_filter", "过滤…", textureFilter, IM_ARRAYSIZE(textureFilter));
        if (ImGui::Button("重新扫描"))
            textures.refresh();
        ImGui::SameLine();
        if (ImGui::Button("清除"))
            clearRequested = true;

        ImGui::BeginChild("##tex_list", ImVec2(360.0f, 260.0f));
        std::string filter = textureFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const TextureEntry& entry : textures.entries())
        {
            if (!filter.empty())
            {
                std::string name = entry.displayName;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name.find(filter) == std::string::npos)
                    continue;
            }
            if (ImGui::Selectable(entry.displayName.c_str()))
                chosenPath = entry.absolutePath;
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    if (clearRequested || !chosenPath.empty())
    {
        std::shared_ptr<Texture> next;
        if (!chosenPath.empty())
        {
            if (state.services.scene == nullptr)
            {
                state.notify(LogEntry::Level::Error, "无法加载贴图：场景不可用");
            }
            else if ((next = state.services.scene->loadMaterialTexture(chosenPath, srgb)) == nullptr)
            {
                state.notify(LogEntry::Level::Error, "贴图加载失败：" + chosenPath);
            }
        }

        if (clearRequested || next != nullptr)
        {
            state.commands.execute(std::make_unique<SetMaterialTextureCommand>(
                *state.services.registry, entity, slot, next, state.services.scene));
            state.markDirty();
            if (clearRequested)
                state.notifySuccess(std::string("已清除 ") + label + " 贴图");
            else
                state.notifySuccess(std::string("已更换 ") + label + " 贴图");
        }
    }

    ImGui::PopID();
}

bool InspectorPanel::drawParticleTextureWidget(scene::ComponentRecord& record)
{
    scene::NamedProperty* value = record.find("texture");
    if (value == nullptr)
        return false;

    ImGui::PushID("texture");
    std::string current = value->value.asString();
    std::string currentGeneric = current;
    std::replace(currentGeneric.begin(), currentGeneric.end(), '\\', '/');
    const std::string currentName = currentGeneric.empty()
        ? std::string("(程序化圆形)")
        : std::filesystem::path(currentGeneric).filename().string();

    ImGui::TextUnformatted("粒子贴图");
    ImGui::SameLine(92.0f);
    ImGui::TextDisabled("%s", currentName.c_str());
    if (!currentGeneric.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n空表示程序化圆形；启动时预加载，新增文件需重启", currentGeneric.c_str());

    ImGui::SameLine();
    bool pickClicked = ImGui::SmallButton("选择…");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("从 Resources/Texture/Particles 选择粒子 sprite");
    if (!currentGeneric.empty())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("清除"))
        {
            value->value = scene::PropertyValue::fromString(std::string());
            ImGui::PopID();
            return true;
        }
    }

    if (pickClicked)
    {
        if (!texturesScanned)
        {
            textures.refresh();
            texturesScanned = true;
        }
        textureFilter[0] = '\0';
        ImGui::OpenPopup("##particle_texture_picker");
    }

    bool changed = false;
    std::string chosen;
    if (ImGui::BeginPopup("##particle_texture_picker"))
    {
        size_t particleCount = 0;
        for (const TextureEntry& entry : textures.entries())
        {
            std::string rel = entry.displayName;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (rel.rfind("Particles/", 0) == 0)
                ++particleCount;
        }

        ImGui::TextDisabled("Particles · %zu 张 · %s", particleCount,
                            textures.scannedRoot().c_str());
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##particle_tex_filter", "过滤…", textureFilter,
                                 IM_ARRAYSIZE(textureFilter));
        if (ImGui::Button("重新扫描"))
            textures.refresh();

        ImGui::BeginChild("##particle_tex_list", ImVec2(360.0f, 260.0f));
        std::string filter = textureFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        size_t listed = 0;
        for (const TextureEntry& entry : textures.entries())
        {
            std::string rel = entry.displayName;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (rel.rfind("Particles/", 0) != 0)
                continue;
            if (!filter.empty())
            {
                std::string lower = rel;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find(filter) == std::string::npos)
                    continue;
            }
            ++listed;
            const bool selected = rel == currentGeneric;
            if (ImGui::Selectable(rel.c_str(), selected))
                chosen = rel;
        }
        if (listed == 0)
            ImGui::TextDisabled("没有匹配的粒子贴图。");
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    if (!chosen.empty())
    {
        value->value = scene::PropertyValue::fromString(chosen);
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

void InspectorPanel::drawRenderTag(EditorState& state, entt::entity entity)
{
    entt::registry& registry = *state.services.registry;
    auto* tag = registry.try_get<RenderTagComponent>(entity);
    if (!tag)
        return;

    if (!ImGui::CollapsingHeader("Render Tag"))
        return;

    // E3：Render Tag 也走命令栈（它直接决定这一帧怎么画，不是纯元数据）。
    bool skybox = tag->isSkybox;
    bool grid = tag->isGrid;
    ImGui::Checkbox("Skybox", &skybox);
    ImGui::Checkbox("Grid", &grid);
    if (tag->isTerrain)
        ImGui::TextDisabled("Terrain: vertex-color mode (geometry pipeline)");
    if (skybox != tag->isSkybox || grid != tag->isGrid)
    {
        state.commands.execute(std::make_unique<SetRenderTagCommand>(
            registry, entity, tag->isSkybox, tag->isGrid, skybox, grid));
        state.markDirty();
    }

    drawNativeComponentFooter(state, entity, "native.renderTag", "Render Tag");
}

} // namespace editor
