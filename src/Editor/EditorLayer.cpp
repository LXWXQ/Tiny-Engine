#include "Editor/EditorLayer.h"
#include "Editor/SceneSerializer.h"
#include "Editor/Commands.h"

#include "Editor/panels/HierarchyPanel.h"
#include "Editor/panels/InspectorPanel.h"
#include "Editor/panels/ConsolePanel.h"
#include "Editor/panels/ViewportPanel.h"
#include "Editor/panels/AssetBrowserPanel.h"
#include "Editor/panels/HistoryPanel.h"
#include "Editor/panels/RenderSettingsPanel.h"
#include "Editor/EditorSceneQuery.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Material.h"

#include <imgui.h>
#include "AssetPaths.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace editor
{

EditorLayer::EditorLayer() = default;
EditorLayer::~EditorLayer() = default;

void EditorLayer::initialize(const EngineServices& services, const std::string& defaultScenePath)
{
    state.services = services;
    state.documentPath = defaultScenePath;
    state.playState = PlayState::Editing;
    state.documentDirty = false;

    // E4-b：本机偏好。读不到就当首次运行，用默认值，不阻塞启动。
    preferencesPath = TINY_EDITOR_CONFIG_PATH;
    {
        std::string preferencesError;
        if (preferences.load(preferencesPath, preferencesError))
            state.pushLog(LogEntry::Level::Info, "[Editor] preferences: " + preferencesPath);
        else
            state.pushLog(LogEntry::Level::Info, "[Editor] preferences default (" + preferencesError + ")");
    }

    createDefaultPanels();

    for (auto& panel : panels)
        panel->onAttach(state);

    // 必须等面板建好之后再套：applyPreferences() 按名字找面板，先套等于没有面板可套。
    applyPreferences();

    state.pushLog(LogEntry::Level::Info, "[Editor] initialized");
    if (!defaultScenePath.empty())
        state.pushLog(LogEntry::Level::Info, "[Editor] scene: " + defaultScenePath);

    initialized = true;
    active = true;
}

void EditorLayer::createDefaultPanels()
{
    // 加新面板只需要在这里 +1 行，然后在 drawPanels()/View 菜单里按名字引用。
    registerPanel<ViewportPanel>();
    registerPanel<HierarchyPanel>();
    registerPanel<AssetBrowserPanel>();
    registerPanel<InspectorPanel>();
    registerPanel<ConsolePanel>();
    // E4-d：撤销历史。放在右栏下半部分，和 Inspector 一起构成"属性 + 历史"。
    registerPanel<HistoryPanel>();
    // E5-b：渲染设置。浮动工具窗（不参与固定布局），默认可见。
    registerPanel<RenderSettingsPanel>();

}

EditorPanel* EditorLayer::findPanel(const char* panelName) const
{
    const auto it = panelsByName.find(panelName);
    return it == panelsByName.end() ? nullptr : it->second;
}

void EditorLayer::beginFrame(float dt, bool viewportHovered)
{
    if (!initialized || !active)
        return;

    lastFrameDelta = dt;
    state.viewportHovered = viewportHovered;

    // ImGui 已经算出"这次输入该归 UI 还是归 3D 场景"。
    // 用它来门控快捷键，避免"在 InputText 里打字触发 Delete 删除实体"这种事故。
    ImGuiIO& io = ImGui::GetIO();
    state.wantsKeyboard = io.WantTextInput;

    updateWindowTitle();

    handleShortcuts();
    updatePlayState(dt);
}

namespace
{
// E1：多选批量操作。地形 chunk / 流送管理器不参与批量删除与复制：
// 前者由流送系统回收，后者是整块高度场的宿主。
bool isBatchExcluded(entt::registry& registry, entt::entity entity)
{
    return registry.any_of<TerrainChunkComponent>(entity) ||
           registry.any_of<TerrainStreamingComponent>(entity);
}

// 只保留"父节点没有被同时选中"的根，否则同一棵子树会被删两次。
std::vector<entt::entity> selectionRoots(EditorState& state)
{
    entt::registry& registry = *state.services.registry;
    const std::vector<entt::entity>& selected = state.selection.entities;
    std::vector<entt::entity> roots;
    for (entt::entity entity : selected)
    {
        if (!registry.valid(entity) || isBatchExcluded(registry, entity))
            continue;

        bool coveredByAncestor = false;
        const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
        while (hierarchy != nullptr && hierarchy->parent != entt::null)
        {
            const entt::entity parent = hierarchy->parent;
            if (std::find(selected.begin(), selected.end(), parent) != selected.end())
            {
                coveredByAncestor = true;
                break;
            }
            hierarchy = registry.try_get<HierarchyComponent>(parent);
        }
        if (!coveredByAncestor)
            roots.push_back(entity);
    }
    return roots;
}

void deleteSelection(EditorState& state)
{
    const std::vector<entt::entity> roots = selectionRoots(state);
    if (roots.empty())
        return;

    const entt::entity cameraEntity = state.services.scene->getCameraEntity();
    std::vector<std::unique_ptr<Command>> children;
    for (entt::entity entity : roots)
    {
        if (entity == cameraEntity)
            continue;
        children.push_back(std::make_unique<DeleteEntityCommand>(
            *state.services.registry, entity, state.services.scene));
    }
    if (children.empty())
        return;

    state.selection.clear();
    state.commands.execute(std::make_unique<CompositeCommand>("Delete Entities", std::move(children)));
    state.markDirty();
}

void duplicateSelection(EditorState& state)
{
    const std::vector<entt::entity> roots = selectionRoots(state);
    if (roots.empty())
        return;

    std::vector<std::unique_ptr<Command>> children;
    std::vector<DuplicateEntityCommand*> raw;
    for (entt::entity entity : roots)
    {
        if (!DuplicateEntityCommand::canDuplicate(*state.services.registry, entity))
            continue;
        children.push_back(std::make_unique<DuplicateEntityCommand>(
            *state.services.registry, state.services.scene, entity));
        raw.push_back(static_cast<DuplicateEntityCommand*>(children.back().get()));
    }
    if (children.empty())
        return;

    const bool single = children.size() == 1;
    if (single)
    {
        DuplicateEntityCommand* command = raw.front();
        state.commands.execute(std::move(children.front()));
        state.selection.select(command->duplicatedRoot());
    }
    else
    {
        state.commands.execute(std::make_unique<CompositeCommand>("Duplicate Entities", std::move(children)));
        state.selection.clear();
        for (DuplicateEntityCommand* command : raw)
        {
            if (command->duplicatedRoot() != entt::null)
                state.selection.add(command->duplicatedRoot());
        }
    }
    state.markDirty();
}

// F：把相机沿当前视线退到"选中集包围球刚好占满视野"的位置，方向不变。
void focusSelection(EditorState& state)
{
    entt::registry& registry = *state.services.registry;
    const std::vector<entt::entity>& selected = state.selection.entities;

    bool hasBounds = false;
    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };
    for (entt::entity entity : selected)
    {
        if (!registry.valid(entity))
            continue;
        if (const auto* bounds = registry.try_get<WorldBoundsComponent>(entity);
            bounds != nullptr && bounds->world.valid)
        {
            boundsMin = hasBounds ? glm::min(boundsMin, bounds->world.min) : bounds->world.min;
            boundsMax = hasBounds ? glm::max(boundsMax, bounds->world.max) : bounds->world.max;
            hasBounds = true;
        }
        else if (const auto* world = registry.try_get<WorldTransformComponent>(entity))
        {
            const glm::vec3 point = world->worldPosition;
            boundsMin = hasBounds ? glm::min(boundsMin, point) : point;
            boundsMax = hasBounds ? glm::max(boundsMax, point) : point;
            hasBounds = true;
        }
    }
    if (!hasBounds)
        return;

    const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    const float radius = glm::max(glm::length(boundsMax - boundsMin) * 0.5f, 1.0f);
    const float distance = radius * 3.0f + 2.0f;

    TransformComponent& camera = state.services.scene->getCameraTransform();
    const glm::vec3 euler = camera.getEuler();
    const float cosPitch = std::cos(euler.x);
    const glm::vec3 forward{ std::sin(euler.y) * cosPitch, -std::sin(euler.x), std::cos(euler.y) * cosPitch };

    camera.setTranslation(center - forward * distance);
    state.transformRefreshPending = true;
    state.pushLog(LogEntry::Level::Info, "[Editor] focused selection | radius=" + std::to_string(radius));
}

// E4-c：快捷键速查表。菜单、弹窗、自检都读这一张表 ——
// 加一个快捷键时只改这里 + 真正处理它的地方，不会出现"文档写了但没实现"。
struct ShortcutEntry
{
    const char* group;
    const char* keys;
    const char* action;
};

const ShortcutEntry kShortcutEntries[] =
{
    { "文档", "Ctrl+N", "新建场景（有未保存修改时先确认）" },
    { "文档", "Ctrl+O", "打开场景" },
    { "文档", "Ctrl+S", "保存" },
    { "文档", "Ctrl+Z", "撤销" },
    { "文档", "Ctrl+Shift+Z", "重做（同 Ctrl+Y）" },
    { "文档", "Ctrl+Y", "重做" },
    { "选择", "Ctrl+A", "全选（跳过地形 chunk 与相机）" },
    { "选择", "Ctrl+D", "复制选中（含子树）" },
    { "选择", "Delete", "删除选中" },
    { "选择", "F", "把相机聚焦到选中" },
    { "变换", "W / E / R", "移动 / 旋转 / 缩放（多选时整组一起变）" },
    { "播放", "F5", "开始播放" },
    { "播放", "F6", "暂停 / 继续" },
    { "播放", "F7", "停止并恢复快照" },
    { "视图", "?", "打开 / 关闭本速查表" },
    { "视图", "Esc", "关闭速查表" },
};

constexpr size_t kShortcutEntryCount = sizeof(kShortcutEntries) / sizeof(kShortcutEntries[0]);
} // namespace

