#include "Editor/panels/InspectorPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <random>

namespace editor
{

namespace
{
void drawTerrainBrushControls(EditorState& state)
{
    if (!ImGui::CollapsingHeader("Sculpt Brush"))
        return;

    ImGui::Checkbox("Enable (Ctrl+LMB)", &state.terrainBrushEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("viewport");

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

        ImGui::TextDisabled("Multi-entity editing is not supported yet;");
        ImGui::TextDisabled("use the Hierarchy panel to pick one entity.");
        ImGui::End();
        return;
    }

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
    drawCharacter(state, entity);
    drawTerrain(state, entity);
    drawStreamingTerrain(state, entity);
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
    const glm::vec3 oldTranslation = transform->getTranslation();
    const glm::vec3 oldRotation = transform->getEuler();
    const glm::vec3 oldScale = transform->getScale();
    // 欧拉角在万向节附近可能不是唯一表示；撤销要回到**原来的四元数**，
    // 不能从 oldRotation 反算（反算会丢掉一个自由度）。
    const glm::quat oldOrientation = transform->getOrientation();

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
    (void)state;
    entt::registry& registry = *state.services.registry;
    const auto* controller = registry.try_get<CharacterControllerComponent>(entity);
    if (!controller)
        return;

    if (!ImGui::CollapsingHeader("Character Controller", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled("Kinematic heightfield controller");
    ImGui::Text("Radius %.2f | Height %.2f | Speed %.2f",
                controller->radius, controller->height, controller->moveSpeed);
    ImGui::Text("Gravity %.2f | Jump %.2f | Slope %.1f deg",
                controller->gravity, controller->jumpSpeed, controller->maxSlopeAngle);
    ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f),
                       "%s | ground %s",
                       controller->grounded ? "Grounded" : "Airborne",
                       controller->groundEntity == entt::null ? "none" : "yes");
    ImGui::TextDisabled("Play mode: WASD move, Space jump.");
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
    if (tag->isTerrain)
        ImGui::TextDisabled("Terrain: vertex-color mode (geometry pipeline)");
    if (changed)
        state.markDirty();
}

} // namespace editor
