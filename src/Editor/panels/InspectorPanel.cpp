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
bool drawPropertyWidgets(scene::ComponentRecord& record,
                         const scene::ComponentDescriptor& descriptor)
{
    bool changed = false;
    for (const scene::PropertyDescriptor& property : descriptor.properties)
    {
        scene::NamedProperty* value = record.find(property.name);
        if (value == nullptr)
            continue;

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
            if (ImGui::DragInt(property.label.c_str(), &v, 0.1f))
            {
                value->value = scene::PropertyValue::fromInt(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Float:
        {
            float v = value->value.asFloat();
            if (ImGui::DragFloat(property.label.c_str(), &v, 0.01f, 0.0f, 0.0f, "%.3f"))
            {
                value->value = scene::PropertyValue::fromFloat(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Vec3:
        {
            glm::vec3 v = value->value.asVec3();
            if (ImGui::DragFloat3(property.label.c_str(), glm::value_ptr(v),
                                  0.01f, 0.0f, 0.0f, "%.3f"))
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
    }
    return changed;
}

// 原生组件由固定面板绘制，这里补一个统一的移除入口（同样走命令栈）。
void drawNativeRemoveFooter(EditorState& state, entt::entity entity, const char* type,
                            const char* label)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || state.services.registry == nullptr)
        return;

    entt::registry& registry = *state.services.registry;
    const scene::ComponentDescriptor* descriptor = scene->getComponentRegistry().find(type);
    if (descriptor == nullptr || !descriptor->removable)
        return;

    if (descriptor->canRemove && !descriptor->canRemove(registry, entity))
    {
        ImGui::TextDisabled("%s is managed by its creation flow.", label);
        return;
    }

    ImGui::Separator();
    ImGui::PushID(type);
    if (ImGui::Button("Remove Component"))
    {
        std::optional<scene::ComponentRecord> before = scene->readComponent(entity, type);
        if (before)
        {
            state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                *scene, entity, type, std::move(before), std::nullopt,
                "Remove Component"));
            state.markDirty();
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
        ImGui::TextDisabled("没有选中对象。");
        ImGui::TextDisabled("在 Hierarchy 里点一个对象，或用 Assets 双击模型加进来。");
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
    drawCharacter(state, entity);
    drawTerrain(state, entity);
    drawStreamingTerrain(state, entity);
    drawMesh(state, entity);
    drawMaterial(state, entity);
    drawRenderTag(state, entity);
    drawComponents(state, entity);

    ImGui::End();
}

void InspectorPanel::drawComponents(EditorState& state, entt::entity entity)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || state.services.registry == nullptr)
        return;

    entt::registry& registry = *state.services.registry;
    scene::ComponentRegistry& components = scene->getComponentRegistry();
    if (components.empty())
        return;

    ImGui::Separator();
    ImGui::TextDisabled("ECS Components");

    for (const scene::ComponentDescriptor& descriptor : components.descriptors())
    {
        if (!descriptor.has(registry, entity))
            continue;
        // 原生组件由固定面板绘制（Transform / Character / Render Tag…），
        // 这里只负责元数据扩展组件，避免同一份字段出现两次。
        if (!descriptor.drawProperties)
            continue;

        ImGui::PushID(descriptor.type.c_str());
        const bool open = ImGui::CollapsingHeader(descriptor.label.c_str());

        // 右键移除。移除和添加都走同一条 SetComponentRecordCommand：
        // before 有值、after 为空就是删除，撤销时把整份属性写回去。
        if (ImGui::BeginPopupContextItem("##component_menu"))
        {
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
                bool changed = false;

                changed = drawPropertyWidgets(afterRecord, descriptor);

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

    // ---- + Add Component ----
    // 只列 exposed 且当前实体未挂载的组件，按 category 分组。
    if (ImGui::Button("+ Add Component"))
        ImGui::OpenPopup("##add_component");

    if (ImGui::BeginPopup("##add_component"))
    {
        std::string lastCategory;
        bool first = true;
        bool any = false;

        for (const scene::ComponentDescriptor& descriptor : components.descriptors())
        {
            if (!descriptor.exposed || descriptor.has(registry, entity))
                continue;

            if (first || descriptor.category != lastCategory)
            {
                if (!first)
                    ImGui::Separator();
                ImGui::TextDisabled("%s", descriptor.category.c_str());
                lastCategory = descriptor.category;
                first = false;
            }

            const bool addable = descriptor.addable;
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
            if (!addable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", descriptor.unavailableReason.c_str());
            any = true;
        }

        if (!any)
            ImGui::TextDisabled("没有可添加的组件。");
        ImGui::EndPopup();
    }
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
            if (drawPropertyWidgets(after, *descriptor) && !after.equivalentTo(*before))
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
    drawNativeRemoveFooter(state, entity, "native.characterController", "Character Controller");
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

    drawNativeRemoveFooter(state, entity, "native.renderTag", "Render Tag");
}

} // namespace editor