void EditorLayer::handleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return;      // 正在输入文字，快捷键全部让路

    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        if (shift)
        {
            if (state.commands.canRedo())
            {
                state.commands.redo();
                state.markDirty();
            }
        }
        else if (state.commands.canUndo())
        {
            state.commands.undo();
            state.markDirty();
        }
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    {
        if (state.commands.canRedo())
        {
            state.commands.redo();
            state.markDirty();
        }
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        saveScene();
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
    {
        requestDocumentAction(EditorState::PendingDocumentAction::New);
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
    {
        requestDocumentAction(EditorState::PendingDocumentAction::Open);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_W, false))
    {
        state.gizmo.mode = GizmoMode::Translate;
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
    {
        state.gizmo.mode = GizmoMode::Rotate;
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    {
        state.gizmo.mode = GizmoMode::Scale;
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        // E1：删除整个选中集合（父节点被选中时子节点自动跳过，避免重复删除）。
        deleteSelection(state);
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
    {
        duplicateSelection(state);
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
    {
        // 只选"有名字、不是流送 chunk"的实体，避免 Ctrl+A 把地形块全部选中。
        entt::registry& registry = *state.services.registry;
        const entt::entity cameraEntity = state.services.scene->getCameraEntity();
        state.selection.clear();
        for (auto [entity, name] : registry.view<NameComponent>().each())
        {
            (void)name;
            if (entity == cameraEntity || isBatchExcluded(registry, entity))
                continue;
            state.selection.add(entity);
        }
        state.pushLog(LogEntry::Level::Info,
                      "[Editor] selected " + std::to_string(state.selection.size()) + " entities");
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        focusSelection(state);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && shift)
    {
        // ? 是 Shift+/，要求 shift 是为了不抢单独的 /。
        showShortcutSheet = !showShortcutSheet;
    }
    else if (showShortcutSheet && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        showShortcutSheet = false;
    }
}

void EditorLayer::updatePlayState(float dt)
{
    if (state.playState != PlayState::Playing ||
        !state.services.scene || !state.services.registry || dt <= 0.0f)
        return;

    entt::registry& registry = *state.services.registry;
    entt::entity controlled = state.selection.primary();
    if (controlled == entt::null || !registry.valid(controlled) ||
        !registry.try_get<CharacterControllerComponent>(controlled))
    {
        controlled = entt::null;
        for (auto [entity, controller] : registry.view<CharacterControllerComponent>().each())
        {
            (void)controller;
            controlled = entity;
            break;
        }
    }
    if (controlled == entt::null)
        return;

    ImGuiIO& io = ImGui::GetIO();
    glm::vec3 move{ 0.0f };
    if (!io.WantTextInput)
    {
        const float yaw = state.services.scene->getCameraTransform().getEuler().y;
        const glm::vec3 forward{ std::sin(yaw), 0.0f, std::cos(yaw) };
        const glm::vec3 right{ std::cos(yaw), 0.0f, -std::sin(yaw) };

        if (ImGui::IsKeyDown(ImGuiKey_W)) move += forward;
        if (ImGui::IsKeyDown(ImGuiKey_S)) move -= forward;
        if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
    }

    const bool jump = !io.WantTextInput &&
                      ImGui::IsKeyPressed(ImGuiKey_Space, false);
    CharacterMoveResult result;
    if (state.services.scene->updateCharacterController(
            controlled, move, jump, dt, result))
    {
        state.transformRefreshPending = true;
    }
}

void EditorLayer::drawUI()
{
    if (!initialized || !active)
        return;

    // 菜单栏必须用 ImGui 的主视口尺寸铺满顶部。
    // 没有 docking 分支时，这是把菜单栏固定在窗口顶部的标准做法。
    if (ImGui::BeginMainMenuBar())
    {
        drawMenuBar();
        ImGui::EndMainMenuBar();
    }

    drawToolbar();

    drawPanels();
    // BeginMainMenuBar 在第一帧还没把主视口的 WorkPos 下推（此时 WorkPos.y == 0），
    // 这一帧算出来的面板矩形会整体偏高一个菜单栏高度。所以布局标志要多留一帧：
    // 等工作区稳定之后再落位，否则第一帧的错位置会被 imgui.ini 记住。
    if (ImGui::GetMainViewport()->WorkPos.y > 0.5f)
        layoutNeedsReset = false;

    drawStatusBar();

    drawShortcutSheet();

    // 未保存确认放在所有面板之后：它要盖在最上层，且本帧的改动要先算进 dirty。
    drawUnsavedPrompt();

    // 本帧有面板改过变换 -> 在提交渲染之前把世界变换与包围盒算准。
    // 放在这里而不是各个面板里，是为了"多个面板同帧改动只刷新一次"。
    if (state.transformRefreshPending)
    {
        state.transformRefreshPending = false;
        if (state.services.scene)
            state.services.scene->refreshTransformsAndVisibility();
    }
}

void EditorLayer::drawMenuBar()
{
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::BeginMenu("File"))
    {
        const bool writable = state.documentWritable();
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, writable))
            requestDocumentAction(EditorState::PendingDocumentAction::New);
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, writable))
            requestDocumentAction(EditorState::PendingDocumentAction::Open);
        if (ImGui::BeginMenu("Open Recent", writable && !state.recentScenes.empty()))
        {
            for (const std::string& recent : state.recentScenes)
            {
                const std::string label = std::filesystem::path(recent).filename().string();
                if (ImGui::MenuItem(label.c_str(), nullptr, false, writable))
                    requestOpenScene(recent);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", recent.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, writable)) saveScene();
        if (ImGui::MenuItem("Save As...", nullptr, false, writable)) saveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Reload Config") && state.services.reloadConfig)
            state.services.reloadConfig();
        if (ImGui::MenuItem("Save Config") && state.services.saveConfig)
            state.services.saveConfig();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
            requestDocumentAction(EditorState::PendingDocumentAction::Exit);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem(state.commands.canUndo() ? "Undo" : "Undo (nothing)", "Ctrl+Z",
                            false, state.commands.canUndo()))
        {
            state.commands.undo();
            state.markDirty();
        }
        if (ImGui::MenuItem(state.commands.canRedo() ? "Redo" : "Redo (nothing)", "Ctrl+Y",
                            false, state.commands.canRedo()))
        {
            state.commands.redo();
            state.markDirty();
        }
        ImGui::Separator();
        ImGui::TextDisabled("History: %d undo / %d redo",
                            static_cast<int>(state.commands.undoCount()),
                            static_cast<int>(state.commands.redoCount()));
        ImGui::Separator();
        if (ImGui::MenuItem("Move (W)", nullptr, state.gizmo.mode == GizmoMode::Translate))
            state.gizmo.mode = GizmoMode::Translate;
        if (ImGui::MenuItem("Rotate (E)", nullptr, state.gizmo.mode == GizmoMode::Rotate))
            state.gizmo.mode = GizmoMode::Rotate;
        if (ImGui::MenuItem("Scale (R)", nullptr, state.gizmo.mode == GizmoMode::Scale))
            state.gizmo.mode = GizmoMode::Scale;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create"))
    {
        if (ImGui::MenuItem("Terrain"))
            createTerrainFromUi();
        if (ImGui::MenuItem("Character"))
            createCharacterFromUi();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Play"))
    {
        const bool editing = (state.playState == PlayState::Editing);
        const bool playing = (state.playState == PlayState::Playing);
        const bool paused = (state.playState == PlayState::Paused);

        if (ImGui::MenuItem("Play", "F5", false, editing))
            enterPlay();
        if (ImGui::MenuItem("Pause", "F6", false, playing))
            pausePlay();
        if (ImGui::MenuItem("Resume", "F6", false, paused))
            resumePlay();
        if (ImGui::MenuItem("Stop", "F7", false, !editing))
            stopPlay();
        ImGui::Separator();
        ImGui::TextDisabled("State: %s",
                            editing ? "Editing" : (playing ? "Playing" : "Paused"));
        ImGui::EndMenu();

        if (ImGui::IsKeyPressed(ImGuiKey_F5) && editing) enterPlay();
        if (ImGui::IsKeyPressed(ImGuiKey_F6))
        {
            if (playing) pausePlay();
            else if (paused) resumePlay();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F7) && !editing) stopPlay();
    }

    if (ImGui::BeginMenu("View"))
    {
        if (auto* panel = findPanel("Viewport"))
            ImGui::MenuItem("Viewport", nullptr, &panel->visible);
        if (auto* panel = findPanel("Assets"))
            ImGui::MenuItem("Assets", nullptr, &panel->visible);
        if (auto* panel = findPanel("Hierarchy"))
            ImGui::MenuItem("Hierarchy", nullptr, &panel->visible);
        if (auto* panel = findPanel("Inspector"))
            ImGui::MenuItem("Inspector", nullptr, &panel->visible);
        if (auto* panel = findPanel("Console"))
            ImGui::MenuItem("Console", nullptr, &panel->visible);
        if (auto* panel = findPanel("History"))
            ImGui::MenuItem("History", nullptr, &panel->visible);
        if (auto* panel = findPanel("Render"))
            ImGui::MenuItem("Render Settings", nullptr, &panel->visible);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
            layoutNeedsReset = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help"))
    {
        ImGui::MenuItem("Keyboard Shortcuts", "?", &showShortcutSheet);
        ImGui::Separator();
        ImGui::TextDisabled("Tiny Editor");
        ImGui::TextDisabled("Tiny Engine (Vulkan)");
        ImGui::EndMenu();
    }

    // 右侧只留文档身份。播放状态与帧率在工具栏右侧，帧数据在底部状态栏，
    // 撤销深度在 History 面板 —— 同一件事写三处就是噪音，而且改动时容易漏改一处。
    const std::string documentName = std::filesystem::path(state.documentPath).filename().string();
    char documentLabel[192];
    std::snprintf(documentLabel, sizeof documentLabel, "%s%s%s",
                  state.documentDirty ? "* " : "",
                  documentName.empty() ? "untitled" : documentName.c_str(),
                  state.documentDirty ? " (unsaved)" : "");
    const float rightWidth = ImGui::CalcTextSize(documentLabel).x + 12.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth);
    if (state.documentDirty)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", documentLabel);
    else
        ImGui::TextDisabled("%s", documentLabel);
}

entt::entity EditorLayer::createCharacterFromUi()
{
    if (!state.services.scene || !state.services.registry)
        return entt::null;

    glm::vec3 spawn = state.services.scene->getCameraTransform().getTranslation();
    float ground = 0.0f;
    if (state.services.scene->sampleTerrainGroundHeightWorld(spawn.x, spawn.z, ground))
        spawn.y = ground;

    auto command = std::make_unique<CreateCharacterCommand>(
        *state.services.registry, *state.services.scene, "Character", spawn);
    CreateCharacterCommand* raw = command.get();
    state.commands.execute(std::move(command));
    const entt::entity entity = raw->currentEntity();
    if (entity == entt::null || !state.services.registry->valid(entity))
    {
        state.pushLog(LogEntry::Level::Error, "[Editor] create character failed");
        return entt::null;
    }

    state.selection.select(entity);
    state.transformRefreshPending = true;
    state.markDirty();
    state.pushLog(LogEntry::Level::Info, "[Editor] created Character (kinematic controller)");
    return entity;
}

entt::entity EditorLayer::createTerrainFromUi()
{
    if (!state.services.scene || !state.services.registry)
        return entt::null;

    terrain::TerrainConfig config;
    const entt::entity entity = createTerrainThroughCommand(
        *state.services.registry, *state.services.scene, state.commands,
        config, "Terrain");

    if (entity == entt::null)
    {
        state.pushLog(LogEntry::Level::Error, "[Editor] create terrain failed");
        return entt::null;
    }

    state.selection.select(entity);
    state.markDirty();
    if (const auto* terrain = state.services.registry->try_get<TerrainComponent>(entity))
    {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "[Editor] created Terrain | %ux%u, vertices=%u, indices=%u, %.2f ms",
                      terrain->config.resolutionX, terrain->config.resolutionZ,
                      terrain->vertexCount, terrain->indexCount,
                      terrain->lastGenerationMs);
        state.pushLog(LogEntry::Level::Info, buffer);
    }
    return entity;
}
namespace
{
// E5：工具栏与状态栏的固定高度。面板默认矩形、视口矩形都要减掉它们，
// 所以必须是常量 —— 让它们按内容自适应的话，视口会跟着文字宽度抖动。
constexpr float kToolbarHeight = 34.0f;
constexpr float kStatusBarHeight = 26.0f;

const ImGuiWindowFlags kBarFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoNav;

// 一枚"当前生效"按钮：生效时用按下配色，鼠标停上去给说明。
// 用法上它自己吃掉一个 SameLine，所以调用后可以直接画下一个。
bool barToggle(const char* label, bool active, const char* tooltip)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool pressed = ImGui::Button(label);
    if (active)
        ImGui::PopStyleColor(2);
    if (tooltip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::SameLine();
    return pressed;
}

const char* gizmoModeLabel(GizmoMode mode)
{
    return (mode == GizmoMode::Translate) ? "移动"
         : (mode == GizmoMode::Rotate) ? "旋转" : "缩放";
}
} // namespace

void EditorLayer::drawToolbar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 注意：BeginMainMenuBar 已经把主视口的 WorkPos 下推了菜单栏高度，
    // 这里再减一次就会把工具栏顶到面板底下去。
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, kToolbarHeight), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 4.0f));
    if (ImGui::Begin("##toolbar", nullptr, kBarFlags))
    {
        // ---- 播放 ----
        const bool editing = state.playState == PlayState::Editing;
        const bool playing = state.playState == PlayState::Playing;

        // 按钮上的字刻意用英文：缺中文字形时那种字体会退化成 ?，
        // 而工具栏是"不读说明也点得对"的东西，不能赌字体加载成功。
        if (ImGui::Button(editing ? "Play" : (playing ? "Pause" : "Resume")))
        {
            if (editing)
                enterPlay();
            else if (playing)
                pausePlay();
            else
                resumePlay();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("F5 播放 / F6 暂停恢复");
        ImGui::SameLine();

        ImGui::BeginDisabled(editing);
        if (ImGui::Button("Stop"))
            stopPlay();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("F7 停止并恢复播放前的场景");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // ---- 手柄模式 ----
        if (barToggle("Move", state.gizmo.mode == GizmoMode::Translate, "W：移动手柄"))
            state.gizmo.mode = GizmoMode::Translate;
        if (barToggle("Rotate", state.gizmo.mode == GizmoMode::Rotate, "E：旋转手柄"))
            state.gizmo.mode = GizmoMode::Rotate;
        if (barToggle("Scale", state.gizmo.mode == GizmoMode::Scale, "R：缩放手柄"))
            state.gizmo.mode = GizmoMode::Scale;

        // ---- 吸附 ----
        ImGui::Checkbox("Snap", &state.gizmo.snapEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("平移 %.2f / 旋转 %.0f 度 / 缩放 %.2f；按住 Shift 可临时吸附",
                              state.gizmo.snapTranslate, state.gizmo.snapRotateDegrees,
                              state.gizmo.snapScale);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // ---- 渲染设置（浮动工具窗的开关） ----
        if (auto* renderPanel = findPanel("Render"))
        {
            if (barToggle("Render", renderPanel->visible, "渲染设置：曝光 / Bloom / SSAO / 方向光"))
                renderPanel->visible = !renderPanel->visible;
        }

        // ---- 聚焦 / 撤销 ----
        ImGui::BeginDisabled(state.selection.size() == 0);
        if (ImGui::Button("Focus"))
            focusSelection(state);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("F：把相机退到让选中对象占满视野的位置");
        ImGui::SameLine();

        ImGui::BeginDisabled(!state.commands.canUndo());
        if (ImGui::Button("Undo"))
        {
            state.commands.undo();
            state.markDirty();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ctrl+Z");
        ImGui::SameLine();
        ImGui::BeginDisabled(!state.commands.canRedo());
        if (ImGui::Button("Redo"))
        {
            state.commands.redo();
            state.markDirty();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ctrl+Y");
        ImGui::SameLine();

        const char* playLabel = editing ? "Editing" : (playing ? "Playing" : "Paused");
        char right[192];
        std::snprintf(right, sizeof right, "%s | %s | %.0f FPS",
                      state.documentDirty ? "* unsaved" : "saved", playLabel,
                      static_cast<double>(ImGui::GetIO().Framerate));
        const float textWidth = ImGui::CalcTextSize(right).x;
        const float targetX = ImGui::GetWindowWidth() - textWidth - 10.0f;
        if (targetX > ImGui::GetCursorPosX())
            ImGui::SameLine(targetX);
        if (state.documentDirty)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", right);
        else
            ImGui::TextDisabled("%s", right);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void EditorLayer::drawStatusBar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barY = viewport->WorkPos.y + viewport->WorkSize.y - kStatusBarHeight;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, barY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, kStatusBarHeight), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
    if (ImGui::Begin("##statusbar", nullptr, kBarFlags))
    {
        const size_t selected = state.selection.size();
        if (selected == 0)
            ImGui::TextDisabled("没有选中对象");
        else
            ImGui::Text("%zu 个对象选中", selected);
        ImGui::SameLine();
        ImGui::TextDisabled("| %s | 吸附 %s", gizmoModeLabel(state.gizmo.mode),
                            state.gizmo.snapEnabled ? "开" : "关（按住 Shift 临时开）");

        // 帧数据放这里而不是只塞进 Statistics 面板：调场景时眼睛一直在视口，
        // 侧栏是要"转头去看"的，状态栏是余光就能扫到的。
        const ImGuiIO& io = ImGui::GetIO();
        unsigned int draws = 0;
        unsigned int tris = 0;
        if (state.services.scene != nullptr)
        {
            const RenderTelemetry& telemetry = state.services.scene->getTelemetry();
            draws = telemetry.drawCalls;
            tris = telemetry.triangles;
        }
        char right[192];
        std::snprintf(right, sizeof right, "%.0f FPS (%.2f ms) | %u draws | %u tris | undo %d",
                      static_cast<double>(io.Framerate),
                      (io.Framerate > 0.0f) ? (1000.0f / io.Framerate) : 0.0f,
                      draws, tris, static_cast<int>(state.commands.undoCount()));
        const float textWidth = ImGui::CalcTextSize(right).x;
        const float targetX = ImGui::GetWindowWidth() - textWidth - 10.0f;
        if (targetX > ImGui::GetCursorPosX())
            ImGui::SameLine(targetX);
        ImGui::TextDisabled("%s", right);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorLayer::drawPanels()
{
    // 关键约束：ImGui::SetNextWindowPos/Size 是**单槽位**的 ——
    // 连续调用多次只有最后一次生效，而且会被"下一个 Begin 的窗口"消费。
    // 所以绝不能先把三个面板的位置都设置一遍再统一绘制
    // （那样 Console 的矩形会套到 Hierarchy 上，另外两个面板退回默认位置）。
    // 正确做法：在**每个面板 Begin 之前紧邻**设置它自己的矩形。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    const float leftWidth = 280.0f;
    const float rightWidth = 340.0f;
    const float bottomHeight = 200.0f;

    const float top = viewport->WorkPos.y + kToolbarHeight;
    const float availableWidth = viewport->WorkSize.x;
    const float availableHeight = viewport->WorkSize.y - kToolbarHeight - kStatusBarHeight;

    const ImGuiCond cond = layoutNeedsReset ? ImGuiCond_Always : ImGuiCond_FirstUseEver;

    for (auto& panel : panels)
    {
        if (!panel->visible)
            continue;

        // 按名字给每个内置面板指定默认矩形；未知面板（用户新增的）交给 ImGui 自行摆放。
        const std::string panelName = panel->name();
        if (panelName == "Viewport")
        {
            // 中央区域：左右让给 Hierarchy / Inspector，底部让给 Console。
            //
            // 用 ImGuiCond_Always 而不是 cond：视口是"计算出来的固定区域"，
            // 不应该被用户拖动或记住历史位置。如果沿用 FirstUseEver，
            // 一旦 imgui.ini 里存了错误位置（比如误拖过），就会一直错下去。
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + leftWidth, top), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(availableWidth - leftWidth - rightWidth,
                                            availableHeight - bottomHeight), ImGuiCond_Always);
        }
        else if (panelName == "Assets")
        {
            // 左栏下半部分。Hierarchy 占上 60%，Assets 占下 40%。
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                           top + availableHeight * 0.6f), cond);
            ImGui::SetNextWindowSize(ImVec2(leftWidth, availableHeight * 0.4f), cond);
        }
        else if (panelName == "Hierarchy")
        {
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, top), cond);
            ImGui::SetNextWindowSize(ImVec2(leftWidth, availableHeight * 0.6f), cond);
        }
        else if (panelName == "Inspector")
        {
            // 右栏上 65%：属性。剩下 35% 留给 History。
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + availableWidth - rightWidth, top), cond);
            ImGui::SetNextWindowSize(ImVec2(rightWidth, availableHeight * 0.65f), cond);
        }
        else if (panelName == "History")
        {
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + availableWidth - rightWidth,
                                           top + availableHeight * 0.65f), cond);
            ImGui::SetNextWindowSize(ImVec2(rightWidth, availableHeight * 0.35f), cond);
        }
        else if (panelName == "Console")
        {
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + leftWidth,
                                           top + availableHeight - bottomHeight), cond);
            ImGui::SetNextWindowSize(ImVec2(availableWidth - leftWidth - rightWidth, bottomHeight), cond);
        }

        panel->draw(state);
    }

    // 记录"没被面板遮住的中央区域"，供后续做离屏视口时使用。
    // 现在 3D 画面铺满整个交换链，这块矩形只是给扩展留的锚点。
    state.viewportX = viewport->WorkPos.x + leftWidth;
    state.viewportY = top;
    state.viewportWidth = availableWidth - leftWidth - rightWidth;
    state.viewportHeight = availableHeight - bottomHeight;
}

void EditorLayer::endFrame()
{
    if (!initialized || !active)
        return;
    state.scrollLogToBottom = false;

    // E4-b：偏好不逐帧写盘。先同步一次当前状态，变化后等 0.75s 再落盘，
    // 这样连点 View 菜单里的开关只会产生一次 IO。
    syncPreferencesFromState();
    if (preferencesDirty)
    {
        preferencesSaveDelay += ImGui::GetIO().DeltaTime;
        if (preferencesSaveDelay >= 0.75f)
            savePreferences();
    }
}

// ---------------------------------------------------------------------------
// 场景文档操作
// ---------------------------------------------------------------------------
bool EditorLayer::newScene()
{
    if (!state.documentWritable())
    {
        state.pushLog(LogEntry::Level::Warning, "[Editor] new scene blocked: 播放中不重置场景（按 F7 停止）");
        return false;
    }

    if (!state.services.scene)
        return false;

    state.services.scene->clearEntities();
    state.selection.clear();
    state.commands.clear();
    state.documentPath.clear();
    state.documentDirty = false;
    state.pushLog(LogEntry::Level::Info, "[Editor] new scene");
    return true;
}

bool EditorLayer::saveScene()
{
    // 播放期间场景是运行期状态，写盘会把玩家跑出来的位姿存进文档。
    if (!state.documentWritable())
    {
        state.pushLog(LogEntry::Level::Warning, "[Editor] save blocked: 播放中不写入文档（按 F7 停止后保存）");
        return false;
    }

    if (state.documentPath.empty())
        return saveSceneAs();

    if (!state.services.scene)
        return false;

    const scene::SceneDescription description = state.services.scene->captureDescription();
    std::string error;
    if (!SceneSerializer::saveToFile(description, state.documentPath, error))
    {
        state.pushLog(LogEntry::Level::Error, "[Editor] save failed: " + error);
        return false;
    }

    state.documentDirty = false;
    state.pushLog(LogEntry::Level::Info, "[Editor] saved: " + state.documentPath);
    return true;
}

bool EditorLayer::saveSceneAs()
{
    if (!state.services.saveFileDialog)
        return false;

    const std::string path = state.services.saveFileDialog(
        "Tiny Engine Scene (*.scene.json)\0*.scene.json\0All Files (*.*)\0*.*\0",
        "Save Scene As");
    if (path.empty())
        return false;

    state.documentPath = path;
    return saveScene();
}

bool EditorLayer::openSceneDialog()
{
    if (!state.services.openFileDialog)
        return false;

    const std::string path = state.services.openFileDialog(
        "Tiny Engine Scene (*.scene.json)\0*.scene.json\0All Files (*.*)\0*.*\0",
        "Open Scene");
    if (path.empty())
        return false;

    return openScene(path);
}

bool EditorLayer::openScene(const std::string& path)
{
    std::string error;
    if (!loadSceneFromPath(path, error))
    {
        state.pushLog(LogEntry::Level::Error, "[Editor] open failed: " + error);
        return false;
    }
    return true;
}

bool EditorLayer::loadSceneFromPath(const std::string& path, std::string& errorMessage, bool recordRecent)
{
    if (!state.documentWritable())
    {
        errorMessage = "播放中不能打开场景：先按 F7 停止播放";
        return false;
    }

    if (!state.services.scene)
    {
        errorMessage = "no scene bound";
        return false;
    }

    scene::SceneDescription description;
    if (!SceneSerializer::loadFromFile(path, description, errorMessage))
        return false;

    if (!state.services.scene->buildFromDescription(description, errorMessage))
        return false;

    state.selection.clear();
    state.commands.clear();
    state.documentPath = path;
    state.documentDirty = false;

    if (recordRecent)
    {
        preferences.pushRecent(path);
        state.recentScenes = preferences.recentScenes;
        preferencesDirty = true;
        preferencesSaveDelay = 0.0f;
    }

    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "[Editor] loaded %d entities from %s",
                  static_cast<int>(description.entities.size()), path.c_str());
    state.pushLog(LogEntry::Level::Info, buffer);
    return true;
}

// ---------------------------------------------------------------------------
// 播放状态机
//
// 语义：
//   Editing -> Playing : 拍快照，进入运行态
//   Playing -> Paused  : 只停在当前状态，**不**恢复快照
//   Paused  -> Playing : 继续
//   Playing/Paused -> Editing : 恢复快照（丢弃运行期的所有改动）
//
// 为什么"暂停"不恢复快照：暂停的语义是"停一下"，用户期望再点 Play 能接着跑。
// 如果暂停就恢复快照，那暂停期间看到的东西和回来之后不一样，很难理解。
// ---------------------------------------------------------------------------
void EditorLayer::enterPlay()
{
    if (!state.services.scene || state.playState != PlayState::Editing)
        return;

    // 播放前必须先把"未保存的修改"落进快照，否则 Stop 会丢改动
    state.playSnapshot = state.services.scene->captureDescription();
    state.hasPlaySnapshot = true;
    state.playState = PlayState::Playing;
    state.commands.clear();          // 运行期的操作不该能撤销回编辑态
    state.pushLog(LogEntry::Level::Info, "[Editor] play");
}

void EditorLayer::pausePlay()
{
    if (state.playState != PlayState::Playing)
        return;
    state.playState = PlayState::Paused;
    state.pushLog(LogEntry::Level::Info, "[Editor] paused");
}

void EditorLayer::resumePlay()
{
    if (state.playState != PlayState::Paused)
        return;
    state.playState = PlayState::Playing;
    state.pushLog(LogEntry::Level::Info, "[Editor] resumed");
}

void EditorLayer::stopPlay()
{
    if (state.playState == PlayState::Editing)
        return;

    if (state.hasPlaySnapshot && state.services.scene)
    {
        std::string error;
        if (!state.services.scene->buildFromDescription(state.playSnapshot, error))
            state.pushLog(LogEntry::Level::Error, "[Editor] stop failed to restore: " + error);
    }

    state.hasPlaySnapshot = false;
    state.playSnapshot = scene::SceneDescription{};
    state.playState = PlayState::Editing;
    state.selection.clear();
    state.commands.clear();
    state.pushLog(LogEntry::Level::Info, "[Editor] stopped, scene restored");
}

// ---------------------------------------------------------------------------
// E4：文档安全（未保存确认 + 窗口标题）
//
// 原来 Ctrl+N / Ctrl+O / File->Exit 会静默丢掉未保存的修改，标题栏也从不显示
// 当前文档与脏标记。现在所有"会替换文档"的入口都先经过 requestDocumentAction()：
//   干净 或 播放中 -> 直接执行（播放中由 saveScene/newScene 的守卫拒绝并打日志）
//   有未保存修改   -> 只记下意图，画模态框，用户选完再执行
// 这样"确认"只有一处实现，菜单与快捷键不会各写一套。
// ---------------------------------------------------------------------------
void EditorLayer::requestDocumentAction(EditorState::PendingDocumentAction action)
{
    if (action == EditorState::PendingDocumentAction::None)
        return;

    if (!state.documentDirty || !state.documentWritable())
    {
        performDocumentAction(action);
        return;
    }

    state.pendingDocumentAction = action;
    unsavedPromptRequested = true;
}

void EditorLayer::requestOpenScene(const std::string& path)
{
    if (path.empty())
        return;

    if (!state.documentDirty || !state.documentWritable())
    {
        std::string error;
        if (!loadSceneFromPath(path, error))
            state.pushLog(LogEntry::Level::Error, "[Editor] open failed: " + error);
        return;
    }

    // 有未保存修改：记下目标，等用户在模态框里选完再打开。
    state.pendingDocumentPath = path;
    state.pendingDocumentAction = EditorState::PendingDocumentAction::Open;
    unsavedPromptRequested = true;
}

void EditorLayer::performDocumentAction(EditorState::PendingDocumentAction action)
{
    switch (action)
    {
    case EditorState::PendingDocumentAction::New:
        newScene();
        break;
    case EditorState::PendingDocumentAction::Open:
    {
        // 从 File -> Open Recent 进来时路径已经定了，不要再弹一次对话框。
        const std::string target = state.pendingDocumentPath;
        state.pendingDocumentPath.clear();
        if (target.empty())
        {
            openSceneDialog();
        }
        else
        {
            std::string error;
            if (!loadSceneFromPath(target, error))
                state.pushLog(LogEntry::Level::Error, "[Editor] open failed: " + error);
        }
        break;
    }
    case EditorState::PendingDocumentAction::Exit:
        if (state.services.requestExit)
            state.services.requestExit();
        break;
    case EditorState::PendingDocumentAction::None:
        break;
    }
}

void EditorLayer::cancelPendingDocumentAction()
{
    state.pendingDocumentAction = EditorState::PendingDocumentAction::None;
    unsavedPromptRequested = false;
}

// ---- E4-c：快捷键速查 ------------------------------------------------------
void EditorLayer::drawShortcutSheet()
{
    if (!showShortcutSheet)
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(720.0f, 900.0f));
    if (!ImGui::Begin("Keyboard Shortcuts", &showShortcutSheet, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    const char* currentGroup = nullptr;
    if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        for (const ShortcutEntry& entry : kShortcutEntries)
        {
            if (currentGroup == nullptr || std::strcmp(currentGroup, entry.group) != 0)
            {
                currentGroup = entry.group;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", entry.group);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("");
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.action);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("输入框获得焦点时快捷键会让路，不会误触。");

    ImGui::End();
}

void EditorLayer::drawUnsavedPrompt()
{
    constexpr const char* kPopupId = "未保存的修改";

    if (unsavedPromptRequested)
    {
        ImGui::OpenPopup(kPopupId);
        unsavedPromptRequested = false;
    }

    if (state.pendingDocumentAction == EditorState::PendingDocumentAction::None)
        return;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const std::string name = state.documentPath.empty() ? std::string("untitled") : state.documentPath;
    ImGui::TextUnformatted("当前场景有未保存的修改：");
    ImGui::TextUnformatted(name.c_str());
    ImGui::Separator();

    const EditorState::PendingDocumentAction action = state.pendingDocumentAction;

    if (ImGui::Button("保存并继续", ImVec2(120, 0)))
    {
        // 保存失败（没有路径又取消了另存为对话框）就停在提示框里，
        // 不能把用户的意图吞掉 —— 否则"保存并继续"变成了"什么都没发生"。
        if (saveScene())
        {
            state.pendingDocumentAction = EditorState::PendingDocumentAction::None;
            ImGui::CloseCurrentPopup();
            performDocumentAction(action);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("放弃修改", ImVec2(120, 0)))
    {
        state.documentDirty = false;
        state.pendingDocumentAction = EditorState::PendingDocumentAction::None;
        ImGui::CloseCurrentPopup();
        performDocumentAction(action);
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(120, 0)))
    {
        state.pendingDocumentAction = EditorState::PendingDocumentAction::None;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ---- E4-b：偏好 ------------------------------------------------------------
//
// 单向同步：EditorState 是"现在是什么样子"，EditorPreferences 是"上次存下来的样子"。
// 每帧把 state 投影到 preferences，两者不同才标记脏 + 重置延迟，避免逐帧写盘。

void EditorLayer::applyPreferences()
{
    for (auto& panel : panels)
    {
        if (!panel)
            continue;
        panel->visible = preferences.panelVisible(panel->name());
    }

    switch (preferences.gizmoMode)
    {
    case 1: state.gizmo.mode = GizmoMode::Rotate; break;
    case 2: state.gizmo.mode = GizmoMode::Scale; break;
    default: state.gizmo.mode = GizmoMode::Translate; break;
    }

    state.recentScenes = preferences.recentScenes;
}

void EditorLayer::syncPreferencesFromState()
{
    bool changed = false;

    for (const auto& panel : panels)
    {
        if (!panel)
            continue;
        const std::string name = panel->name();
        if (preferences.panelVisible(name) != panel->visible)
        {
            preferences.setPanelVisible(name, panel->visible);
            changed = true;
        }
    }

    const int mode = static_cast<int>(state.gizmo.mode);
    if (preferences.gizmoMode != mode)
    {
        preferences.gizmoMode = mode;
        changed = true;
    }

    if (!changed)
        return;
    preferencesDirty = true;
    preferencesSaveDelay = 0.0f;
}

void EditorLayer::savePreferences()
{
    if (preferencesPath.empty())
        return;

    std::string error;
    if (!preferences.save(preferencesPath, error))
    {
        state.pushLog(LogEntry::Level::Warning, "[Editor] preferences save failed: " + error);
        preferencesDirty = false;
        preferencesSaveDelay = 0.0f;
        return;
    }

    preferencesDirty = false;
    preferencesSaveDelay = 0.0f;
}

void EditorLayer::updateWindowTitle()
{
    if (!state.services.setWindowTitle)
        return;

    std::string name = "untitled";
    if (!state.documentPath.empty())
        name = std::filesystem::path(state.documentPath).filename().string();

    std::string title = name;
    if (state.documentDirty)
        title += " *";
    title += " - Tiny Editor";

    // 标题每帧都会变 → 只在真正变化时调一次引擎回调，避免每帧 glfwSetWindowTitle。
    if (title == lastWindowTitle)
        return;
    lastWindowTitle = title;
    state.services.setWindowTitle(title);
}


// ---------------------------------------------------------------------------
// D-8 自检
//
// 覆盖三条最容易出错的路径：
//   1. 命令栈：execute / undo / redo 的值语义是否精确（浮点不漂移）
//   2. 删除实体：撤销后子树是否完整重建、父子关系是否恢复
//   3. 场景描述 -> JSON -> 场景描述 的往返是否保真
//
// 不碰文件系统、不建 Vulkan 设备 —— 只依赖 entt 与 nlohmann/json，
// 所以能在一帧内跑完，可以进每次回归。
// ---------------------------------------------------------------------------
bool EditorLayer::selfTest()
{
    entt::registry registry;

    // ---- 0) Gizmo 数学：投影/反投影往返、射线-轴最近点、射线-平面求交 ----
    if (!Gizmo::selfTest())
        return false;

    // ---- 0a) E5-b：渲染设置面板的输入夹取（纯函数） ----
    if (!RenderSettingsPanel::selfTest())
        return false;

    // ---- 0aa) B2：空状态判据（UX-01）。天空盒与相机不算内容，地形 chunk 算。 ----
    {
        entt::registry probe;
        const bool emptyAtStart = isSceneEmpty(probe);

        const entt::entity skybox = probe.create();
        probe.emplace<MeshComponent>(skybox, MeshComponent{});
        RenderTagComponent skyTag{};
        skyTag.isSkybox = true;
        probe.emplace<RenderTagComponent>(skybox, skyTag);
        const bool emptyWithSkyboxOnly = isSceneEmpty(probe);

        const entt::entity realMesh = probe.create();
        probe.emplace<MeshComponent>(realMesh, MeshComponent{});
        probe.emplace<RenderTagComponent>(realMesh, RenderTagComponent{});
        const bool notEmptyWithMesh = isSceneEmpty(probe);

        if (!emptyAtStart || !emptyWithSkyboxOnly || notEmptyWithMesh)
        {
            std::cerr << "[Editor] empty state selfTest FAIL | empty=" << emptyAtStart
                << " skyboxOnly=" << emptyWithSkyboxOnly << " withMesh=" << notEmptyWithMesh << std::endl;
            return false;
        }
        std::cout << "[Editor] empty state selfTest PASS | skybox+empty ignored, real mesh counts" << std::endl;
    }

    // ---- 0b) E4-b：偏好文件往返（纯逻辑，不碰磁盘） ----
    if (!EditorPreferences::selfTest())
        return false;

    // ---- 0c) E4-c：速查表本身就是数据，顺手校验它没有空项/重复键 ----
    {
        if (kShortcutEntryCount < 12)
        {
            std::cerr << "[Editor] shortcut sheet selfTest FAIL: table too small" << std::endl;
            return false;
        }
        for (size_t i = 0; i < kShortcutEntryCount; ++i)
        {
            const ShortcutEntry& entry = kShortcutEntries[i];
            if (entry.group == nullptr || entry.keys == nullptr || entry.action == nullptr ||
                entry.group[0] == 0 || entry.keys[0] == 0 || entry.action[0] == 0)
            {
                std::cerr << "[Editor] shortcut sheet selfTest FAIL: empty entry at " << i << std::endl;
                return false;
            }
            for (size_t j = i + 1; j < kShortcutEntryCount; ++j)
            {
                if (std::strcmp(entry.keys, kShortcutEntries[j].keys) == 0)
                {
                    std::cerr << "[Editor] shortcut sheet selfTest FAIL: duplicate keys " << entry.keys << std::endl;
                    return false;
                }
            }
        }
        std::cout << "[Editor] shortcut sheet selfTest PASS | entries=" << kShortcutEntryCount << std::endl;
    }

    // ---- 0d) E4-d：历史跳转。jumpTo 必须一步步 apply/revert，不能整段搬运 ----
    {
        struct CountingCommand : Command
        {
            int* value = nullptr;
            int delta = 0;
            void apply() override { *value += delta; }
            void revert() override { *value -= delta; }
            const char* name() const override { return "Counting"; }
        };

        CommandStack stack;
        int value = 0;
        for (int i = 0; i < 3; ++i)
        {
            auto command = std::make_unique<CountingCommand>();
            command->value = &value;
            command->delta = 1;
            stack.execute(std::move(command));
        }
        if (value != 3 || stack.undoCount() != 3 || stack.undoEntries().size() != 3)
        {
            std::cerr << "[Editor] history jump selfTest FAIL: setup" << std::endl;
            return false;
        }

        stack.jumpTo(0);
        if (value != 0 || stack.undoCount() != 0 || stack.redoCount() != 3)
        {
            std::cerr << "[Editor] history jump selfTest FAIL: jump to 0" << std::endl;
            return false;
        }

        stack.jumpTo(3);
        if (value != 3 || stack.undoCount() != 3 || stack.redoCount() != 0)
        {
            std::cerr << "[Editor] history jump selfTest FAIL: jump to 3" << std::endl;
            return false;
        }

        stack.jumpTo(99);   // 超出范围要 clamp 到总深度，不能越界
        if (stack.undoCount() != 3)
        {
            std::cerr << "[Editor] history jump selfTest FAIL: clamp" << std::endl;
            return false;
        }

        std::cout << "[Editor] history jump selfTest PASS | depth 0 -> 3 -> clamp" << std::endl;
    }

    // ---- 1) 命令栈：精确撤销 ----
    {
        const entt::entity entity = registry.create();
        const glm::vec3 original{ 1.0f, 2.0f, 3.0f };
        TransformComponent initialTransform;
        initialTransform.setTranslation(original);
        registry.emplace<TransformComponent>(entity, initialTransform);

        CommandStack stack;
        const glm::vec3 target{ 4.0f, 5.0f, 6.0f };
        stack.execute(std::make_unique<SetTransformCommand>(
            registry, entity, original, target, glm::vec3(0.0f), glm::vec3(0.0f),
            glm::vec3(1.0f), glm::vec3(1.0f)));

        const glm::vec3 afterApply = registry.get<TransformComponent>(entity).getTranslation();
        if (afterApply != target)
        {
            std::cerr << "[Editor] selfTest FAIL: transform not applied\n";
            return false;
        }

        stack.undo();
        const glm::vec3 afterUndo = registry.get<TransformComponent>(entity).getTranslation();
        // 必须**精确**相等，不能有 epsilon —— 命令存的是旧值拷贝，不是增量。
        if (afterUndo != original)
        {
            std::cerr << "[Editor] selfTest FAIL: undo not exact ("
                      << afterUndo.x << "," << afterUndo.y << "," << afterUndo.z << ")\n";
            return false;
        }

        stack.redo();
        if (registry.get<TransformComponent>(entity).getTranslation() != target)
        {
            std::cerr << "[Editor] selfTest FAIL: redo failed\n";
            return false;
        }

        // B-06 exception: this command-stack selfTest owns a standalone
        // entt::registry, not a Scene. Production destruction must go through
        // Scene::destroyEntity so meshes/indexes/streaming maps are cleaned up.
        registry.destroy(entity);
        stack.clear();
    }

    // ---- 2) 删除实体：撤销后子树重建 ----
    {
        const entt::entity root = registry.create();
        registry.emplace<TransformComponent>(root);
        registry.emplace<HierarchyComponent>(root);
        registry.emplace<NameComponent>(root, NameComponent{ "Root" });

        const entt::entity child = registry.create();
        registry.emplace<TransformComponent>(child);
        registry.emplace<HierarchyComponent>(child);
        registry.emplace<NameComponent>(child, NameComponent{ "Child" });
        registry.get<HierarchyComponent>(child).parent = root;
        registry.get<HierarchyComponent>(root).children.push_back(child);

        CommandStack stack;
        stack.execute(std::make_unique<DeleteEntityCommand>(registry, root));

        if (registry.valid(root) || registry.valid(child))
        {
            std::cerr << "[Editor] selfTest FAIL: delete did not remove subtree\n";
            return false;
        }

        stack.undo();

        // 重建后 ID 变了，所以要按名字找回来
        entt::entity restoredRoot = entt::null;
        entt::entity restoredChild = entt::null;
        for (entt::entity e : registry.view<NameComponent>())
        {
            const std::string& name = registry.get<NameComponent>(e).name;
            if (name == "Root") restoredRoot = e;
            if (name == "Child") restoredChild = e;
        }

        if (restoredRoot == entt::null || restoredChild == entt::null)
        {
            std::cerr << "[Editor] selfTest FAIL: undo did not restore both entities\n";
            return false;
        }

        // 父子关系必须恢复，否则"撤销删除"之后物体会飘到世界原点
        const auto* parentOfChild = registry.try_get<HierarchyComponent>(restoredChild);
        if (!parentOfChild || parentOfChild->parent != restoredRoot)
        {
            std::cerr << "[Editor] selfTest FAIL: parent link not restored\n";
            return false;
        }

        const auto& childrenOfRoot = registry.get<HierarchyComponent>(restoredRoot).children;
        if (childrenOfRoot.size() != 1 || childrenOfRoot.front() != restoredChild)
        {
            std::cerr << "[Editor] selfTest FAIL: children list not restored\n";
            return false;
        }
    }

    // ---- 2b) E1：批量删除 + 复制（子树） ----
    {
        const entt::entity root = registry.create();
        registry.emplace<TransformComponent>(root);
        registry.emplace<HierarchyComponent>(root);
        registry.emplace<NameComponent>(root, NameComponent{ "BatchRoot" });

        const entt::entity child = registry.create();
        registry.emplace<TransformComponent>(child);
        registry.emplace<HierarchyComponent>(child);
        registry.emplace<NameComponent>(child, NameComponent{ "BatchChild" });
        registry.get<HierarchyComponent>(child).parent = root;
        registry.get<HierarchyComponent>(root).children.push_back(child);

        const entt::entity peer = registry.create();
        registry.emplace<TransformComponent>(peer);
        registry.emplace<NameComponent>(peer, NameComponent{ "BatchPeer" });

        CommandStack stack;
        {
            std::vector<std::unique_ptr<Command>> childrenCommands;
            childrenCommands.push_back(std::make_unique<DeleteEntityCommand>(registry, root));
            childrenCommands.push_back(std::make_unique<DeleteEntityCommand>(registry, peer));
            stack.execute(std::make_unique<CompositeCommand>(
                "Delete Entities", std::move(childrenCommands)));
        }

        if (registry.valid(root) || registry.valid(child) || registry.valid(peer))
        {
            std::cerr << "[Editor] selfTest FAIL: composite delete left entities alive (batch)\n";
            return false;
        }
        if (stack.undoCount() != 1)
        {
            std::cerr << "[Editor] selfTest FAIL: composite delete pushed "
                      << stack.undoCount() << " commands (batch)\n";
            return false;
        }

        stack.undo();

        entt::entity restoredRoot = entt::null;
        entt::entity restoredPeer = entt::null;
        for (entt::entity e : registry.view<NameComponent>())
        {
            const std::string& name = registry.get<NameComponent>(e).name;
            if (name == "BatchRoot") restoredRoot = e;
            if (name == "BatchPeer") restoredPeer = e;
        }
        if (restoredRoot == entt::null || restoredPeer == entt::null)
        {
            std::cerr << "[Editor] selfTest FAIL: composite undo did not restore entities\n";
            return false;
        }

        auto duplicate = std::make_unique<DuplicateEntityCommand>(registry, nullptr, restoredRoot);
        DuplicateEntityCommand* raw = duplicate.get();
        stack.execute(std::move(duplicate));

        const entt::entity copyRoot = raw->duplicatedRoot();
        if (copyRoot == entt::null || !registry.valid(copyRoot))
        {
            std::cerr << "[Editor] selfTest FAIL: duplicate did not create a root\n";
            return false;
        }
        const auto* copyName = registry.try_get<NameComponent>(copyRoot);
        if (copyName == nullptr || copyName->name != "BatchRoot Copy")
        {
            std::cerr << "[Editor] selfTest FAIL: duplicate name not renamed\n";
            return false;
        }
        entt::entity copyChild = entt::null;
        for (entt::entity e : registry.view<NameComponent>())
            if (registry.get<NameComponent>(e).name == "BatchChild Copy") copyChild = e;
        if (copyChild == entt::null)
        {
            std::cerr << "[Editor] selfTest FAIL: duplicate did not copy the subtree\n";
            return false;
        }
        const auto* copyHierarchy = registry.try_get<HierarchyComponent>(copyChild);
        if (copyHierarchy == nullptr || copyHierarchy->parent != copyRoot)
        {
            std::cerr << "[Editor] selfTest FAIL: duplicate parent link wrong\n";
            return false;
        }

        stack.undo();
        if (registry.valid(copyRoot) || registry.valid(copyChild))
        {
            std::cerr << "[Editor] selfTest FAIL: duplicate undo left copies alive\n";
            return false;
        }

        for (entt::entity e : registry.view<NameComponent>())
        {
            const std::string& name = registry.get<NameComponent>(e).name;
            if (name == "BatchRoot" || name == "BatchChild" || name == "BatchPeer")
                registry.destroy(e);
        }
        stack.clear();
    }

    // ---- 2c) E2：多对象变换一条命令 ----
    {
        const entt::entity a = registry.create();
        registry.emplace<TransformComponent>(a);
        registry.emplace<NameComponent>(a, NameComponent{ "BatchMoveA" });
        const entt::entity b = registry.create();
        registry.emplace<TransformComponent>(b);
        registry.emplace<NameComponent>(b, NameComponent{ "BatchMoveB" });

        auto* transformA = &registry.get<TransformComponent>(a);
        auto* transformB = &registry.get<TransformComponent>(b);
        const TransformPose poseA{ a, glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f),
                                   glm::vec3(0.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                                   glm::vec3(1.0f), glm::vec3(2.0f),
                                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                   glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f)) };
        const TransformPose poseB{ b, glm::vec3(5.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
                                   glm::vec3(0.0f), glm::vec3(0.0f),
                                   glm::vec3(1.0f), glm::vec3(1.0f),
                                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

        CommandStack moveStack;
        moveStack.execute(std::make_unique<SetTransformsCommand>(
            registry, std::vector<TransformPose>{ poseA, poseB }, nullptr));

        if (glm::distance(transformA->getTranslation(), glm::vec3(1.0f, 2.0f, 3.0f)) > 1e-4f ||
            glm::distance(transformB->getTranslation(), glm::vec3(-1.0f, 0.0f, 0.0f)) > 1e-4f)
        {
            std::cerr << "[Editor] selfTest FAIL: batch transform did not move both entities" << std::endl;
            return false;
        }
        if (moveStack.undoCount() != 1)
        {
            std::cerr << "[Editor] selfTest FAIL: batch transform pushed " << moveStack.undoCount() << " commands (multi)" << std::endl;
            return false;
        }

        moveStack.undo();
        if (glm::distance(transformA->getTranslation(), glm::vec3(0.0f)) > 1e-4f ||
            glm::distance(transformB->getTranslation(), glm::vec3(5.0f)) > 1e-4f)
        {
            std::cerr << "[Editor] selfTest FAIL: batch transform undo did not restore poses" << std::endl;
            return false;
        }

        const SetTransformsCommand sameOrder(registry, std::vector<TransformPose>{ poseA, poseB }, nullptr);
        const SetTransformsCommand otherOrder(registry, std::vector<TransformPose>{ poseB, poseA }, nullptr);
        if (!sameOrder.mergeWith(SetTransformsCommand(registry, std::vector<TransformPose>{ poseA, poseB }, nullptr)))
        {
            std::cerr << "[Editor] selfTest FAIL: batch transform merge rejected same entities" << std::endl;
            return false;
        }
        if (otherOrder.mergeWith(SetTransformsCommand(registry, std::vector<TransformPose>{ poseA, poseB }, nullptr)))
        {
            std::cerr << "[Editor] selfTest FAIL: batch transform merge accepted a different selection" << std::endl;
            return false;
        }

        registry.destroy(a);
        registry.destroy(b);
        moveStack.clear();
    }

    // ---- 2d) E3：材质参数 + Render Tag 一条命令 ----
    {
        const entt::entity e = registry.create();
        auto material = std::make_shared<Material>();
        material->baseColorFactor = glm::vec4(1.0f);
        material->metallicFactor = 1.0f;
        material->roughnessFactor = 1.0f;
        MaterialComponent materialComp;
        materialComp.material = material;
        registry.emplace<MaterialComponent>(e, materialComp);
        registry.emplace<RenderTagComponent>(e);

        MaterialSnapshot before;
        before.baseColor = material->baseColorFactor;
        before.metallic = material->metallicFactor;
        before.roughness = material->roughnessFactor;
        before.alphaCutoff = material->alphaCutoff;
        before.alphaMode = static_cast<int>(material->alphaMode);

        MaterialSnapshot after = before;
        after.baseColor = glm::vec4(0.25f, 0.5f, 0.75f, 1.0f);
        after.metallic = 0.2f;
        after.roughness = 0.8f;

        CommandStack materialStack;
        materialStack.execute(std::make_unique<SetMaterialCommand>(
            registry, e, before, after, nullptr));
        if (material->metallicFactor != 0.2f || material->roughnessFactor != 0.8f ||
            materialStack.undoCount() != 1)
        {
            std::cerr << "[Editor] selfTest FAIL: material command did not apply (E3)" << std::endl;
            return false;
        }
        materialStack.undo();
        if (material->metallicFactor != 1.0f || material->baseColorFactor.z != 1.0f)
        {
            std::cerr << "[Editor] selfTest FAIL: material undo did not restore factors (E3)" << std::endl;
            return false;
        }

        auto* tag = registry.try_get<RenderTagComponent>(e);
        materialStack.execute(std::make_unique<SetRenderTagCommand>(
            registry, e, tag->isSkybox, tag->isGrid, true, false));
        if (!tag->isSkybox)
        {
            std::cerr << "[Editor] selfTest FAIL: render tag command did not apply (E3)" << std::endl;
            return false;
        }
        materialStack.undo();
        if (tag->isSkybox)
        {
            std::cerr << "[Editor] selfTest FAIL: render tag undo did not restore flags (E3)" << std::endl;
            return false;
        }

        // 同一实体的连续编辑必须能合并（拖滑杆只留一条撤销）。
        const SetMaterialCommand first(registry, e, before, after, nullptr);
        const SetMaterialCommand second(registry, e, after, before, nullptr);
        if (!second.mergeWith(first))
        {
            std::cerr << "[Editor] selfTest FAIL: material commands did not merge (E3)" << std::endl;
            return false;
        }

        registry.destroy(e);
        materialStack.clear();
    }

    // ---- 3) 场景描述 <-> JSON 往返 ----
    {
        scene::SceneDescription input;

        scene::EntityDescription a;
        a.name = "Root";
        a.translation = { 1.5f, -2.25f, 0.125f };
        a.rotation = { 0.1f, 0.2f, 0.3f };
        a.scale = { 2.0f, 2.0f, 2.0f };
        a.parentIndex = -1;
        a.meshPath = TINY_RESOURCE_DIR "Models/sphere.obj";
        a.isSkybox = false;
        input.entities.push_back(a);

        scene::EntityDescription b;
        b.name = "Child With Space";
        b.parentIndex = 0;                 // 引用数组下标
        b.meshPath = TINY_RESOURCE_DIR "Models/DamagedHelmet/DamagedHelmet.obj";
        b.material.present = true;
        b.material.name = "glTF/Default";
        b.material.baseColorFactor = { 0.2f, 0.4f, 0.6f, 1.0f };
        b.material.metallicFactor = 0.75f;
        b.material.roughnessFactor = 0.35f;
        input.entities.push_back(b);



        scene::EntityDescription terrain;
        terrain.name = "Terrain";
        terrain.isTerrain = true;
        terrain.terrainConfig.seed = 98765u;
        terrain.terrainConfig.resolutionX = 33;
        terrain.terrainConfig.resolutionZ = 31;
        terrain.terrainConfig.sizeX = 120.0f;
        terrain.terrainConfig.sizeZ = 80.0f;
        terrain.terrainConfig.heightScale = 12.5f;
        terrain.terrainConfig.octaveCount = 5;
        terrain.terrainConfig.ridgeWeight = 0.4f;
        terrain.terrainConfig.heightCurve = 1.35f;
        terrain.terrainConfig.islandWeight = 0.7f;
        terrain.terrainConfig.islandRadius = 0.55f;
        terrain.terrainConfig.ridgeSharpness = 2.2f;
        terrain.terrainConfig.detailFrequency = 3.1f;
        terrain.terrainConfig.materialDetailStrength = 0.8f;
        terrain.terrainConfig.uvTiling = 1.7f;
        terrain.terrainConfig.chunkGridX = 3u;
        terrain.terrainConfig.chunkGridZ = 2u;
        terrain.terrainConfig.lodLevelCount = 2u;
        terrain.terrainConfig.lodDistanceScale = 1.6f;
        terrain.terrainConfig.surfaceStyle = terrain::TerrainSurfaceStyle::Alpine;
        input.entities.push_back(terrain);

        scene::SceneDescription output;
        std::string error;
        if (!SceneSerializer::roundTrip(input, output, error))
        {
            std::cerr << "[Editor] selfTest FAIL: roundTrip -> " << error << "\n";
            return false;
        }

        if (output.entities.size() != 3)
        {
            std::cerr << "[Editor] selfTest FAIL: entity count mismatch\n";
            return false;
        }
        if (output.entities[1].parentIndex != 0)
        {
            std::cerr << "[Editor] selfTest FAIL: parentIndex lost in round trip\n";
            return false;
        }
        if (output.entities[0].translation != input.entities[0].translation)
        {
            std::cerr << "[Editor] selfTest FAIL: translation not preserved\n";
            return false;
        }
        if (output.entities[1].material.metallicFactor != 0.75f)
        {
            std::cerr << "[Editor] selfTest FAIL: material factor not preserved\n";
            return false;
        }
        // 路径必须被还原成绝对路径（存盘时写相对，读回时拼回资源根）
        if (output.entities[0].meshPath != input.entities[0].meshPath)

        {

            std::cerr << "[Editor] selfTest FAIL: mesh path round trip mismatch: '"

                      << output.entities[0].meshPath << "'\n";

            return false;

        }

        if (!output.entities[2].isTerrain ||
            output.entities[2].terrainConfig.seed != 98765u ||
            output.entities[2].terrainConfig.resolutionX != 33u ||
            output.entities[2].terrainConfig.resolutionZ != 31u ||
            output.entities[2].terrainConfig.sizeX != 120.0f ||
            output.entities[2].terrainConfig.sizeZ != 80.0f ||
            output.entities[2].terrainConfig.heightScale != 12.5f ||
            output.entities[2].terrainConfig.octaveCount != 5u ||
            output.entities[2].terrainConfig.ridgeWeight != 0.4f ||
            output.entities[2].terrainConfig.heightCurve != 1.35f ||
            output.entities[2].terrainConfig.islandWeight != 0.7f ||
            output.entities[2].terrainConfig.islandRadius != 0.55f ||
            output.entities[2].terrainConfig.ridgeSharpness != 2.2f ||
            output.entities[2].terrainConfig.detailFrequency != 3.1f ||
            output.entities[2].terrainConfig.materialDetailStrength != 0.8f ||
            output.entities[2].terrainConfig.uvTiling != 1.7f ||
            output.entities[2].terrainConfig.chunkGridX != 3u ||
            output.entities[2].terrainConfig.chunkGridZ != 2u ||
            output.entities[2].terrainConfig.lodLevelCount != 2u ||
            output.entities[2].terrainConfig.lodDistanceScale != 1.6f ||
            output.entities[2].terrainConfig.surfaceStyle != terrain::TerrainSurfaceStyle::Alpine)
        {
            std::cerr << "[Editor] selfTest FAIL: terrain params lost in round trip\n";
            return false;
        }
        if (!output.entities[2].meshPath.empty())
        {
            std::cerr << "[Editor] selfTest FAIL: terrain wrote a GPU mesh path\n";
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 需要真实场景的自检
//
// 为什么单独一个函数：CreateEntityCommand 会真的读盘 + 上传顶点缓冲，
// 必须有 Device / commandPool / 材质系统。静态 selfTest() 只能测纯数据结构。
// ---------------------------------------------------------------------------
bool EditorLayer::selfTestWithScene()
{
    if (!state.services.scene || !state.services.registry)
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: no scene bound\n";
        return false;
    }

    std::string gpuTerrainError;
    if (!state.services.scene->selfTestTerrainCompute(gpuTerrainError))
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: GPU terrain compute: "
                  << gpuTerrainError << "\n";
        return false;
    }

    entt::registry& registry = *state.services.registry;
    const std::string cubePath = std::string(TINY_RESOURCE_DIR) + "Models/debug_cube.obj";

    const size_t meshesBefore = registry.view<MeshComponent>().size();
    const size_t entitiesBefore = registry.view<TransformComponent>().size();

    // 用命令创建，而不是直接调 createMeshEntity —— 要验证的正是"命令能撤销"。
    auto command = std::make_unique<CreateEntityCommand>(
        registry, *state.services.scene, cubePath, "EditorSelfTestCube");
    CreateEntityCommand* raw = command.get();
    state.commands.execute(std::move(command));

    const entt::entity created = raw->currentEntity();
    if (created == entt::null)
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: CreateEntityCommand produced null entity\n";
        return false;
    }
    if (registry.view<MeshComponent>().size() != meshesBefore + 1)
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: mesh count not incremented\n";
        return false;
    }

    // 撤销：实体必须消失，且模型不能泄漏（这里只看实体数）
    state.commands.undo();
    if (registry.view<MeshComponent>().size() != meshesBefore ||
        registry.view<TransformComponent>().size() != entitiesBefore)
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: undo did not remove entity\n";
        return false;
    }

    // 重做：走快照路径（不重新读盘），实体必须回来
    state.commands.redo();
    if (registry.view<MeshComponent>().size() != meshesBefore + 1)
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: redo did not restore entity\n";
        return false;
    }

    // 清理：撤销回原状并清空历史，避免影响后续帧与截图
    state.commands.undo();
    state.commands.clear();
    state.selection.clear();

    if (registry.view<MeshComponent>().size() != meshesBefore)
    {
        std::cerr << "[Editor] selfTestWithScene FAIL: scene not restored after cleanup\n";
        return false;
    }

    // ---- F2 / D-18: terrain command, snapshot redo, and regeneration ----
    {
        terrain::TerrainConfig config;
        config.seed = 777u;
        config.resolutionX = 33;
        config.resolutionZ = 33;
        config.sizeX = 40.0f;
        config.sizeZ = 40.0f;
        config.heightScale = 8.0f;

        auto terrainCommand = std::make_unique<CreateTerrainCommand>(
            registry, *state.services.scene, config, "EditorSelfTestTerrain",
            glm::vec3(0.0f));
        CreateTerrainCommand* terrainCommandRaw = terrainCommand.get();
        state.commands.execute(std::move(terrainCommand));

        entt::entity terrainEntity = terrainCommandRaw->currentEntity();
        if (terrainEntity == entt::null || !registry.valid(terrainEntity))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: CreateTerrainCommand produced null entity\n";
            return false;
        }

        auto* terrainComp = registry.try_get<TerrainComponent>(terrainEntity);
        auto* terrainMesh = registry.try_get<MeshComponent>(terrainEntity);
        auto* terrainBounds = registry.try_get<BoundsComponent>(terrainEntity);
        if (!terrainComp || !terrainMesh || !terrainMesh->model || !terrainBounds ||
            !terrainBounds->getLocal().valid)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain components incomplete\n";
            return false;
        }
        const auto& terrainSubMeshes = terrainMesh->model->getSubMeshes();
        bool chunkBoundsValid = terrainSubMeshes.size() == 12u;
        uint32_t lodSubMeshCounts[3] = {};
        for (const SubMesh& subMesh : terrainSubMeshes)
        {
            chunkBoundsValid = chunkBoundsValid && subMesh.hasBounds &&
                subMesh.isTerrainChunk && subMesh.terrainLod < 3u &&
                subMesh.indexCount > 0u &&
                subMesh.terrainSurfaceIndexCount > 0u &&
                subMesh.terrainSurfaceIndexCount <= subMesh.indexCount &&
                subMesh.boundsMin.x <= subMesh.boundsMax.x &&
                subMesh.boundsMin.z <= subMesh.boundsMax.z;
            if (subMesh.terrainLod < 3u)
                lodSubMeshCounts[subMesh.terrainLod] += 1u;
        }
        chunkBoundsValid = chunkBoundsValid &&
            lodSubMeshCounts[0] == 4u && lodSubMeshCounts[1] == 4u &&
            lodSubMeshCounts[2] == 4u;

        if (!terrainComp->generated ||
            terrainComp->vertexCount == 0u ||
            terrainComp->indexCount == 0u ||
            terrainMesh->model->getVertexCount() != terrainComp->vertexCount ||
            terrainMesh->model->getIndexCount() != terrainComp->indexCount ||
            !chunkBoundsValid)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain counts/generated state mismatch\n";
            return false;
        }
        if (registry.any_of<MaterialComponent>(terrainEntity))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain unexpectedly owns a material slot\n";
            return false;
        }

        if (!terrainComp->cpuHeightField ||
            !terrainComp->cpuHeightField->generatedOnGpu ||
            terrainComp->cpuHeightField->width != config.resolutionX ||
            terrainComp->cpuHeightField->height != config.resolutionZ)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: GPU terrain collision HeightField missing\n";
            return false;
        }

        const terrain::HeightField& collisionField = *terrainComp->cpuHeightField;
        const float expectedCenterHeight = collisionField.worldHeightAt(0.0f, 0.0f);
        float localHeight = 0.0f;
        float worldHeight = 0.0f;
        glm::vec3 terrainHit{};
        float terrainDistance = 0.0f;
        if (!state.services.scene->sampleTerrainHeightLocal(
                terrainEntity, 0.0f, 0.0f, localHeight) ||
            std::abs(localHeight - expectedCenterHeight) > 1e-4f ||
            !state.services.scene->sampleTerrainHeightWorld(
                terrainEntity, 0.0f, 0.0f, worldHeight) ||
            std::abs(worldHeight - expectedCenterHeight) > 1e-2f ||
            !state.services.scene->raycastTerrain(
                terrainEntity, glm::vec3(0.0f, 100.0f, 0.0f),
                glm::vec3(0.0f, -1.0f, 0.0f), terrainHit, terrainDistance) ||
            std::abs(terrainHit.y - expectedCenterHeight) > 1e-2f ||
            terrainDistance <= 0.0f)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain collision query mismatch\n";
            return false;
        }
        if (state.services.scene->sampleTerrainHeightLocal(
                terrainEntity, config.sizeX, 0.0f, localHeight) ||
            state.services.scene->raycastTerrain(
                terrainEntity, glm::vec3(config.sizeX, 100.0f, 0.0f),
                glm::vec3(0.0f, -1.0f, 0.0f), terrainHit, terrainDistance))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain collision accepted out-of-bounds query\n";
            return false;
        }

        const std::shared_ptr<Model> firstModel = terrainMesh->model;
        const uint64_t firstRevision = terrainComp->revision;

        state.commands.undo();
        if (registry.valid(terrainEntity) ||
            registry.view<MeshComponent>().size() != meshesBefore ||
            registry.view<TransformComponent>().size() != entitiesBefore)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain undo did not remove entity\n";
            return false;
        }

        state.commands.redo();
        terrainEntity = terrainCommandRaw->currentEntity();
        terrainComp = registry.try_get<TerrainComponent>(terrainEntity);
        terrainMesh = registry.try_get<MeshComponent>(terrainEntity);
        if (!terrainComp || !terrainMesh || terrainMesh->model != firstModel ||
            terrainComp->revision != firstRevision)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain redo did not reuse snapshots\n";
            return false;
        }

        // Simulate a config edit (normally SetTerrainConfigCommand), then build
        // the new mesh outside the command and wrap both states in a command.
        terrainComp->config.seed = 778u;
        const TerrainComponent oldTerrain = *terrainComp;
        const MeshComponent oldMesh = *terrainMesh;
        auto* oldBounds = registry.try_get<BoundsComponent>(terrainEntity);
        auto* oldTag = registry.try_get<RenderTagComponent>(terrainEntity);
        if (!oldBounds || !oldTag)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain snapshot components missing\n";
            return false;
        }
        const BoundsComponent oldBoundsSnapshot = *oldBounds;
        const RenderTagComponent oldTagSnapshot = *oldTag;

        std::string terrainError;
        if (!state.services.scene->regenerateTerrain(terrainEntity, terrainError))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: regenerateTerrain -> "
                      << terrainError << "\n";
            return false;
        }

        const TerrainComponent newTerrain = registry.get<TerrainComponent>(terrainEntity);
        const MeshComponent newMesh = registry.get<MeshComponent>(terrainEntity);
        const BoundsComponent newBounds = registry.get<BoundsComponent>(terrainEntity);
        const RenderTagComponent newTag = registry.get<RenderTagComponent>(terrainEntity);
        if (newMesh.model == oldMesh.model ||
            newTerrain.revision != oldTerrain.revision + 1u ||
            newTerrain.generatedConfig.seed != 778u)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain was not regenerated\n";
            return false;
        }
        if (state.services.scene->getRetiredModelCount() == 0u)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: old terrain Model was not deferred\n";
            return false;
        }

        state.commands.execute(std::make_unique<RegenerateTerrainCommand>(
            registry, *state.services.scene, terrainEntity,
            oldTerrain, newTerrain,
            oldMesh, newMesh,
            oldBoundsSnapshot, newBounds,
            oldTagSnapshot, newTag));

        state.commands.undo();
        auto* undoneMesh = registry.try_get<MeshComponent>(terrainEntity);
        auto* undoneTerrain = registry.try_get<TerrainComponent>(terrainEntity);
        if (!undoneMesh || !undoneTerrain || undoneMesh->model != oldMesh.model ||
            undoneTerrain->generatedConfig.seed != 777u ||
            undoneTerrain->cpuHeightField != oldTerrain.cpuHeightField)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: regenerate undo did not restore old mesh\n";
            return false;
        }

        state.commands.redo();
        auto* redoneMesh = registry.try_get<MeshComponent>(terrainEntity);
        auto* redoneTerrain = registry.try_get<TerrainComponent>(terrainEntity);
        if (!redoneMesh || !redoneTerrain || redoneMesh->model != newMesh.model ||
            redoneTerrain->generatedConfig.seed != 778u)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: regenerate redo did not restore new mesh\n";
            return false;
        }

        // Stage 7: async CPU generation, main-thread upload, and stale-result
        // discard. Two requests are submitted back-to-back; only the newer one
        // may install a mesh and invoke its callback.
        auto* asyncTerrain = registry.try_get<TerrainComponent>(terrainEntity);
        auto* asyncMeshBefore = registry.try_get<MeshComponent>(terrainEntity);
        if (!asyncTerrain || !asyncMeshBefore || !asyncMeshBefore->model)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: async terrain pre-state missing\n";
            return false;
        }
        const uint64_t asyncRevisionBefore = asyncTerrain->revision;
        const std::shared_ptr<Model> asyncModelBefore = asyncMeshBefore->model;

        bool firstCallback = false;
        bool secondCallback = false;
        bool secondSuccess = false;
        asyncTerrain->config.seed = 779u;
        const bool firstSubmitted = state.services.scene->requestRegenerateTerrain(
            terrainEntity, [&firstCallback](bool, const std::string&)
            {
                firstCallback = true;
            });

        asyncTerrain->config.seed = 780u;
        const bool secondSubmitted = state.services.scene->requestRegenerateTerrain(
            terrainEntity, [&secondCallback, &secondSuccess](bool success, const std::string&)
            {
                secondCallback = true;
                secondSuccess = success;
            });

        if (!asyncTerrain->generating)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: async request did not mark generating\n";
            return false;
        }

        state.services.scene->waitForTerrainGeneration();

        auto* asyncTerrainAfter = registry.try_get<TerrainComponent>(terrainEntity);
        auto* asyncMeshAfter = registry.try_get<MeshComponent>(terrainEntity);
        if (!firstSubmitted || !secondSubmitted || firstCallback ||
            !secondCallback || !secondSuccess ||
            !asyncTerrainAfter || !asyncMeshAfter || !asyncMeshAfter->model)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: async terrain request/callback mismatch\n";
            return false;
        }
        if (asyncTerrainAfter->generating ||
            asyncTerrainAfter->revision != asyncRevisionBefore + 1u ||
            asyncTerrainAfter->generatedConfig.seed != 780u ||
            asyncMeshAfter->model == asyncModelBefore ||
            asyncTerrainAfter->lastGenerationMs <= 0.0)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: async terrain result/stale discard mismatch\n";
            return false;
        }
        std::cout << "[Editor] async terrain generation PASS"
                  << " | stale discarded, revision=" << asyncTerrainAfter->revision << "\n";

        // Undo regeneration, then the terrain creation command.
        state.commands.undo();
        state.commands.undo();
        state.commands.clear();
        state.selection.clear();
        if (registry.view<MeshComponent>().size() != meshesBefore ||
            registry.view<TransformComponent>().size() != entitiesBefore)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: terrain test cleanup failed\n";
            return false;
        }
    }

    // ---- B-01: independent streaming chunks + manager serialization ----
    {
        Scene& scene = *state.services.scene;
        const glm::vec3 cameraBefore = scene.getCameraTransform().getTranslation();

        terrain::TerrainConfig chunkConfig;
        chunkConfig.seed = 9001u;
        chunkConfig.resolutionX = 17u;
        chunkConfig.resolutionZ = 17u;
        chunkConfig.sizeX = 16.0f;
        chunkConfig.sizeZ = 16.0f;
        chunkConfig.heightScale = 3.0f;
        chunkConfig.lodLevelCount = 2u;
        chunkConfig.localColorRange = false;
        chunkConfig.skirtsEnabled = false;

        const entt::entity manager = scene.createStreamingTerrain(
            chunkConfig, 1, 2, "StreamingSelfTest");
        if (manager == entt::null || !scene.hasStreamingTerrain())
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming terrain manager missing\n";
            return false;
        }
        const int32_t centerX = static_cast<int32_t>(
            std::floor(cameraBefore.x / chunkConfig.sizeX));
        const int32_t centerZ = static_cast<int32_t>(
            std::floor(cameraBefore.z / chunkConfig.sizeZ));
        const auto chunkKey = [](int32_t x, int32_t z)
        {
            return (static_cast<int64_t>(static_cast<uint32_t>(x)) << 32) |
                   static_cast<uint32_t>(z);
        };

        const auto countLoadedChunks = [&registry]()
        {
            size_t loaded = 0;
            for (auto [entity, chunk, terrain, mesh] :
                 registry.view<TerrainChunkComponent, TerrainComponent, MeshComponent>().each())
            {
                (void)entity;
                (void)chunk;
                if (terrain.generated && !terrain.generating && mesh.model &&
                    terrain.meshGeneratedOnGpu &&
                    terrain.cpuHeightField && terrain.cpuHeightField->generatedOnGpu)
                    ++loaded;
            }
            return loaded;
        };

        if (scene.getInFlightTerrainGpuRequests() == 0u ||
            countLoadedChunks() != 0u)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming heightfield stage did not stay async\n";
            return false;
        }

        constexpr size_t expectedChunks = 9u;
        for (uint32_t frame = 0; frame < 20 && countLoadedChunks() < expectedChunks; ++frame)
        {
            scene.refreshStreamingTerrain();
            scene.waitForTerrainGeneration();
        }

        if (scene.getStreamingTerrainChunkCount() != expectedChunks ||
            countLoadedChunks() != expectedChunks)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streamed chunk set did not settle | "
                      << "mapped=" << scene.getStreamingTerrainChunkCount()
                      << " loaded=" << countLoadedChunks() << "\n";
            return false;
        }

        std::unordered_set<const Model*> chunkModels;
        std::unordered_map<int64_t, entt::entity> chunksByKey;
        for (auto [entity, chunk, terrain, mesh] :
             registry.view<TerrainChunkComponent, TerrainComponent, MeshComponent>().each())
        {
            chunkModels.insert(mesh.model.get());
            chunksByKey[chunkKey(chunk.chunkX, chunk.chunkZ)] = entity;
            if (mesh.model->getSubMeshes().size() != chunkConfig.lodLevelCount)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: streamed chunk LOD count mismatch\n";
                return false;
            }
        }
        if (chunkModels.size() != expectedChunks)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streamed chunks do not own independent Models\n";
            return false;
        }

        const auto leftIt = chunksByKey.find(chunkKey(centerX, centerZ));
        const auto rightIt = chunksByKey.find(chunkKey(centerX + 1, centerZ));
        if (leftIt == chunksByKey.end() || rightIt == chunksByKey.end())
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: adjacent streamed chunks missing\n";
            return false;
        }
        const TerrainComponent& left = registry.get<TerrainComponent>(leftIt->second);
        const TerrainComponent& right = registry.get<TerrainComponent>(rightIt->second);
        for (uint32_t sample = 0; sample <= 4u; ++sample)
        {
            const float t = static_cast<float>(sample) / 4.0f;
            const float leftLocalZ = -chunkConfig.sizeZ * 0.5f + t * chunkConfig.sizeZ;
            const float globalZ = left.config.sampleOriginZ + leftLocalZ;
            const float rightLocalZ = globalZ - right.config.sampleOriginZ;
            const float leftHeight = left.cpuHeightField->worldHeightAt(
                chunkConfig.sizeX * 0.5f, leftLocalZ);
            const float rightHeight = right.cpuHeightField->worldHeightAt(
                -chunkConfig.sizeX * 0.5f, rightLocalZ);
            if (std::abs(leftHeight - rightHeight) > 1e-4f)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: streamed chunk seam height mismatch | "
                          << leftHeight << " vs " << rightHeight << "\n";
                return false;
            }
        }

        const entt::entity pickedChunk = leftIt->second;
        const auto* pickedTransform = registry.try_get<TransformComponent>(pickedChunk);
        glm::vec3 pickedHit{};
        float pickedDistance = 0.0f;
        if (!pickedTransform ||
            !scene.raycastTerrain(
                pickedChunk,
                glm::vec3(pickedTransform->getTranslation().x, 100.0f,
                          pickedTransform->getTranslation().z),
                glm::vec3(0.0f, -1.0f, 0.0f), pickedHit, pickedDistance) ||
            pickedDistance <= 0.0f ||
            scene.resolveTerrainChunkOwner(pickedChunk) != manager)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streamed chunk picking/owner mismatch\n";
            return false;
        }

        const double initialStreamingMaxMs = scene.getMaxStreamingUpdateMs();

        // Move three chunks: old ring must unload and the new radius must stream in.
        scene.getCameraTransform().setTranslation(
            cameraBefore + glm::vec3(chunkConfig.sizeX * 4.0f, 0.0f, 0.0f));
        const int32_t movedCenterX = centerX + 4;
        for (uint32_t frame = 0; frame < 20; ++frame)
        {
            scene.refreshStreamingTerrain();
            scene.waitForTerrainGeneration();
        }

        const double movedStreamingMaxMs = scene.getMaxStreamingUpdateMs();
        if (initialStreamingMaxMs <= 0.0 || movedStreamingMaxMs <= 0.0 ||
            !std::isfinite(movedStreamingMaxMs))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming update timing invalid\n";
            return false;
        }

        bool movedSetValid = scene.getStreamingTerrainChunkCount() == expectedChunks;
        for (auto [entity, chunk] : registry.view<TerrainChunkComponent>().each())
        {
            (void)entity;
            if (std::max(std::abs(chunk.chunkX - movedCenterX),
                         std::abs(chunk.chunkZ - centerZ)) > 1)
                movedSetValid = false;
        }
        if (!movedSetValid || countLoadedChunks() != expectedChunks)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming unload/reload set invalid"
                      << " | mapped=" << scene.getStreamingTerrainChunkCount()
                      << " loaded=" << countLoadedChunks()
                      << " center=" << movedCenterX << "," << centerZ
                      << " valid=" << movedSetValid << "\n";
            for (auto [entity, chunk] : registry.view<TerrainChunkComponent>().each())
            {
                (void)entity;
                std::cerr << "  chunk " << chunk.chunkX << "," << chunk.chunkZ << "\n";
            }
            return false;
        }

        const entt::entity serializedCharacter = scene.createCharacterEntity(
            "B07 Serialized Character",
            scene.getCameraTransform().getTranslation() + glm::vec3(0.0f, 2.0f, 0.0f));
        if (serializedCharacter == entt::null)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: could not create serialized character\n";
            return false;
        }

        const scene::SceneDescription captured = scene.captureDescription();
        int worldManagers = 0;
        int serializedChunks = 0;
        int serializedCharacters = 0;
        for (const scene::EntityDescription& entity : captured.entities)
        {
            if (entity.isTerrainWorld)
                ++worldManagers;
            if (entity.isTerrain && entity.terrainConfig.chunkGridX == 1u)
                ++serializedChunks;
            if (entity.isCharacter)
            {
                ++serializedCharacters;
                if (std::abs(entity.characterRadius - 0.35f) > 1e-5f ||
                    std::abs(entity.characterHeight - 1.8f) > 1e-5f)
                {
                    std::cerr << "[Editor] selfTestWithScene FAIL: character fields lost\n";
                    return false;
                }
            }
        }
        scene::SceneDescription serialized;
        std::string serializationError;
        if (!SceneSerializer::roundTrip(captured, serialized, serializationError) ||
            worldManagers != 1 || serializedChunks != 0 || serializedCharacters != 1)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming serialization | "
                      << serializationError << "\n";
            return false;
        }
        int roundTripManagers = 0;
        for (const scene::EntityDescription& entity : serialized.entities)
        {
            if (entity.isTerrainWorld)
            {
                ++roundTripManagers;
                if (entity.terrainLoadRadius != 1 ||
                    entity.terrainMaxLoadsPerFrame != 2u ||
                    entity.terrainWorldConfig.sizeX != chunkConfig.sizeX)
                {
                    std::cerr << "[Editor] selfTestWithScene FAIL: streaming manager fields lost\n";
                    return false;
                }
            }
        }
        if (roundTripManagers != 1)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming manager round-trip missing\n";
            return false;
        }

        // File-level save/load smoke, not only in-memory roundTrip. Some Windows
        // setups deny writes to the user TEMP directory (errno 13) even though it
        // is listed as writable, so fall back to a repo-local scratch directory.
        std::string fileError;
        std::filesystem::path scenePath =
            std::filesystem::temp_directory_path() / "tiny_streaming_selftest.scene.json";
        bool saved = SceneSerializer::saveToFile(serialized, scenePath.string(), fileError);
        if (!saved)
        {
            std::error_code scratchError;
            const std::filesystem::path scratchRoot =
                std::filesystem::current_path(scratchError) / ".tiny-streaming-selftest";
            if (!scratchError)
            {
                std::filesystem::create_directories(scratchRoot, scratchError);
                if (!scratchError)
                {
                    scenePath = scratchRoot / "tiny_streaming_selftest.scene.json";
                    saved = SceneSerializer::saveToFile(serialized, scenePath.string(), fileError);
                }
            }
        }
        if (!saved)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming save -> "
                      << fileError << "\n";
            return false;
        }
        scene::SceneDescription fileDescription;
        if (!SceneSerializer::loadFromFile(scenePath.string(), fileDescription, fileError))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming load -> "
                      << fileError << "\n";
            return false;
        }
        std::error_code removeError;
        std::filesystem::remove(scenePath, removeError);

        // Rebuild the scene from the serialized manager template. This clears
        // the old chunk set and must stream a fresh 3x3 set around the camera.
        const glm::vec3 movedCamera = scene.getCameraTransform().getTranslation();
        std::string rebuildError;
        if (!scene.buildFromDescription(fileDescription, rebuildError) ||
            !scene.hasStreamingTerrain())
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming rebuild -> "
                      << rebuildError << "\n";
            return false;
        }

        entt::entity restoredCharacter = entt::null;
        for (auto [entity, controller] : registry.view<CharacterControllerComponent>().each())
        {
            (void)controller;
            restoredCharacter = entity;
            break;
        }
        if (restoredCharacter == entt::null)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: character missing after rebuild\n";
            return false;
        }
        const auto* restoredController = registry.try_get<CharacterControllerComponent>(restoredCharacter);
        if (!restoredController ||
            std::abs(restoredController->radius - 0.35f) > 1e-5f ||
            std::abs(restoredController->height - 1.8f) > 1e-5f)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: character fields changed after rebuild\n";
            return false;
        }
        scene.destroyEntity(restoredCharacter);
        for (uint32_t frame = 0; frame < 20 && countLoadedChunks() < expectedChunks; ++frame)
        {
            scene.refreshStreamingTerrain();
            scene.waitForTerrainGeneration();
        }
        if (scene.getStreamingTerrainChunkCount() != expectedChunks ||
            countLoadedChunks() != expectedChunks)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming rebuild chunk set invalid\n";
            return false;
        }
        std::unordered_set<const Model*> rebuiltModels;
        for (auto [entity, chunk, terrain, mesh] :
             registry.view<TerrainChunkComponent, TerrainComponent, MeshComponent>().each())
        {
            (void)entity;
            (void)chunk;
            (void)terrain;
            rebuiltModels.insert(mesh.model.get());
        }
        if (rebuiltModels.size() != expectedChunks)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming rebuild Models not independent\n";
            return false;
        }

        const entt::entity rebuiltManager = scene.getStreamingTerrainEntity();
        auto deleteWorld = std::make_unique<DeleteEntityCommand>(
            registry, rebuiltManager, &scene);
        state.commands.execute(std::move(deleteWorld));
        if (scene.hasStreamingTerrain() ||
            registry.view<TerrainChunkComponent>().size() != 0u ||
            registry.valid(rebuiltManager))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: deleting streamed world leaked chunks\n";
            return false;
        }

        state.commands.undo();
        const entt::entity restoredManager = scene.getStreamingTerrainEntity();
        if (restoredManager == entt::null || !scene.hasStreamingTerrain())
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming delete undo did not re-adopt manager\n";
            return false;
        }
        for (uint32_t frame = 0; frame < 20 && countLoadedChunks() < expectedChunks; ++frame)
        {
            scene.refreshStreamingTerrain();
            scene.waitForTerrainGeneration();
        }
        if (scene.getStreamingTerrainChunkCount() != expectedChunks ||
            countLoadedChunks() != expectedChunks)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming delete undo did not restore chunks\n";
            return false;
        }

        state.commands.redo();
        if (scene.hasStreamingTerrain() ||
            registry.view<TerrainChunkComponent>().size() != 0u)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming delete redo left chunks\n";
            return false;
        }
        state.commands.undo();
        state.commands.clear();

        scene.destroyEntity(scene.getStreamingTerrainEntity());
        scene.getCameraTransform().setTranslation(cameraBefore);
        scene.refreshTransformsAndVisibility();
        if (scene.hasStreamingTerrain() ||
            registry.view<TerrainChunkComponent>().size() != 0u ||
            registry.view<MeshComponent>().size() != meshesBefore)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming manager cleanup leaked chunks\n";
            return false;
        }
        std::cout << "[Editor] streamed terrain PASS | chunks=9 independentModels=9 "
                  << "seam=OK stitch=OK skirts=off gpuMesh=OK unload=OK rebuild=OK picking=OK deleteUndo=OK "
                  << "maxUpdateMs=" << movedStreamingMaxMs << "\n";
    }

    std::cout << "[Editor] scene selfTest PASS | create/undo/redo + terrain create/generate/collision/autodirty/gpucompute/streaming"
              << " (meshes=" << meshesBefore << ")\n";
    // ---- 4) 精确拾取：射线必须命中"更近"的球，而不是把相机包在里面的 Sponza ----
    // 这是 AABB 拾取失效的典型场景：Sponza 的 AABB 把相机包住，
    // 射线与它的入射距离恒为 0，于是点任何东西都选中 Sponza。
    if (state.services.scene && state.services.registry)
    {
        entt::registry& registry = *state.services.registry;
        Scene& scene = *state.services.scene;

        // 注意：不能用 sphere.obj —— 那是半径 1000 的天空盒球。
        const std::string spherePath = std::string(TINY_RESOURCE_DIR) + "Models/debug_cube.obj";
        const entt::entity sphere = scene.createMeshEntity(spherePath, "PickTestCube");
        if (sphere == entt::null)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: could not create test mesh\n";
            return false;
        }

        // 把球放到相机前方 6 个单位
        const TransformComponent& cameraTransform = scene.getCameraTransform();
        const glm::vec3 forward = glm::vec3{
            std::sin(cameraTransform.getEuler().y) * std::cos(cameraTransform.getEuler().x),
            -std::sin(cameraTransform.getEuler().x),
            std::cos(cameraTransform.getEuler().y) * std::cos(cameraTransform.getEuler().x)
        };
        const glm::vec3 spherePos = cameraTransform.getTranslation() + forward * 6.0f;
        registry.get<TransformComponent>(sphere).setTranslation(spherePos);
        registry.get<TransformComponent>(sphere).setScale(glm::vec3(1.0f));
        scene.markTransformDirty(sphere);

        // 刷新世界变换与包围盒，否则测试读到的是上一帧的旧数据
        scene.refreshTransformsAndVisibility();

        // 从相机中心朝球心打一条射线
        const glm::vec3 rayOrigin = cameraTransform.getTranslation();
        const glm::vec3 rayDir = glm::normalize(spherePos - rayOrigin);

        float hitDistance = 0.0f;
        const auto* worldTransform = registry.try_get<WorldTransformComponent>(sphere);
        const auto* meshComp = registry.try_get<MeshComponent>(sphere);
        if (!worldTransform || !meshComp || !meshComp->model)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: test mesh has no model\n";
            return false;
        }

        if (!meshComp->model->intersectRay(worldTransform->matrix, rayOrigin, rayDir, hitDistance))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: ray missed the test mesh\n";
            return false;
        }

        // 命中点必须在"物体中心之前"（近侧），否则说明射线穿过去命中了别的东西。
        // 距离中心 6 个单位，所以近侧命中应 < 6。
        if (hitDistance <= 0.0f || hitDistance >= 6.0f)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: hit distance="
                      << hitDistance << " expected the near side (< 6)\n";
            return false;
        }

        // ---- 5) 包围盒必须跟随变换（否则拖拽时白框会留在原地）----
        // 这正是"拖动时包围盒和物体分开"的回归守卫：
        // 改完 TransformComponent 后必须 refreshTransformsAndVisibility()，
        // 否则 WorldBoundsComponent 停在上一帧。
        {
            auto* transform = registry.try_get<TransformComponent>(sphere);
            const auto* boundsBefore = registry.try_get<WorldBoundsComponent>(sphere);
            if (!transform || !boundsBefore || !boundsBefore->world.valid)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: sphere bounds missing\n";
                return false;
            }

            const glm::vec3 centerBefore = boundsBefore->world.center();

            // 模拟一次拖拽：沿世界 +Y 平移 3 个单位
            const glm::vec3 delta{ 0.0f, 3.0f, 0.0f };
            // No explicit markTransformDirty(): the component owner callback
            // must schedule the Scene dirty queue by itself.
            transform->setTranslation(transform->getTranslation() + delta);
            scene.refreshTransformsAndVisibility();

            const auto* boundsAfter = registry.try_get<WorldBoundsComponent>(sphere);
            if (!boundsAfter || !boundsAfter->world.valid)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: bounds missing after move\n";
                return false;
            }

            const glm::vec3 centerAfter = boundsAfter->world.center();
            if (glm::length((centerAfter - centerBefore) - delta) > 1e-3f)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: bounds did not follow transform (moved "
                          << glm::length(centerAfter - centerBefore) << ", expected 3)\n";
                return false;
            }

            if (scene.getLastWorldTransformUpdates() != 1u ||
                scene.getLastSpatialIndexUpdates() != 1u)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: dirty transform/spatial update count mismatch\n";
                return false;
            }
            scene.refreshTransformsAndVisibility();
            if (scene.getLastWorldTransformUpdates() != 0u ||
                scene.getLastSpatialIndexUpdates() != 0u)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: clean scene still rebuilt transforms/bounds\n";
                return false;
            }

            // BoundsComponent::markDirty() must enqueue spatial refresh without
            // an explicit Scene::markSpatialDirty().
            auto* changedBounds = registry.try_get<BoundsComponent>(sphere);
            if (!changedBounds)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: bounds missing before dirty callback test\n";
                return false;
            }
            AABB changedLocal = changedBounds->getLocal();
            changedLocal.min.y -= 1.0f;
            changedLocal.max.y += 1.0f;
            changedBounds->setLocal(changedLocal);
            scene.refreshTransformsAndVisibility();
            if (scene.getLastWorldTransformUpdates() != 0u ||
                scene.getLastSpatialIndexUpdates() != 1u)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: bounds dirty callback did not enqueue spatial refresh\n";
                return false;
            }
        }

        scene.destroyEntity(sphere);
        scene.refreshTransformsAndVisibility();
    }

    // ---- 2e) E3b：播放期间文档只读（快照不落盘，也不被 New/Open 盖掉）----
    {
        const PlayState savedPlayState = state.playState;
        const bool writableBefore = state.documentWritable();

        state.playState = PlayState::Playing;
        if (state.documentWritable())
        {
            std::cerr << "[Editor] selfTest FAIL: document must be read-only while playing" << std::endl;
            return false;
        }

        state.log.clear();
        if (saveScene() || newScene())
        {
            std::cerr << "[Editor] selfTest FAIL: save/new must be blocked while playing" << std::endl;
            return false;
        }
        if (state.log.empty() || state.log.back().text.find("播放中") == std::string::npos)
        {
            std::cerr << "[Editor] selfTest FAIL: blocked document ops must log a reason" << std::endl;
            return false;
        }

        std::string playLockError;
        if (loadSceneFromPath(".tmp/play-lock-missing.scene.json", playLockError) ||
            playLockError.find("播放中") == std::string::npos)
        {
            std::cerr << "[Editor] selfTest FAIL: open must be blocked while playing" << std::endl;
            return false;
        }

        state.playState = savedPlayState;
        if (state.documentWritable() != writableBefore)
        {
            std::cerr << "[Editor] selfTest FAIL: play state not restored after lock test" << std::endl;
            return false;
        }

        std::cout << "[Editor] play document lock selfTest PASS | save/new/open blocked while playing" << std::endl;
    }

    // ---- 2f) E4：脏文档的操作确认（不执行、不丢意图）----
    {
        const bool dirtyBefore = state.documentDirty;
        const EditorState::PendingDocumentAction pendingBefore = state.pendingDocumentAction;
        const size_t namesBefore = registry.view<NameComponent>().size();

        state.documentDirty = true;
        requestDocumentAction(EditorState::PendingDocumentAction::New);
        if (state.pendingDocumentAction != EditorState::PendingDocumentAction::New)
        {
            std::cerr << "[Editor] selfTest FAIL: dirty document must queue the action" << std::endl;
            return false;
        }
        if (registry.view<NameComponent>().size() != namesBefore)
        {
            std::cerr << "[Editor] selfTest FAIL: dirty document performed the action anyway" << std::endl;
            return false;
        }

        cancelPendingDocumentAction();
        if (state.pendingDocumentAction != EditorState::PendingDocumentAction::None)
        {
            std::cerr << "[Editor] selfTest FAIL: cancel did not clear the pending action" << std::endl;
            return false;
        }

        // 标题必须带脏标记，且只在变化时推给引擎。
        if (state.services.setWindowTitle)
        {
            updateWindowTitle();
            if (lastWindowTitle.find(" *") == std::string::npos)
            {
                std::cerr << "[Editor] selfTest FAIL: dirty title missing marker: " << lastWindowTitle << std::endl;
                return false;
            }
            state.documentDirty = false;
            updateWindowTitle();
            if (lastWindowTitle.find(" *") != std::string::npos)
            {
                std::cerr << "[Editor] selfTest FAIL: clean title keeps dirty marker" << std::endl;
                return false;
            }
        }

        state.documentDirty = dirtyBefore;
        state.pendingDocumentAction = pendingBefore;
        std::cout << "[Editor] unsaved changes prompt selfTest PASS | dirty blocks new/open/exit until resolved" << std::endl;
    }

    // ---- 2g) E4-b：偏好落盘往返（真写一次文件，但落在 .tmp 下） ----
    {
        std::error_code code;
        std::filesystem::create_directories(".tmp", code);
        const std::string path = ".tmp/editor-preferences-selftest.json";

        EditorPreferences prefs;
        prefs.setPanelVisible("Console", false);
        prefs.gizmoMode = 2;
        prefs.pushRecent("Resources/Scenes/main.scene.json");
        std::string error;
        if (!prefs.save(path, error))
        {
            std::cerr << "[Editor] preferences file selfTest FAIL: save: " << error << std::endl;
            return false;
        }

        EditorPreferences loaded;
        if (!loaded.load(path, error))
        {
            std::cerr << "[Editor] preferences file selfTest FAIL: load: " << error << std::endl;
            return false;
        }
        if (loaded.gizmoMode != 2 || loaded.panelVisible("Console") ||
            loaded.recentScenes.size() != 1 ||
            loaded.recentScenes[0] != "Resources/Scenes/main.scene.json")
        {
            std::cerr << "[Editor] preferences file selfTest FAIL: values lost on disk" << std::endl;
            return false;
        }

        std::cout << "[Editor] preferences file selfTest PASS | save + load + recent round trip" << std::endl;
    }

    return true;
}

} // namespace editor
