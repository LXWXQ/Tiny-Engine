#include "Editor/EditorLayer.h"
#include "Editor/SceneSerializer.h"
#include "Editor/Commands.h"
#include "Editor/Prefab.h"

#include "Editor/panels/HierarchyPanel.h"
#include "Editor/panels/InspectorPanel.h"
#include "Editor/TextureLibrary.h"
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
    // 视口左上角的模式徽标通过这个回调切换，确保仍走 setEditorMode() 的
    // 相机/角色接管与通知逻辑，而不是面板直接改 mode。
    state.toggleEditorMode = [this]()
    {
        setEditorMode(state.isRuntime() ? EditorState::EditorMode::Edit
                                        : EditorState::EditorMode::Runtime);
    };

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

namespace
{
// UX-08：定义在下方匿名 namespace 里，这里先声明，beginFrame 要用。
bool advanceTerrainCreateRequest(bool& pending, int& waitFrames);
} // namespace

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

    // F3：场景里可能已经有体素世界（例如 --voxel-world 启动或打开存档），
    // 笔刷要认它 —— 首次见到就记住，之后由「创建 -> 体素世界」更新。
    // 体素世界会随「停止播放 / 打开场景」被重建，旧实体句柄随即失效，
    // 所以这里同时检查 null 与 valid，否则笔刷会一直指向一个已销毁的实体。
    if (state.services.registry != nullptr &&
        (state.voxelWorld == entt::null || !state.services.registry->valid(state.voxelWorld)))
    {
        auto view = state.services.registry->view<VoxelWorldComponent>();
        state.voxelWorld = view.empty() ? entt::null : *view.begin();
    }

    if (advanceTerrainCreateRequest(state.terrainCreatePending, state.terrainCreateWaitFrames))
        createTerrainFromUi();

    // tech-debt #19：异步地形构建的收尾播报（每帧最多看一次）。
    if (state.pendingTerrainBuild != entt::null && state.services.registry != nullptr)
    {
        const entt::entity building = state.pendingTerrainBuild;
        const auto* terrain = state.services.registry->try_get<TerrainComponent>(building);
        if (terrain == nullptr)
        {
            state.pendingTerrainBuild = entt::null;
        }
        else if (!terrain->generating)
        {
            std::string error = terrain->lastError;
            char buffer[256];
            if (terrain->generated)
            {
                std::snprintf(buffer, sizeof(buffer),
                              "地形生成完成 | %ux%u, vertices=%u, indices=%u, %.2f ms",
                              terrain->generatedConfig.resolutionX,
                              terrain->generatedConfig.resolutionZ,
                              terrain->vertexCount, terrain->indexCount,
                              terrain->lastGenerationMs);
                state.notifySuccess(buffer);
            }
            else
            {
                state.notify(LogEntry::Level::Error,
                             "地形生成失败：" + (error.empty() ? std::string("未知原因") : error));
            }
            state.pendingTerrainBuild = entt::null;
        }
    }

    handleShortcuts();
    updatePlayState(dt);
    updateRuntimeMode(dt);
}

namespace
{
// E1：多选批量操作。地形 chunk / 流送管理器不参与批量删除与复制：
// 前者由流送系统回收，后者是整块高度场的宿主。
bool isBatchExcluded(entt::registry& registry, entt::entity entity)
{
    return registry.any_of<TerrainChunkComponent>(entity) ||
           registry.any_of<TerrainStreamingComponent>(entity) ||
           registry.any_of<VoxelChunkComponent>(entity);
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
    const size_t count = children.size();
    state.commands.execute(std::make_unique<CompositeCommand>("Delete Entities", std::move(children)));
    state.markDirty();
    state.notifySuccess("已删除 " + std::to_string(count) + " 个对象（Ctrl+Z 可撤销）");
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

// Ctrl+A：只选"有名字、不是流送 chunk"的实体，避免把地形块全部选中。
void selectAllEntities(EditorState& state)
{
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

// E4-c / UX-13：快捷键只有这一张表 —— 速查表按它排版，handleShortcuts 也按它分发，
// 键位文本由 key + 修饰键现算（shortcutKeys），不再另写一份字符串，避免两份代码漂移。
enum class ShortcutAction
{
    NewScene,
    OpenScene,
    SaveScene,
    Undo,
    Redo,
    SelectAll,
    Duplicate,
    DeleteSelection,
    FocusSelection,
    ModeTranslate,
    ModeRotate,
    ModeScale,
    Play,
    PauseResume,
    Stop,
    ToggleShortcutSheet,
    CloseShortcutSheet,
    FocusConsoleSearch,
    ToggleMode,
};

struct ShortcutEntry
{
    const char* group;
    ImGuiKey key;
    bool ctrl;
    bool shift;
    ShortcutAction action;
    const char* actionText;
};

const ShortcutEntry kShortcutEntries[] =
{
    { "文档", ImGuiKey_N, true, false, ShortcutAction::NewScene, "新建场景（有未保存修改时先确认）" },
    { "文档", ImGuiKey_O, true, false, ShortcutAction::OpenScene, "打开场景" },
    { "文档", ImGuiKey_S, true, false, ShortcutAction::SaveScene, "保存" },
    { "文档", ImGuiKey_Z, true, false, ShortcutAction::Undo, "撤销" },
    { "文档", ImGuiKey_Z, true, true, ShortcutAction::Redo, "重做（同 Ctrl+Y）" },
    { "文档", ImGuiKey_Y, true, false, ShortcutAction::Redo, "重做" },
    { "选择", ImGuiKey_A, true, false, ShortcutAction::SelectAll, "全选（跳过地形 chunk 与相机）" },
    { "选择", ImGuiKey_D, true, false, ShortcutAction::Duplicate, "复制选中（含子树）" },
    { "选择", ImGuiKey_Delete, false, false, ShortcutAction::DeleteSelection, "删除选中" },
    { "选择", ImGuiKey_F, false, false, ShortcutAction::FocusSelection, "把相机聚焦到选中" },
    { "变换", ImGuiKey_W, false, false, ShortcutAction::ModeTranslate, "移动手柄（多选时整组一起变）" },
    { "变换", ImGuiKey_E, false, false, ShortcutAction::ModeRotate, "旋转手柄" },
    { "变换", ImGuiKey_R, false, false, ShortcutAction::ModeScale, "缩放手柄" },
    { "播放", ImGuiKey_F5, false, false, ShortcutAction::Play, "开始播放" },
    { "播放", ImGuiKey_F6, false, false, ShortcutAction::PauseResume, "暂停 / 继续" },
    { "播放", ImGuiKey_F7, false, false, ShortcutAction::Stop, "停止并恢复快照" },
    { "视图", ImGuiKey_Slash, false, true, ShortcutAction::ToggleShortcutSheet, "打开 / 关闭本速查表（Shift+/ 就是 ?）" },
    { "视图", ImGuiKey_Escape, false, false, ShortcutAction::CloseShortcutSheet, "关闭速查表" },
    { "视图", ImGuiKey_F, true, false, ShortcutAction::FocusConsoleSearch, "跳到 Console 的日志搜索框" },
    { "模式", ImGuiKey_F8, false, false, ShortcutAction::ToggleMode, "编辑 / 运行模式切换（运行模式：左键挖、右键放）" },
};

constexpr size_t kShortcutEntryCount = sizeof(kShortcutEntries) / sizeof(kShortcutEntries[0]);

// 帮助 → 示例：菜单项与加载分支共用这张表，增删示例时不会两边各改一半。
struct ExampleEntry
{
    const char* id;
    const char* label;
    bool separatorBefore;
};

const ExampleEntry kEditorExamples[] =
{
    { "voxel-glass", "体素玻璃（V-01）", false },
    { "voxel-world", "体素世界（3×3 chunk）", false },
    { "terrain", "程序化地形 257×257", true },
    { "streaming", "流送地形（围绕相机加载）", false },
};

bool isKnownExampleId(const std::string& id)
{
    for (const ExampleEntry& example : kEditorExamples)
    {
        if (id == example.id)
            return true;
    }
    return false;
}

// 显示用键位文本：唯一来源是 key + 修饰键，速查表与自检都调它。
std::string shortcutKeys(const ShortcutEntry& entry)
{
    std::string text;
    if (entry.ctrl)
        text += "Ctrl+";
    if (entry.shift)
        text += "Shift+";
    const char* keyName = ImGui::GetKeyName(entry.key);
    text += (keyName != nullptr) ? keyName : "?";
    return text;
}

} // namespace

void EditorLayer::handleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return;      // 正在输入文字，快捷键全部让路

    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;

    // UX-13：按表分发。修饰键必须精确匹配，所以 Ctrl+Z 不会顺带触发 Ctrl+Shift+Z。
    for (const ShortcutEntry& entry : kShortcutEntries)
    {
        if (entry.ctrl != ctrl || entry.shift != shift)
            continue;
        if (!ImGui::IsKeyPressed(entry.key, false))
            continue;

        switch (entry.action)
        {
        case ShortcutAction::NewScene:
            requestDocumentAction(EditorState::PendingDocumentAction::New);
            break;
        case ShortcutAction::OpenScene:
            requestDocumentAction(EditorState::PendingDocumentAction::Open);
            break;
        case ShortcutAction::SaveScene:
            saveScene();
            break;
        case ShortcutAction::Undo:
            if (state.commands.canUndo())
            {
                state.commands.undo();
                state.markDirty();
            }
            break;
        case ShortcutAction::Redo:
            if (state.commands.canRedo())
            {
                state.commands.redo();
                state.markDirty();
            }
            break;
        case ShortcutAction::SelectAll:
            selectAllEntities(state);
            break;
        case ShortcutAction::Duplicate:
            duplicateSelection(state);
            break;
        case ShortcutAction::DeleteSelection:
            // E1：删除整个选中集合（父节点被选中时子节点自动跳过，避免重复删除）。
            deleteSelection(state);
            break;
        case ShortcutAction::FocusSelection:
            focusSelection(state);
            break;
        case ShortcutAction::ModeTranslate:
            state.gizmo.mode = GizmoMode::Translate;
            break;
        case ShortcutAction::ModeRotate:
            state.gizmo.mode = GizmoMode::Rotate;
            break;
        case ShortcutAction::ModeScale:
            state.gizmo.mode = GizmoMode::Scale;
            break;
        case ShortcutAction::Play:
            if (state.playState == PlayState::Editing)
                enterPlay();
            break;
        case ShortcutAction::PauseResume:
            if (state.playState == PlayState::Playing)
                pausePlay();
            else if (state.playState == PlayState::Paused)
                resumePlay();
            break;
        case ShortcutAction::Stop:
            if (state.playState != PlayState::Editing)
                stopPlay();
            break;
        case ShortcutAction::ToggleShortcutSheet:
            showShortcutSheet = !showShortcutSheet;
            break;
        case ShortcutAction::CloseShortcutSheet:
            if (showShortcutSheet)
                showShortcutSheet = false;
            break;
        case ShortcutAction::ToggleMode:
            setEditorMode(state.isRuntime() ? EditorState::EditorMode::Edit
                                             : EditorState::EditorMode::Runtime);
            return;
        case ShortcutAction::FocusConsoleSearch:
            if (EditorPanel* console = findPanel("Console"); console != nullptr)
            {
                console->visible = true;
                state.consoleSearchFocusRequest = true;
            }
            break;
        }
        return;      // 一帧只处理一个快捷键，避免多个动作互相踩
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

    // B3：Toast 的倒计时与「最近错误」的年龄都按帧推进，
    // 放在 UI 构建的最前面，保证同一帧里做的动作立刻可见。
    state.tickNotices(ImGui::GetIO().DeltaTime);

    drawToolbar();

    drawPanels();
    // BeginMainMenuBar 在第一帧还没把主视口的 WorkPos 下推（此时 WorkPos.y == 0），
    // 这一帧算出来的面板矩形会整体偏高一个菜单栏高度。所以布局标志要多留一帧：
    // 等工作区稳定之后再落位，否则第一帧的错位置会被 imgui.ini 记住。
    if (ImGui::GetMainViewport()->WorkPos.y > 0.5f)
        layoutNeedsReset = false;

    drawStatusBar();

    drawToast();
    drawOpenFailurePrompt();

    // Console 已经画出来 = 用户能看到这些错误了；清掉未读与高亮请求，
    // 否则徽标会一直挂着，而高亮会把滚动位置钉死在那一行。
    if (EditorPanel* console = findPanel("Console"); console != nullptr && console->visible)
    {
        if (!state.errorRaisedThisFrame)
            state.markErrorsRead();
        state.focusConsoleRequest = false;
    }

    drawShortcutSheet();
    drawAboutWindow();

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

    if (ImGui::BeginMenu("文件"))
    {
        const bool writable = state.documentWritable();
        if (ImGui::MenuItem("新建场景", "Ctrl+N", false, writable))
            requestDocumentAction(EditorState::PendingDocumentAction::New);
        if (ImGui::MenuItem("打开场景...", "Ctrl+O", false, writable))
            requestDocumentAction(EditorState::PendingDocumentAction::Open);
        if (ImGui::BeginMenu("最近打开", writable && !state.recentScenes.empty()))
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
        if (ImGui::MenuItem("保存", "Ctrl+S", false, writable)) saveScene();
        if (ImGui::MenuItem("另存为...", nullptr, false, writable)) saveSceneAs();
        if (ImGui::MenuItem("保存选中为预制体...", nullptr, false,
                            writable && state.services.scene != nullptr && !state.selection.empty()))
            savePrefabFromSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("重新加载配置") && state.services.reloadConfig)
            state.services.reloadConfig();
        if (ImGui::MenuItem("保存配置") && state.services.saveConfig)
            state.services.saveConfig();
        ImGui::Separator();
        if (ImGui::MenuItem("退出"))
            requestDocumentAction(EditorState::PendingDocumentAction::Exit);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("编辑"))
    {
        if (ImGui::MenuItem(state.commands.canUndo() ? "撤销" : "撤销（没有可撤销的操作）", "Ctrl+Z",
                            false, state.commands.canUndo()))
        {
            state.commands.undo();
            state.markDirty();
        }
        if (ImGui::MenuItem(state.commands.canRedo() ? "重做" : "重做（没有可重做的操作）", "Ctrl+Y",
                            false, state.commands.canRedo()))
        {
            state.commands.redo();
            state.markDirty();
        }
        ImGui::Separator();
        ImGui::TextDisabled("历史：%d 步可撤销 / %d 步可重做",
                            static_cast<int>(state.commands.undoCount()),
                            static_cast<int>(state.commands.redoCount()));
        ImGui::Separator();
        if (ImGui::MenuItem("移动 (W)", nullptr, state.gizmo.mode == GizmoMode::Translate))
            state.gizmo.mode = GizmoMode::Translate;
        if (ImGui::MenuItem("旋转 (E)", nullptr, state.gizmo.mode == GizmoMode::Rotate))
            state.gizmo.mode = GizmoMode::Rotate;
        if (ImGui::MenuItem("缩放 (R)", nullptr, state.gizmo.mode == GizmoMode::Scale))
            state.gizmo.mode = GizmoMode::Scale;
        ImGui::Separator();
        // UX-06：这四个以前只活在快捷键里，菜单里补上门面。
        const bool hasSelection = !state.selection.entities.empty();
        if (ImGui::MenuItem("复制选中", "Ctrl+D", false, hasSelection))
            duplicateSelection(state);
        if (ImGui::MenuItem("删除选中", "Del", false, hasSelection))
            deleteSelection(state);
        if (ImGui::MenuItem("聚焦选中", "F", false, hasSelection))
            focusSelection(state);
        if (ImGui::MenuItem("全选", "Ctrl+A", false, state.services.registry != nullptr))
            selectAllEntities(state);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("创建"))
    {
        if (ImGui::MenuItem("地形", nullptr, false,
                            !state.terrainCreatePending && state.services.scene != nullptr))
            requestTerrainCreate();
        if (ImGui::MenuItem("角色"))
            createCharacterFromUi();
        ImGui::Separator();
        if (ImGui::MenuItem("实例化预制体...", nullptr, false,
                            state.documentWritable() && state.services.scene != nullptr))
            instantiatePrefabDialog();
        if (ImGui::MenuItem("体素世界", nullptr, false, state.services.scene != nullptr))
        {
            const entt::entity world = state.services.scene->createVoxelWorld(20251104ull, 1, 0);
            if (world != entt::null)
            {
                state.voxelWorld = world;
                state.notifySuccess("已创建体素世界（3x3 chunk）");
            }
            else
            {
                state.notify(LogEntry::Level::Error, "体素世界创建失败");
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("播放"))
    {
        const bool editing = (state.playState == PlayState::Editing);
        const bool playing = (state.playState == PlayState::Playing);
        const bool paused = (state.playState == PlayState::Paused);

        if (ImGui::MenuItem("播放", "F5", false, editing))
            enterPlay();
        if (ImGui::MenuItem("暂停", "F6", false, playing))
            pausePlay();
        if (ImGui::MenuItem("继续", "F6", false, paused))
            resumePlay();
        if (ImGui::MenuItem("停止", "F7", false, !editing))
            stopPlay();
        ImGui::Separator();
        ImGui::TextDisabled("状态：%s",
                            editing ? "编辑中" : (playing ? "播放中" : "已暂停"));
        ImGui::EndMenu();

    }

    if (ImGui::BeginMenu("视图"))
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
            ImGui::MenuItem("渲染设置", nullptr, &panel->visible);
        ImGui::Separator();
        if (ImGui::MenuItem("重置布局"))
            layoutNeedsReset = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("帮助"))
    {
        if (ImGui::BeginMenu("示例"))
        {
            const bool canLoad = state.documentWritable() && state.services.scene != nullptr;
            for (const ExampleEntry& example : kEditorExamples)
            {
                if (example.separatorBefore)
                    ImGui::Separator();
                if (ImGui::MenuItem(example.label, nullptr, false, canLoad))
                    requestExample(example.id);
            }
            ImGui::Separator();
            ImGui::TextDisabled("示例会替换当前场景；有未保存修改时先确认");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("键盘快捷键", "?", &showShortcutSheet);
        if (ImGui::MenuItem("打开控制台并跳到最近错误", nullptr, false, !state.lastErrorSummary.empty()))
            openConsoleAtLastError();
        if (ImGui::MenuItem("复制诊断信息"))
            copyDiagnostics();
        if (ImGui::MenuItem("关于 Tiny Editor"))
            showAboutWindow = true;
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
        state.notify(LogEntry::Level::Error, "创建角色失败：请查看 Console");
        return entt::null;
    }

    state.selection.select(entity);
    state.transformRefreshPending = true;
    state.markDirty();
    state.notifySuccess("已创建角色（运动学控制器）");
    return entity;
}

void EditorLayer::requestTerrainCreate()
{
    if (state.terrainCreatePending)
        return;      // 已经排队：连点不会叠出两块地形
    state.terrainCreatePending = true;
    state.terrainCreateWaitFrames = 0;
    // 这条提示是"已经收下请求"，不是"已经生成完"；完成时还会再来一条带尺寸与耗时的。
    state.notify(LogEntry::Level::Info, "正在生成地形…");
}

entt::entity EditorLayer::createTerrainFromUi()
{
    if (!state.services.scene || !state.services.registry)
        return entt::null;

    terrain::TerrainConfig config;
    // tech-debt #19：作业系统可用时就异步建 —— 实体当帧出现，几何后台合成，
    // 一帧内的主线程成本只剩"建实体 + 提交作业"。
    const bool asyncBuild = state.services.scene->supportsAsyncTerrainBuild();
    const entt::entity entity = createTerrainThroughCommand(
        *state.services.registry, *state.services.scene, state.commands,
        config, "Terrain", asyncBuild);

    if (entity == entt::null)
    {
        state.notify(LogEntry::Level::Error, "创建地形失败：请查看 Console");
        return entt::null;
    }

    state.selection.select(entity);
    state.markDirty();
    if (const auto* terrain = state.services.registry->try_get<TerrainComponent>(entity))
    {
        if (terrain->generating)
        {
            // 异步路径：几何还没好，先盯住它，完成后由 beginFrame 里的轮询播报。
            state.pendingTerrainBuild = entity;
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "地形已创建（后台生成 %ux%u）",
                          terrain->config.resolutionX, terrain->config.resolutionZ);
            state.notify(LogEntry::Level::Info, buffer);
        }
        else
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "[Editor] created Terrain | %ux%u, vertices=%u, indices=%u, %.2f ms",
                          terrain->config.resolutionX, terrain->config.resolutionZ,
                          terrain->vertexCount, terrain->indexCount,
                          terrain->lastGenerationMs);
            state.notifySuccess(buffer);
        }
    }
    return entity;
}
namespace
{
// E5：工具栏与状态栏的固定高度。面板默认矩形、视口矩形都要减掉它们，
// 所以必须是常量 —— 让它们按内容自适应的话，视口会跟着文字宽度抖动。
// UX-08：地形创建的两阶段请求。返回 true 表示"这一帧该真正执行了"。
// 第 1 次调用只把 waitFrames 抬到 1（这一帧用于把"生成中"画出来并 present），
// 第 2 次调用才返回 true 并清空状态。抽成自由函数是为了自检能直接驱动它。
bool advanceTerrainCreateRequest(bool& pending, int& waitFrames)
{
    if (!pending)
        return false;
    if (waitFrames < 1)
    {
        ++waitFrames;
        return false;
    }
    pending = false;
    waitFrames = 0;
    return true;
}

constexpr float kToolbarHeight = 34.0f;
constexpr float kStatusBarHeight = 26.0f;

// UX-19：布局常量要跟着字体一起放大。字体是按 content scale 加载的，
// 反过来用字号倒推缩放因子，就不用把 uiScale 从 ImGuiSystem 一路传进来。
// 100% 缩放的机器上恒为 1.0，布局与旧版本逐像素一致。
float uiScaleFactor()
{
    const float size = ImGui::GetFontSize();
    return size > 16.0f ? size / 16.0f : 1.0f;
}

float toolbarHeight() { return kToolbarHeight * uiScaleFactor(); }
float statusBarHeight() { return kStatusBarHeight * uiScaleFactor(); }

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

// UX-15：状态栏里的数量用 k/M 缩写（270.5k、2.7M），精确值放 tooltip。
void formatCount(char* out, size_t size, unsigned int value)
{
    if (value >= 1000000u)
        std::snprintf(out, size, "%.1fM", static_cast<double>(value) / 1000000.0);
    else if (value >= 10000u)
        std::snprintf(out, size, "%.1fk", static_cast<double>(value) / 1000.0);
    else
        std::snprintf(out, size, "%u", value);
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
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, toolbarHeight()), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 4.0f));
    if (ImGui::Begin("##toolbar", nullptr, kBarFlags))
    {
        // ---- 播放 ----
        const bool editing = state.playState == PlayState::Editing;
        const bool playing = state.playState == PlayState::Playing;

        // UX-07：命令层统一中文，面板标题保留英文区域名（它同时是 imgui.ini
        // 与偏好文件里的键）。E4-c 起会加载系统 CJK 字体，缺字形只在没装中文字体的机器上退化。
        if (ImGui::Button(editing ? "播放" : (playing ? "暂停" : "继续")))
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
        if (ImGui::Button("停止"))
            stopPlay();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("F7 停止并恢复播放前的场景");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // ---- 手柄模式 ----
        if (barToggle("移动", state.gizmo.mode == GizmoMode::Translate, "W：移动手柄"))
            state.gizmo.mode = GizmoMode::Translate;
        if (barToggle("旋转", state.gizmo.mode == GizmoMode::Rotate, "E：旋转手柄"))
            state.gizmo.mode = GizmoMode::Rotate;
        if (barToggle("缩放", state.gizmo.mode == GizmoMode::Scale, "R：缩放手柄"))
            state.gizmo.mode = GizmoMode::Scale;

        // ---- 吸附 ----
        ImGui::Checkbox("吸附", &state.gizmo.snapEnabled);
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
            if (barToggle("体素", state.voxelBrushEnabled,
                          "体素笔刷：Alt+左键挖掉方块，Alt+Shift+左键放置方块（Ctrl+左键始终是多选）"))
                state.voxelBrushEnabled = !state.voxelBrushEnabled;
            if (barToggle("渲染", renderPanel->visible, "渲染设置：曝光 / Bloom / SSAO / 方向光"))
                renderPanel->visible = !renderPanel->visible;
        }

        // ---- 聚焦 / 撤销 ----
        ImGui::BeginDisabled(state.selection.size() == 0);
        if (ImGui::Button("聚焦"))
            focusSelection(state);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("F：把相机退到让选中对象占满视野的位置");
        ImGui::SameLine();

        ImGui::BeginDisabled(!state.commands.canUndo());
        if (ImGui::Button("撤销"))
        {
            state.commands.undo();
            state.markDirty();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ctrl+Z");
        ImGui::SameLine();
        ImGui::BeginDisabled(!state.commands.canRedo());
        if (ImGui::Button("重做"))
        {
            state.commands.redo();
            state.markDirty();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ctrl+Y");
        ImGui::SameLine();

        // UX-08：同步生成会占满一帧，忙碌提示必须在此之前就画出来（两阶段请求）。
        // UX-08：同步生成会占满一帧，忙碌提示必须在此之前就画出来（两阶段请求）。
        // tech-debt #19：异步构建期间（pendingTerrainBuild）同样要显示。
        if (state.terrainCreatePending || state.pendingTerrainBuild != entt::null)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "地形生成中…");
            ImGui::SameLine();
        }

        // B3 / UX-12：Console 关着时，错误必须有可见入口。徽标常驻工具栏右侧。
        if (state.unreadErrors > 0)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.25f, 0.25f, 1.0f));
            char badge[48];
            std::snprintf(badge, sizeof badge, "错误 %u", static_cast<unsigned>(state.unreadErrors));
            if (ImGui::Button(badge))
            {
                openConsoleAtLastError();
                state.markErrorsRead();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("打开 Console 并跳到最近一次错误");
        }

        const char* playLabel = editing ? "编辑中" : (playing ? "播放中" : "已暂停");
        char right[192];
        std::snprintf(right, sizeof right, "%s · %s",
                      state.documentDirty ? "* 未保存" : "已保存", playLabel);
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

// B3：把「第几行错了」变成一次点击就能到达的地方。
// 三个入口（Toast、状态栏摘要、工具栏徽标）都走这里，保证行为一致。
bool EditorLayer::openConsoleAtLastError()
{
    EditorPanel* console = findPanel("Console");
    if (console == nullptr)
        return false;
    console->visible = true;
    state.focusConsoleRequest = true;
    state.scrollLogToBottom = true;
    return true;
}

void EditorLayer::drawToast()
{
    if (state.toast.remaining <= 0.0f || state.toast.text.empty())
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float maxWidth = viewport->WorkSize.x * 0.5f;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight() - 12.0f),
        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    bool clicked = false;
    if (ImGui::Begin("##toast", nullptr, flags))
    {
        const bool isError = state.toast.level == LogEntry::Level::Error;
        const bool isWarning = state.toast.level == LogEntry::Level::Warning;
        const ImVec4 color = isError ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
            : (isWarning ? ImVec4(1.0f, 0.85f, 0.35f, 1.0f)
                         : ImVec4(0.75f, 0.9f, 1.0f, 1.0f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxWidth);
        ImGui::TextColored(color, "%s", state.toast.text.c_str());
        ImGui::PopTextWrapPos();
        if (isError || isWarning)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("查看"))
            {
                clicked = true;
                state.markErrorsRead();
                openConsoleAtLastError();
            }
        }
    }
    ImGui::End();

    // 关闭按钮固定用 ImGui 的 NoInputs 之外的方式处理太绕，这里让「查看」就够了：
    // 不点它，Toast 也会在 5 秒后自己消失。
    (void)clicked;
}

// UX-17：打开失败不是终点。给「重试」和「换一个文件」两条出路，
// 而不是要求用户再走一遍 File > Open。
void EditorLayer::drawOpenFailurePrompt()
{
    if (!state.hasFailedOpen())
        return;

    if (!ImGui::IsPopupOpen("无法打开场景"))
        ImGui::OpenPopup("无法打开场景");

    if (ImGui::BeginPopupModal("无法打开场景", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", state.failedOpenPath.c_str());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(520.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", state.failedOpenError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextDisabled("文件可能被占用、路径变了，或者不是有效场景文件。");
        ImGui::Separator();

        const std::string retryPath = state.failedOpenPath;
        if (ImGui::Button("重试"))
        {
            state.clearFailedOpen();
            ImGui::CloseCurrentPopup();
            openScene(retryPath);
        }
        ImGui::SameLine();
        if (ImGui::Button("选择其它文件"))
        {
            state.clearFailedOpen();
            ImGui::CloseCurrentPopup();
            openSceneDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("关闭"))
        {
            state.clearFailedOpen();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::drawStatusBar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barY = viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, barY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight()), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
    if (ImGui::Begin("##statusbar", nullptr, kBarFlags))
    {
        // UX-15：状态栏分三段，各管一件事 ——
        //   左：选中 / 手柄 / 吸附 / 最近错误（"我现在在编辑什么"）
        //   中：场景统计（"屏幕上画了多少东西"）
        //   右：帧率 / 帧耗时 / 撤销深度（"跑得快不快"）
        // 以前五组数字挤在右边一行，扫读时要一字一字读；分三段后眼睛有落点。
        // E8：模式不再在状态栏重复显示；视口左上角的徽标是唯一常驻入口。
        // 这里只保留当前模式下"左键会发生什么"的上下文说明。
        if (state.isRuntime())
        {
            ImGui::TextDisabled("左键挖方块 / 右键放方块（F8 回编辑）");
        }
        else
        {
            const size_t selected = state.selection.size();
            if (selected == 0)
                ImGui::TextDisabled("没有选中对象");
            else
                ImGui::Text("%zu 个对象选中", selected);
            ImGui::SameLine();
            ImGui::TextDisabled("· %s · 吸附 %s", gizmoModeLabel(state.gizmo.mode),
                                state.gizmo.snapEnabled ? "开" : "关（按住 Shift 临时开）");
        }

        // B3 / UX-03、UX-12：错误摘要常驻在状态栏 —— Console 关着也看得见。
        // 30 秒后不算「最近」，颜色变淡但文字保留，直到下一次成功动作把它清掉。
        if (!state.lastErrorSummary.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("·");
            ImGui::SameLine();
            const float alpha = state.lastErrorAge < 30.0f ? 1.0f : 0.55f;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, alpha));
            if (ImGui::SmallButton("错误"))
            {
                openConsoleAtLastError();
                state.markErrorsRead();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.6f, alpha));
            ImGui::TextUnformatted(state.lastErrorSummary.c_str());
            ImGui::PopStyleColor();
        }

        const ImGuiIO& io = ImGui::GetIO();
        unsigned int draws = 0;
        unsigned int tris = 0;
        if (state.services.scene != nullptr)
        {
            // beginFrameTelemetry() 每帧开头把上一帧的完整统计搬进 displayTelemetry，
            // 这里必须读它：读 currentTelemetry 的话，UI 先于渲染录制，永远是 0。
            const RenderTelemetry& telemetry = state.services.scene->getDisplayTelemetry();
            draws = telemetry.drawCalls;
            tris = telemetry.triangles;
        }

        // ---- 中区：场景统计。数字用 k/M 缩写，精确值挂在 tooltip 上 ----
        char triText[32];
        formatCount(triText, sizeof triText, tris);
        char center[128];
        std::snprintf(center, sizeof center, "绘制 %u · 三角形 %s", draws, triText);

        const float centerWidth = ImGui::CalcTextSize(center).x;
        const float centerX = viewport->WorkSize.x * 0.45f;
        if (centerX > ImGui::GetCursorPosX() + 8.0f)
        {
            ImGui::SameLine(centerX);
            ImGui::TextUnformatted(center);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("draw calls %u · triangles %u", draws, tris);
        }

        // ---- 右区：帧率 / 帧耗时 / 撤销深度 ----
        char right[128];
        std::snprintf(right, sizeof right, "%.0f FPS · %.2f ms · 撤销 %d",
                      static_cast<double>(io.Framerate),
                      (io.Framerate > 0.0f) ? (1000.0f / io.Framerate) : 0.0f,
                      static_cast<int>(state.commands.undoCount()));
        const float textWidth = ImGui::CalcTextSize(right).x;
        const float targetX = ImGui::GetWindowWidth() - textWidth - 10.0f;
        if (targetX > ImGui::GetCursorPosX() + 8.0f)
            ImGui::SameLine(targetX);
        ImGui::TextDisabled("%s", right);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// UX-18：诊断入口 —— 出错后用户至少要有一条「把现场带走」的路。
// 文本走剪贴板而不是文件：贴进 issue / 聊天窗口是最常见的下一步。
void EditorLayer::copyDiagnostics()
{
    const ImGuiIO& io = ImGui::GetIO();
    unsigned int draws = 0;
    unsigned int tris = 0;
    unsigned int verts = 0;
    if (state.services.scene != nullptr)
    {
        const RenderTelemetry& telemetry = state.services.scene->getDisplayTelemetry();
        draws = telemetry.drawCalls;
        tris = telemetry.triangles;
        verts = telemetry.vertices;
    }

    char buffer[768];
    std::snprintf(buffer, sizeof buffer,
                  "Tiny Editor 诊断%s%s" "\n"
                  "场景：%s\n"
                  "FPS：%.1f（%.2f ms）\n"
                  "draw calls：%u，triangles：%u，vertices：%u\n"
                  "撤销深度：%d（可重做 %d）\n"
                  "最近错误：%s",
                  state.documentPath.empty() ? "" : "：",
                  state.documentPath.empty() ? "（未命名场景）" : state.documentPath.c_str(),
                  state.services.scene != nullptr ? "已加载" : "不可用",
                  static_cast<double>(io.Framerate),
                  (io.Framerate > 0.0f) ? (1000.0f / io.Framerate) : 0.0f,
                  draws, tris, verts,
                  static_cast<int>(state.commands.undoCount()),
                  static_cast<int>(state.commands.redoCount()),
                  state.lastErrorSummary.empty() ? "无" : state.lastErrorSummary.c_str());
    ImGui::SetClipboardText(buffer);
    state.notifySuccess("诊断信息已复制到剪贴板");
}

// UX-18：关于窗顺带把「语言策略」写进去 —— 这是用户第一次见到项目时最常问的那件事。
void EditorLayer::drawAboutWindow()
{
    if (!showAboutWindow)
        return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("关于 Tiny Editor", &showAboutWindow, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("Tiny Engine —— Vulkan 渲染的轻量场景编辑器");
    ImGui::Separator();
    ImGui::TextDisabled("界面用词：命令与提示中文，面板名与行业参数保留英文。");
    ImGui::TextDisabled("快捷键：Shift+/ 打开速查表。");
    ImGui::TextDisabled("渲染设置：工具栏「渲染」按钮。");
    if (ImGui::Button("复制诊断信息"))
        copyDiagnostics();
    ImGui::End();
}
void EditorLayer::drawPanels()
{
    // 关键约束：ImGui::SetNextWindowPos/Size 是**单槽位**的 ——
    // 连续调用多次只有最后一次生效，而且会被"下一个 Begin 的窗口"消费。
    // 所以绝不能先把三个面板的位置都设置一遍再统一绘制
    // （那样 Console 的矩形会套到 Hierarchy 上，另外两个面板退回默认位置）。
    // 正确做法：在**每个面板 Begin 之前紧邻**设置它自己的矩形。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // UX-19：侧栏宽度也跟字体缩放走，150% 下中文才不会被挤成两行。
    const float leftWidth = 280.0f * uiScaleFactor();
    const float rightWidth = 340.0f * uiScaleFactor();
    const float bottomHeight = 200.0f;

    const float top = viewport->WorkPos.y + toolbarHeight();
    const float availableWidth = viewport->WorkSize.x;
    const float availableHeight = viewport->WorkSize.y - toolbarHeight() - statusBarHeight();

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
            // UX-14：和其他面板一样用 cond（首帧 / 重置布局时落位），之后允许拖动与缩放，
            // 位置由 imgui.ini 记住。View -> Reset Layout 会把 layoutNeedsReset 置真，
            // 下一帧用 Always 重新铺一遍。
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + leftWidth, top), cond);
            ImGui::SetNextWindowSize(ImVec2(availableWidth - leftWidth - rightWidth,
                                            availableHeight - bottomHeight), cond);
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

    // 首帧的视口矩形初值；真正绘制后由 ViewportPanel 用窗口实际矩形覆盖（UX-14 起视口可拖动）。
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
bool EditorLayer::newScene(bool announce)
{
    if (!state.documentWritable())
    {
        state.notify(LogEntry::Level::Warning, "播放中不能新建场景：先按 F7 停止");
        return false;
    }

    if (!state.services.scene)
        return false;

    state.services.scene->clearEntities();
    // 新建场景恢复整套场景功能组件（用当前引擎设置作为默认值）。
    state.services.scene->resetSceneComponentsFromSettings();
    state.selection.clear();
    state.commands.clear();
    state.documentPath.clear();
    state.documentDirty = false;
    state.pendingExampleId.clear();
    if (announce)
        state.notifySuccess("新建空场景");
    return true;
}

void EditorLayer::requestExample(const char* id)
{
    if (id == nullptr || !isKnownExampleId(id))
        return;
    state.pendingExampleId = id;
    requestDocumentAction(EditorState::PendingDocumentAction::Example);
}

bool EditorLayer::loadExample(const std::string& id)
{
    Scene* scene = state.services.scene;
    if (scene == nullptr || !isKnownExampleId(id))
    {
        state.notify(LogEntry::Level::Error, "示例加载失败：未知示例 " + id);
        return false;
    }

    // 运行模式下相机由角色接管、世界可能已被玩家改动；先回编辑模式。
    // setEditorMode 会把相机恢复到进入运行模式前的位姿，后面的预设再覆盖它。
    if (state.isRuntime())
        setEditorMode(EditorState::EditorMode::Edit);

    // 示例等同"打开一份新场景"，复用同一条新建路径；未保存确认已经在
    // requestDocumentAction 里做过。这里不播报"新建空场景"，
    // 免得和紧随其后的示例提示叠在一起。
    if (!newScene(false))
        return false;

    state.voxelWorld = entt::null;
    state.pendingTerrainBuild = entt::null;
    state.terrainCreatePending = false;
    state.terrainCreateWaitFrames = 0;
    state.selection.clear();
    state.commands.clear();

    const auto placeCamera = [scene](const glm::vec3& position, const glm::vec3& euler)
    {
        TransformComponent& camera = scene->getCameraTransform();
        camera.setTranslation(position);
        camera.setEuler(euler);
        camera.setScale(glm::vec3(1.0f));
        scene->markTransformDirty(scene->getCameraEntity());
    };
    constexpr float kPi = 3.14159265358979323846f;

    if (id == "voxel-glass" || id == "voxel-world")
    {
        const entt::entity world = scene->createVoxelWorld(20251104ull, 1, 0);
        if (world == entt::null)
        {
            state.notify(LogEntry::Level::Error, "示例加载失败：体素世界创建失败");
            return false;
        }
        state.voxelWorld = world;

        // 首轮体素网格是异步生成的：先等全部落地再摆玻璃/相机，否则
        // placeVoxelGlassDemo() 写入的玻璃会被仍在跑的旧作业用旧网格覆盖。
        scene->waitForVoxelBuilds();

        if (id == "voxel-glass")
        {
            const int32_t blocks = scene->placeVoxelGlassDemo(world);
            if (blocks <= 0)
            {
                state.notify(LogEntry::Level::Error, "示例加载失败：玻璃演示没有写入方块");
                return false;
            }
            placeCamera(glm::vec3(16.0f, 22.0f, 54.0f), glm::vec3(0.20f, kPi, 0.0f));
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "示例：体素玻璃（%d 个方块，观察透明排序）", blocks);
            state.notifySuccess(buffer);
        }
        else
        {
            placeCamera(glm::vec3(16.0f, 46.0f, 118.0f), glm::vec3(0.42f, kPi, 0.0f));
            state.notifySuccess("示例：体素世界（3×3 chunk）");
        }
    }
    else if (id == "terrain")
    {
        // createTerrainFromUi 用相机前方定位地形中心，所以先把相机摆好；
        // 这里不用 requestTerrainCreate 的两帧延迟，否则示例会先显示一帧空场景。
        placeCamera(glm::vec3(0.0f, 42.0f, 90.0f), glm::vec3(0.42f, kPi, 0.0f));
        if (createTerrainFromUi() == entt::null)
            return false;
        state.selection.clear();
        state.commands.clear();
    }
    else if (id == "streaming")
    {
        // 流送地形围绕相机做初始加载，相机必须在地形创建之前就位。
        placeCamera(glm::vec3(0.0f, 18.0f, 56.0f), glm::vec3(0.30f, kPi, 0.0f));

        terrain::TerrainConfig config;
        config.seed = 9001;
        config.resolutionX = 33;
        config.resolutionZ = 33;
        config.sizeX = 32.0f;
        config.sizeZ = 32.0f;
        config.heightScale = 12.0f;
        config.lodLevelCount = 3;
        config.skirtsEnabled = false;
        config.localColorRange = false;

        const entt::entity streaming = scene->createStreamingTerrain(
            config, 3, 4, "StreamingWorld（示例）");
        if (streaming == entt::null)
        {
            state.notify(LogEntry::Level::Error, "示例加载失败：流送地形创建失败");
            return false;
        }
        state.notifySuccess("示例：流送地形（移动相机时会继续加载周边 chunk）");
    }

    // 示例本身视为未修改文档：浏览时切换/新建不该反复弹"未保存"。
    state.documentDirty = false;
    state.transformRefreshPending = true;
    return true;
}

bool EditorLayer::saveScene()
{
    // 播放期间场景是运行期状态，写盘会把玩家跑出来的位姿存进文档。
    if (!state.documentWritable())
    {
        state.notify(LogEntry::Level::Warning, "播放中不写入文档：先按 F7 停止再保存");
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
        state.notify(LogEntry::Level::Error, "保存失败：" + error);
        return false;
    }

    state.documentDirty = false;
    state.notifySuccess("已保存 " + state.documentPath);
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

bool EditorLayer::savePrefabFromSelection()
{
    if (!state.documentWritable())
    {
        state.notify(LogEntry::Level::Warning, "播放中不写入预制体：先按 F7 停止");
        return false;
    }
    if (!state.services.scene || !state.services.saveFileDialog)
        return false;

    const entt::entity root = state.selection.primary();
    if (root == entt::null)
    {
        state.notify(LogEntry::Level::Warning, "先选中一个实体，再保存为预制体");
        return false;
    }

    const std::string path = state.services.saveFileDialog(
        "Tiny Engine Prefab (*.prefab.json)\0*.prefab.json\0All Files (*.*)\0*.*\0",
        "Save Prefab As");
    if (path.empty())
        return false;

    std::string error;
    if (!prefab::save(*state.services.scene, root, path, error))
    {
        state.notify(LogEntry::Level::Error, "保存预制体失败：" + error);
        return false;
    }

    state.notifySuccess("已保存预制体 " + std::filesystem::path(path).filename().string());
    return true;
}

bool EditorLayer::instantiatePrefabDialog()
{
    if (!state.documentWritable())
    {
        state.notify(LogEntry::Level::Warning, "播放中不能实例化预制体：先按 F7 停止");
        return false;
    }
    if (!state.services.scene || !state.services.openFileDialog)
        return false;

    const std::string path = state.services.openFileDialog(
        "Tiny Engine Prefab (*.prefab.json)\0*.prefab.json\0All Files (*.*)\0*.*\0",
        "Instantiate Prefab");
    if (path.empty())
        return false;

    scene::SceneDescription description;
    std::string error;
    if (!prefab::load(path, description, error))
    {
        state.notify(LogEntry::Level::Error, "读取预制体失败：" + error);
        return false;
    }

    auto command = std::make_unique<InstantiatePrefabCommand>(
        *state.services.scene, description, entt::null);
    InstantiatePrefabCommand* raw = command.get();
    if (!raw->succeeded())
    {
        state.notify(LogEntry::Level::Error, "实例化失败：" + raw->errorMessage());
        return false;
    }

    const entt::entity root = raw->currentRoot();
    state.commands.execute(std::move(command));
    state.selection.select(root);
    state.markDirty();
    state.notifySuccess("已实例化预制体 " + std::filesystem::path(path).filename().string());
    return true;
}

bool EditorLayer::openScene(const std::string& path)
{
    std::string error;
    if (!loadSceneFromPath(path, error))
    {
        // B3：错误要同时进 Toast / 状态栏 / Console，并且留下重试入口（UX-17）。
        state.notify(LogEntry::Level::Error, "打开失败：" + error);
        state.failedOpenPath = path;
        state.failedOpenError = error;
        // 不在这里 OpenPopup：openScene 可能由启动参数在 ImGui 帧之外调用，
        // OpenPopup 必须在帧内。真正的弹窗由 drawOpenFailurePrompt 每帧检查。
        return false;
    }
    state.notifySuccess("已打开 " + path);
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
void EditorLayer::setEditorMode(EditorState::EditorMode mode)
{
    if (state.mode == mode)
        return;
    state.mode = mode;
    if (mode == EditorState::EditorMode::Runtime)
    {
        // 运行模式不改数据也不改选中：把选中清掉，Inspector 自然变成空态。
        state.selection.clear();
        enterRuntimeMode();
        state.notify(LogEntry::Level::Info, "运行模式：左键挖方块 / 地形笔刷，右键放方块（F8 回编辑模式）");
    }
    else
    {
        exitRuntimeMode();
        state.notify(LogEntry::Level::Info, "编辑模式：左键选中、手柄拖拽、面板改参数");
    }
}

void EditorLayer::startInRuntimeMode()
{
    setEditorMode(EditorState::EditorMode::Runtime);
}

void EditorLayer::enterRuntimeMode()
{
    Scene* scene = state.services.scene;
    entt::registry* registry = state.services.registry;
    if (scene == nullptr || registry == nullptr || state.runtimeCharacter != entt::null)
        return;

    TransformComponent& camera = scene->getCameraTransform();
    state.runtimeSavedCameraPosition = camera.getTranslation();
    state.runtimeSavedCameraEuler = camera.getEuler();
    // 相机 euler 用的是弧度；runtimeYaw/runtimePitch 必须原样保存，
    // 不能再按“度”转一次，否则角度会缩小约 57 倍（W/S/A/D 全部反向）。
    state.runtimeYaw = camera.getEuler().y;
    state.runtimePitch = glm::clamp(camera.getEuler().x,
        -glm::radians(85.0f), glm::radians(85.0f));

    // 出生点优先取「相机视线打到地面的位置」，这样俯视地形/体素世界时
    // 角色会站进画面里，而不是落在镜头前方两米的空气里一路坠落。
    const float cosPitch = std::cos(state.runtimePitch);
    const glm::vec3 viewForward{
        std::sin(state.runtimeYaw) * cosPitch,
        -std::sin(state.runtimePitch),
        std::cos(state.runtimeYaw) * cosPitch
    };
    glm::vec3 spawn = camera.getTranslation() +
        glm::vec3(std::sin(state.runtimeYaw), 0.0f, std::cos(state.runtimeYaw)) * 2.0f;
    spawn.y -= 1.62f;
    bool spawnOnGround = false;
    for (float distance = 2.0f; distance <= 180.0f; distance += 1.0f)
    {
        const glm::vec3 probe = camera.getTranslation() + viewForward * distance;
        float ground = 0.0f;
        entt::entity groundEntity = entt::null;
        if (scene->sampleTerrainGroundHeightWorld(probe.x, probe.z, ground, &groundEntity) &&
            probe.y <= ground + 0.1f && probe.y >= ground - 6.0f)
        {
            spawn = glm::vec3(probe.x, ground, probe.z);
            spawnOnGround = true;
            break;
        }
    }
    if (!spawnOnGround)
    {
        float ground = 0.0f;
        entt::entity groundEntity = entt::null;
        if (scene->sampleTerrainGroundHeightWorld(spawn.x, spawn.z, ground, &groundEntity))
        {
            spawn.y = ground;
            spawnOnGround = true;
        }
    }

    state.runtimeCharacter = scene->createCharacterEntity("Runtime Player", spawn);
    // 初始朝向与进入运行模式时的视线一致，避免角色先面向镜头再被第一步转过去。
    registry->get<TransformComponent>(state.runtimeCharacter).setEuler(
        glm::vec3(0.0f, state.runtimeYaw, 0.0f));
    std::cout << "[Runtime] spawn=" << spawn.x << "," << spawn.y << "," << spawn.z
              << " onGround=" << (spawnOnGround ? 1 : 0) << std::endl;
    const bool modelAttached = scene->attachCharacterModel(
        state.runtimeCharacter,
        std::string(TINY_RESOURCE_DIR) + "Models/Character/Character.glb",
        0.5f);
    state.notify(LogEntry::Level::Info,
        modelAttached
            ? "运行模式：WASD 走、Shift 跑、空格跳、中键拖动看、左键挖 / 右键放（F8 回编辑）"
            : "运行模式：角色模型加载失败，使用空控制器（WASD 走、空格跳、F8 回编辑）");
}

void EditorLayer::exitRuntimeMode()
{
    Scene* scene = state.services.scene;
    entt::registry* registry = state.services.registry;
    if (scene == nullptr || registry == nullptr)
        return;

    if (state.runtimeCharacter != entt::null && registry->valid(state.runtimeCharacter))
        scene->destroyEntity(state.runtimeCharacter);
    state.runtimeCharacter = entt::null;

    TransformComponent& camera = scene->getCameraTransform();
    camera.setTranslation(state.runtimeSavedCameraPosition);
    camera.setEuler(state.runtimeSavedCameraEuler);
    scene->markTransformDirty(scene->getCameraEntity());
}

void EditorLayer::applyRuntimeCameraPose()
{
    Scene* scene = state.services.scene;
    entt::registry* registry = state.services.registry;
    if (scene == nullptr || registry == nullptr || state.runtimeCharacter == entt::null ||
        !registry->valid(state.runtimeCharacter))
        return;

    const TransformComponent* feet = registry->try_get<TransformComponent>(state.runtimeCharacter);
    if (feet == nullptr)
        return;

    // 第三人称：相机沿“视线反方向”退到角色身后，焦点在胸口高度。
    // 引擎相机的前方是 +w = (sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch))，
    // pitch 为正表示向下看。
    const glm::vec3 feetPosition = feet->getTranslation();
    const glm::vec3 focus = feetPosition + glm::vec3(0.0f, 1.25f, 0.0f);
    const float cosPitch = std::cos(state.runtimePitch);
    const glm::vec3 viewForward{
        std::sin(state.runtimeYaw) * cosPitch,
        -std::sin(state.runtimePitch),
        std::cos(state.runtimeYaw) * cosPitch
    };
    glm::vec3 cameraPosition = focus - viewForward * 4.0f;
    cameraPosition.y = std::max(cameraPosition.y, feetPosition.y + 0.6f);

    TransformComponent& camera = scene->getCameraTransform();
    camera.setTranslation(cameraPosition);
    camera.setEuler(glm::vec3(state.runtimePitch, state.runtimeYaw, 0.0f));
    scene->markTransformDirty(scene->getCameraEntity());
}

void EditorLayer::updateRuntimeMode(float dt)
{
    Scene* scene = state.services.scene;
    entt::registry* registry = state.services.registry;
    if (!state.isRuntime() || scene == nullptr || registry == nullptr ||
        state.runtimeCharacter == entt::null || !registry->valid(state.runtimeCharacter) ||
        dt <= 0.0f)
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
    {
        // 与 CameraController 保持同一套符号：dx 向右转，dy 向下看。
        state.runtimeYaw += io.MouseDelta.x * 0.01f;
        state.runtimePitch = glm::clamp(state.runtimePitch + io.MouseDelta.y * 0.01f,
            -glm::radians(85.0f), glm::radians(85.0f));
    }

    glm::vec3 move{ 0.0f };
    if (!io.WantTextInput)
    {
        const glm::vec3 forward{ std::sin(state.runtimeYaw), 0.0f, std::cos(state.runtimeYaw) };
        const glm::vec3 right{ std::cos(state.runtimeYaw), 0.0f, -std::sin(state.runtimeYaw) };
        if (ImGui::IsKeyDown(ImGuiKey_W)) move += forward;
        if (ImGui::IsKeyDown(ImGuiKey_S)) move -= forward;
        if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
    }

    const bool jump = !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Space);
    // Win32/远程桌面下 Shift 可能只上报通用修饰键，不一定落到 LeftShift/RightShift
    // 两个具体键上，所以 io.KeyShift 也要认。
    const bool sprint = !io.WantTextInput &&
        (io.KeyShift || ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
         ImGui::IsKeyDown(ImGuiKey_RightShift));
    CharacterMoveResult result;
    if (scene->updateCharacterController(state.runtimeCharacter, move, jump, dt, result,
                                         sprint ? 1.35f : 0.55f))
        state.transformRefreshPending = true;

    applyRuntimeCameraPose();

    static std::string lastRuntimeClip;
    const auto* animation = registry->try_get<CharacterAnimationComponent>(state.runtimeCharacter);
    if (animation != nullptr && animation->currentClipName != lastRuntimeClip)
    {
        lastRuntimeClip = animation->currentClipName;
        const glm::vec3 feet = registry->get<TransformComponent>(state.runtimeCharacter).getTranslation();
        std::cout << "[Runtime] clip=" << lastRuntimeClip
                  << " feet=" << feet.x << "," << feet.y << "," << feet.z
                  << " grounded=" << (result.grounded ? 1 : 0) << "\n";
    }
}

void EditorLayer::enterPlay()
{
    if (!state.services.scene || state.playState != PlayState::Editing)
        return;

    // 播放前必须先把"未保存的修改"落进快照，否则 Stop 会丢改动
    state.playSnapshot = state.services.scene->captureDescription();
    state.hasPlaySnapshot = true;
    state.playState = PlayState::Playing;
    state.commands.clear();          // 运行期的操作不该能撤销回编辑态
    state.notify(LogEntry::Level::Info, "开始播放（F7 停止并恢复）");
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
    state.notifySuccess("已停止播放，场景已恢复");
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
    case EditorState::PendingDocumentAction::Example:
    {
        const std::string id = state.pendingExampleId;
        state.pendingExampleId.clear();
        loadExample(id);
        break;
    }
    case EditorState::PendingDocumentAction::None:
        break;
    }
}

void EditorLayer::cancelPendingDocumentAction()
{
    state.pendingDocumentAction = EditorState::PendingDocumentAction::None;
    state.pendingExampleId.clear();
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
            const std::string keys = shortcutKeys(entry);
            ImGui::TextUnformatted(keys.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.actionText);
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
        state.pendingExampleId.clear();
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

    // ---- 0ad) E6：资产拖放落点 + 贴图库扩展名过滤 ----
    {
        const glm::vec3 camera{ 10.0f, 5.0f, 20.0f };
        const glm::vec3 forward{ 0.0f, 0.0f, 1.0f };
        const glm::vec3 groundHit{ 3.0f, 1.5f, 7.0f };
        const glm::vec3 onTerrain = ViewportPanel::assetDropPosition(true, groundHit, camera, forward);
        const glm::vec3 inAir = ViewportPanel::assetDropPosition(false, groundHit, camera, forward);
        const bool terrainUsesHit = onTerrain == groundHit;
        const bool airUsesCameraFront = inAir == camera + forward * 8.0f;
        if (!terrainUsesHit || !airUsesCameraFront)
        {
            std::cerr << "[Editor] asset drop selfTest FAIL | terrain=" << terrainUsesHit
                      << " air=" << airUsesCameraFront << std::endl;
            return false;
        }
        std::cout << "[Editor] asset drop selfTest PASS | terrain hit point, otherwise camera + 8"
                  << std::endl;
    }

    if (!TextureLibrary::selfTest())
        return false;

    // ---- 0ab) B6：Console 过滤判据（级别开关 + 关键字大小写折叠） ----
    if (!ConsolePanel::selfTest())
        return false;

    // ---- 0ac) B6：地形创建的两阶段请求（先画忙碌帧，再执行） ----
    {
        bool pending = true;
        int waitFrames = 0;
        const bool firstCallRunsNow = advanceTerrainCreateRequest(pending, waitFrames);
        const bool firstFrameStillQueued = pending && waitFrames == 1;
        const bool secondCallRunsNow = advanceTerrainCreateRequest(pending, waitFrames);
        const bool clearedAfterRun = !pending && waitFrames == 0;
        const bool idleStaysIdle = !advanceTerrainCreateRequest(pending, waitFrames);
        if (firstCallRunsNow || !firstFrameStillQueued || !secondCallRunsNow ||
            !clearedAfterRun || !idleStaysIdle)
        {
            std::cerr << "[Editor] terrain create request selfTest FAIL | run1=" << firstCallRunsNow
                      << " queued=" << firstFrameStillQueued << " run2=" << secondCallRunsNow
                      << " cleared=" << clearedAfterRun << " idle=" << idleStaysIdle << std::endl;
            return false;
        }
        std::cout << "[Editor] terrain create request selfTest PASS | busy frame first, build on the next"
                  << std::endl;
    }

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

    // ---- 0ab) B3：通知状态机（Toast / 最近错误 / 未读计数 / 打开失败重试） ----
    {
        EditorState probe;
        probe.notify(LogEntry::Level::Error, "boom");
        const bool raised = probe.toast.remaining > 0.0f && probe.unreadErrors == 1 &&
            probe.lastErrorSummary == "boom" && probe.lastErrorLogIndex == 0;

        probe.tickNotices(1.0f);
        const bool ticking = probe.toast.remaining > 0.0f && probe.lastErrorAge > 0.9f;

        probe.tickNotices(10.0f);
        const bool expired = probe.toast.remaining == 0.0f;

        probe.notify(LogEntry::Level::Error, "boom 2");
        const bool counted = probe.unreadErrors == 2;
        probe.notifySuccess("ok");
        const bool cleared = probe.lastErrorSummary.empty() && probe.toast.remaining > 0.0f;
        const bool stillUnread = probe.unreadErrors == 2;

        probe.failedOpenPath = "missing.scene.json";
        probe.failedOpenError = "file not found";
        const bool failedOpen = probe.hasFailedOpen();
        probe.clearFailedOpen();
        const bool failedCleared = !probe.hasFailedOpen();

        if (!raised || !ticking || !expired || !counted || !cleared || !stillUnread ||
            !failedOpen || !failedCleared)
        {
            std::cerr << "[Editor] notices selfTest FAIL | raised=" << raised
                << " ticking=" << ticking << " expired=" << expired
                << " counted=" << counted << " cleared=" << cleared
                << " stillUnread=" << stillUnread
                << " failedOpen=" << failedOpen << " failedCleared=" << failedCleared << std::endl;
            return false;
        }
        std::cout << "[Editor] notices selfTest PASS | toast decay + error summary + unread badge" << std::endl;
    }

    // ---- 0ac) B5：状态栏数量缩写 + 日志时间戳（纯逻辑） ----
    {
        char small[16];
        char kilo[16];
        char mega[16];
        formatCount(small, sizeof small, 42u);
        formatCount(kilo, sizeof kilo, 270459u);
        formatCount(mega, sizeof mega, 2704590u);
        if (std::strcmp(small, "42") != 0 || std::strcmp(kilo, "270.5k") != 0 ||
            std::strcmp(mega, "2.7M") != 0)
        {
            std::cerr << "[Editor] status bar selfTest FAIL: count formatting " << small << " / "
                      << kilo << " / " << mega << std::endl;
            return false;
        }
        const std::string clock = currentLogClock();
        if (clock.size() != 8 || clock[2] != ':' || clock[5] != ':')
        {
            std::cerr << "[Editor] status bar selfTest FAIL: log clock " << clock << std::endl;
            return false;
        }
        std::cout << "[Editor] status bar selfTest PASS | counts k/M + log clock " << clock
                  << std::endl;
    }

    // ---- 0b) E4-b：偏好文件往返（纯逻辑，不碰磁盘） ----
    if (!EditorPreferences::selfTest())
        return false;

    // ---- 0c) E4-c / UX-13：快捷键表是唯一数据源，顺手校验没有空项与键位冲突 ----
    {
        if (kShortcutEntryCount < 12)
        {
            std::cerr << "[Editor] shortcut sheet selfTest FAIL: table too small" << std::endl;
            return false;
        }
        for (size_t i = 0; i < kShortcutEntryCount; ++i)
        {
            const ShortcutEntry& entry = kShortcutEntries[i];
            if (entry.group == nullptr || entry.actionText == nullptr ||
                entry.group[0] == 0 || entry.actionText[0] == 0)
            {
                std::cerr << "[Editor] shortcut sheet selfTest FAIL: empty entry at " << i << std::endl;
                return false;
            }
            if (shortcutKeys(entry).empty())
            {
                std::cerr << "[Editor] shortcut sheet selfTest FAIL: no key label at " << i << std::endl;
                return false;
            }
            for (size_t j = i + 1; j < kShortcutEntryCount; ++j)
            {
                const ShortcutEntry& other = kShortcutEntries[j];
                if (entry.key == other.key && entry.ctrl == other.ctrl && entry.shift == other.shift)
                {
                    std::cerr << "[Editor] shortcut sheet selfTest FAIL: conflicting binding "
                              << shortcutKeys(entry) << std::endl;
                    return false;
                }
            }
        }
        std::cout << "[Editor] shortcut sheet selfTest PASS | entries=" << kShortcutEntryCount
                  << " conflicts=0" << std::endl;
    }

    // ---- 0e) E8：编辑 / 运行模式是输入归属开关 ----
    {
        EditorState probe;
        if (!probe.allowsSelection() || probe.isRuntime())
        {
            std::cerr << "[Editor] editor mode selfTest FAIL: edit mode must allow selection" << std::endl;
            return false;
        }
        probe.mode = EditorState::EditorMode::Runtime;
        if (probe.allowsSelection() || !probe.isRuntime())
        {
            std::cerr << "[Editor] editor mode selfTest FAIL: runtime mode must block selection" << std::endl;
            return false;
        }
        bool hasToggle = false;
        for (size_t i = 0; i < kShortcutEntryCount; ++i)
            hasToggle = hasToggle || kShortcutEntries[i].action == ShortcutAction::ToggleMode;
        if (!hasToggle)
        {
            std::cerr << "[Editor] editor mode selfTest FAIL: no shortcut for mode toggle" << std::endl;
            return false;
        }
        std::cout << "[Editor] editor mode selfTest PASS | runtime blocks selection, edit allows" << std::endl;
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
        // v3：六种 PropertyType 都要能进 JSON 再原值回来。这里用未注册的
        // 类型名，序列化层只负责语法往返；注册与否由 Scene 加载时决定。
        scene::ComponentRecord probe;
        probe.type = "test.sixTypes";
        probe.properties.push_back({ "flag", "Flag", scene::PropertyValue::fromBool(true) });
        probe.properties.push_back({ "count", "Count", scene::PropertyValue::fromInt(-7) });
        probe.properties.push_back({ "speed", "Speed", scene::PropertyValue::fromFloat(1.25f) });
        probe.properties.push_back({ "offset", "Offset", scene::PropertyValue::fromVec3(glm::vec3(0.5f, -2.0f, 3.25f)) });
        probe.properties.push_back({ "tint", "Tint", scene::PropertyValue::fromColor(glm::vec4(0.25f, 0.5f, 0.75f, 1.0f)) });
        probe.properties.push_back({ "note", "Note", scene::PropertyValue::fromString("ecs") });
        a.components.push_back(std::move(probe));
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
        if (output.entities[0].components.size() != 1 ||
            !output.entities[0].components[0].equivalentTo(input.entities[0].components[0]))
        {
            std::cerr << "[Editor] selfTest FAIL: component record not preserved in round trip\n";
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

    // ---- 2j) F3：挖一格 / 放回去（VoxelEditCommand）----
    {
        Scene* testScene = state.services.scene;
        if (testScene == nullptr)
        {
            std::cerr << "[Editor] selfTest FAIL: voxel edit requires a scene" << std::endl;
            return false;
        }

        const entt::entity voxelWorld = testScene->createVoxelWorld(9090ull, 0, 0);
        testScene->waitForVoxelBuilds();
        if (voxelWorld == entt::null)
        {
            std::cerr << "[Editor] selfTest FAIL: voxel world creation failed" << std::endl;
            return false;
        }

        glm::ivec3 hitBlock(0);
        glm::ivec3 hitNormal(0);
        float hitDistance = 0.0f;
        const bool hit = testScene->raycastVoxelWorld(
            glm::vec3(16.5f, 60.0f, 16.5f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f,
            hitBlock, hitNormal, hitDistance, nullptr, voxelWorld);

        const uint8_t before = testScene->getVoxelBlock(
            voxelWorld, hitBlock.x, hitBlock.y, hitBlock.z);

        // 视口里点中 chunk 实体时必须解析到世界管理器，否则删除/变换会落到
        // 一个随时会被重建的内部实体上（Ctrl+左键多选与笔刷也因此不打架）。
        entt::entity pickedChunk = entt::null;
        for (auto [entity, chunk] : registry.view<VoxelChunkComponent>().each())
        {
            if (chunk.world == voxelWorld)
            {
                pickedChunk = entity;
                break;
            }
        }
        const bool resolvesToWorld = pickedChunk != entt::null &&
            testScene->resolveVoxelChunkOwner(pickedChunk) == voxelWorld;

        CommandStack voxelStack;
        voxelStack.execute(std::make_unique<VoxelEditCommand>(
            *testScene, voxelWorld, hitBlock.x, hitBlock.y, hitBlock.z, before, 0));
        const bool dug = before != 0u && hit &&
                         testScene->getVoxelBlock(voxelWorld, hitBlock.x, hitBlock.y, hitBlock.z) == 0u &&
                         voxelStack.undoCount() == 1;
        // 持久化（tech-debt #19）：挖掉后 capture 必须正好记下这一格 Air。
        bool captureOk = false;
        {
            const scene::SceneDescription captured = testScene->captureDescription();
            for (const scene::VoxelWorldDescription& world : captured.voxelWorlds)
            {
                if (world.seed != 9090ull)
                    continue;
                for (const scene::VoxelChunkDescription& chunk : world.chunks)
                    for (const scene::VoxelBlockEdit& edit : chunk.edits)
                        if (edit.x == voxel::Chunk::toLocal(hitBlock.x) &&
                            edit.y == voxel::Chunk::toLocal(hitBlock.y) &&
                            edit.z == voxel::Chunk::toLocal(hitBlock.z) &&
                            edit.block == 0u)
                            captureOk = true;
            }
        }

        voxelStack.undo();
        const bool restored = testScene->getVoxelBlock(
            voxelWorld, hitBlock.x, hitBlock.y, hitBlock.z) == before;

        // JSON 往返：手搓一个只含体素差分的描述，序列化再读回必须逐字段一致。
        scene::SceneDescription voxelOnly;
        scene::VoxelWorldDescription worldDesc;
        worldDesc.seed = 9090ull;
        worldDesc.radiusChunks = 1;
        worldDesc.baseY = 2;
        scene::VoxelChunkDescription chunkDesc;
        chunkDesc.chunkX = -1;
        chunkDesc.chunkZ = 3;
        chunkDesc.edits.push_back(scene::VoxelBlockEdit{
            4, 5, 6, static_cast<uint8_t>(voxel::BlockId::Brick) });
        worldDesc.chunks.push_back(chunkDesc);
        voxelOnly.voxelWorlds.push_back(worldDesc);

        scene::SceneDescription roundTripped;
        std::string roundTripError;
        bool jsonOk = SceneSerializer::roundTrip(voxelOnly, roundTripped, roundTripError);
        if (jsonOk)
        {
            jsonOk = roundTripped.voxelWorlds.size() == 1 &&
                     roundTripped.voxelWorlds[0].seed == 9090ull &&
                     roundTripped.voxelWorlds[0].radiusChunks == 1 &&
                     roundTripped.voxelWorlds[0].baseY == 2 &&
                     roundTripped.voxelWorlds[0].chunks.size() == 1 &&
                     roundTripped.voxelWorlds[0].chunks[0].chunkX == -1 &&
                     roundTripped.voxelWorlds[0].chunks[0].chunkZ == 3 &&
                     roundTripped.voxelWorlds[0].chunks[0].edits.size() == 1 &&
                     roundTripped.voxelWorlds[0].chunks[0].edits[0].x == 4 &&
                     roundTripped.voxelWorlds[0].chunks[0].edits[0].y == 5 &&
                     roundTripped.voxelWorlds[0].chunks[0].edits[0].z == 6 &&
                     roundTripped.voxelWorlds[0].chunks[0].edits[0].block ==
                         static_cast<uint8_t>(voxel::BlockId::Brick);
        }

        // 清理：体素世界是独立管理器 + chunk 实体，测试结束要全删掉。
        std::vector<entt::entity> chunks;
        for (auto [entity, chunk] : registry.view<VoxelChunkComponent>().each())
        {
            if (chunk.world == voxelWorld)
                chunks.push_back(entity);
        }
        for (entt::entity entity : chunks)
            testScene->destroyEntity(entity);
        testScene->destroyEntity(voxelWorld);

        if (!hit || !dug || !restored || !resolvesToWorld || !captureOk || !jsonOk)
        {
            std::cerr << "[Editor] selfTest FAIL: voxel dig/undo (hit=" << hit
                      << " dug=" << dug << " restored=" << restored
                      << " resolve=" << resolvesToWorld << " capture=" << captureOk
                      << " json=" << jsonOk << " jsonError=" << roundTripError << ")" << std::endl;
            return false;
        }
        std::cout << "[Editor] voxel edit selfTest PASS | dug and restored ("
                  << hitBlock.x << "," << hitBlock.y << "," << hitBlock.z
                  << "), capture + json round trip" << std::endl;
    }

    // ---- 2i) tech-debt #19：异步创建地形（实体先出现，几何后台合成） ----
    {
        Scene& scene = *state.services.scene;
        const size_t meshesBefore = registry.view<MeshComponent>().size();

        // 用 UI 默认档（257x257）量，这才是"点一下创建地形"的真实成本。
        terrain::TerrainConfig asyncConfig;
        asyncConfig.seed = 4321u;
        asyncConfig.localColorRange = false;

        // 同配置先量一次同步创建，作为"异步到底省了多少"的对照。
        const auto syncStart = std::chrono::steady_clock::now();
        const entt::entity syncCreated = scene.createTerrainEntity(
            asyncConfig, "SyncTerrainProbe", glm::vec3(0.0f));
        const double syncMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - syncStart).count();
        if (syncCreated != entt::null)
            scene.destroyEntity(syncCreated);

        const auto submitStart = std::chrono::steady_clock::now();
        const entt::entity created = scene.createTerrainEntityAsync(
            asyncConfig, "AsyncTerrain", glm::vec3(0.0f));
        const double submitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - submitStart).count();

        const auto* whileGenerating = registry.try_get<TerrainComponent>(created);
        const bool entityExistsImmediately = created != entt::null && registry.valid(created);
        const bool stillGenerating = whileGenerating != nullptr &&
                                     whileGenerating->generating && !whileGenerating->generated;
        const bool noMeshYet = registry.try_get<MeshComponent>(created) == nullptr;

        scene.waitForTerrainGeneration();

        const auto* built = registry.try_get<TerrainComponent>(created);
        const auto* builtMesh = registry.try_get<MeshComponent>(created);
        const bool geometryReady = built != nullptr && built->generated && !built->generating &&
                                   built->vertexCount > 0u && builtMesh != nullptr &&
                                   builtMesh->model != nullptr;

        if (!entityExistsImmediately || !stillGenerating || !noMeshYet || !geometryReady)
        {
            std::cerr << "[Scene] async terrain create selfTest FAIL | exists="
                      << entityExistsImmediately << " generating=" << stillGenerating
                      << " noMesh=" << noMeshYet << " ready=" << geometryReady << std::endl;
            return false;
        }
        std::cout << "[Scene] async terrain create selfTest PASS | sync=" << syncMs
                  << " ms, async submit=" << submitMs
                  << " ms, vertices=" << built->vertexCount << std::endl;

        scene.destroyEntity(created);
        if (registry.view<MeshComponent>().size() != meshesBefore)
        {
            std::cerr << "[Scene] async terrain create selfTest FAIL: cleanup mismatch" << std::endl;
            return false;
        }
    }

    // ---- B-01: independent streaming chunks + manager serialization ----
    {
        Scene& scene = *state.services.scene;
        // 这个块只关心"自己创建的 chunk 是否清干净"，所以基线要在这里取：
        // 更早的用例（新建场景 / 体素世界）可能已经改变过 MeshComponent 总数。
        const size_t streamingMeshesBefore = registry.view<MeshComponent>().size();
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

        // F5-01：元数据组件必须走完整的 capture -> JSON -> build 链路。
        // 这里用默认 record 而不是包含 FeatureComponents.h，验证的是公开 API。
        std::optional<scene::ComponentRecord> spinBefore =
            scene.defaultComponentRecord("effects.spin");
        if (!spinBefore || !spinBefore->find("degreesPerSecond"))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: effects.spin not registered\n";
            return false;
        }
        spinBefore->find("degreesPerSecond")->value = scene::PropertyValue::fromFloat(90.0f);
        if (!scene.applyComponentRecord(serializedCharacter, *spinBefore) ||
            !scene.hasComponent(serializedCharacter, "effects.spin"))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: could not attach metadata component\n";
            return false;
        }

        // 场景级组件属于场景文档：改一个值，保存/加载后必须原样恢复。
        auto environment = scene.readComponent(scene.getSceneSettingsEntity(),
                                               "scene.environment");
        if (!environment || !environment->find("exposure"))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: scene environment missing\n";
            return false;
        }
        environment->find("exposure")->value = scene::PropertyValue::fromFloat(1.35f);
        if (!scene.applyComponentRecord(scene.getSceneSettingsEntity(), *environment))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: scene environment apply\n";
            return false;
        }

        const scene::SceneDescription captured = scene.captureDescription();
        int worldManagers = 0;
        int serializedChunks = 0;
        int serializedCharacters = 0;
        int capturedSpinComponents = 0;
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
            for (const scene::ComponentRecord& record : entity.components)
            {
                if (record.type != "effects.spin")
                    continue;
                const scene::NamedProperty* property = record.find("degreesPerSecond");
                if (!property || std::abs(property->value.asFloat() - 90.0f) > 1e-5f)
                {
                    std::cerr << "[Editor] selfTestWithScene FAIL: metadata component value lost\n";
                    return false;
                }
                ++capturedSpinComponents;
            }
        }
        if (capturedSpinComponents != 1)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: metadata component not captured\n";
            return false;
        }
        if (!captured.sceneComponentsAuthored || captured.sceneComponents.size() != 10)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: scene components not captured | count="
                      << captured.sceneComponents.size() << "\n";
            return false;
        }
        scene::SceneDescription serialized;
        std::string serializationError;
        if (!SceneSerializer::roundTrip(captured, serialized, serializationError) ||
            worldManagers != 1 || serializedChunks != 0 || serializedCharacters != 1 ||
            !serialized.sceneComponentsAuthored || serialized.sceneComponents.size() != 10)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming serialization | "
                      << serializationError << " | sceneComponents="
                      << serialized.sceneComponents.size() << "\n";
            return false;
        }
        {
            bool exposureRoundTrip = false;
            for (const scene::ComponentRecord& record : serialized.sceneComponents)
            {
                if (record.type != "scene.environment")
                    continue;
                const scene::NamedProperty* exposure = record.find("exposure");
                exposureRoundTrip = exposure &&
                    std::abs(exposure->value.asFloat() - 1.35f) <= 1e-4f;
            }
            if (!exposureRoundTrip)
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: scene component value lost\n";
                return false;
            }
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
        bool loadedSpin = false;
        for (const scene::EntityDescription& entity : fileDescription.entities)
        {
            for (const scene::ComponentRecord& record : entity.components)
            {
                if (record.type != "effects.spin")
                    continue;
                const scene::NamedProperty* property = record.find("degreesPerSecond");
                if (property && std::abs(property->value.asFloat() - 90.0f) <= 1e-5f)
                    loadedSpin = true;
            }
        }
        if (!loadedSpin)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: metadata component missing after load\n";
            return false;
        }
        std::error_code removeError;
        std::filesystem::remove(scenePath, removeError);

        // 父目录不存在时必须自动创建：编辑器默认文档路径是
        // Resources/Scenes/main.scene.json，而全新检出里该目录并不存在。
        {
            const std::filesystem::path nestedParent = ".tmp/editor-save-nested";
            std::error_code nestedErrorCode;
            std::filesystem::remove_all(nestedParent, nestedErrorCode);
            const std::filesystem::path nestedPath = nestedParent / "scene.json";

            std::string nestedError;
            if (!SceneSerializer::saveToFile(serialized, nestedPath.string(), nestedError))
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: save into missing parent -> "
                          << nestedError << "\n";
                return false;
            }
            if (!std::filesystem::exists(nestedPath))
            {
                std::cerr << "[Editor] selfTestWithScene FAIL: missing parent save produced no file\n";
                return false;
            }
            std::filesystem::remove_all(nestedParent, nestedErrorCode);
            std::cout << "[Editor] scene save missing parent selfTest PASS | parent created"
                      << std::endl;
        }

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
        if (std::abs(scene.getSettings().exposure - 1.35f) > 1e-4f ||
            scene.captureSceneComponents().size() != 10)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: scene components missing after rebuild |"
                      << " exposure=" << scene.getSettings().exposure
                      << " count=" << scene.captureSceneComponents().size() << "\n";
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
        std::optional<scene::ComponentRecord> restoredSpin =
            scene.readComponent(restoredCharacter, "effects.spin");
        if (!restoredSpin || !restoredSpin->find("degreesPerSecond") ||
            std::abs(restoredSpin->find("degreesPerSecond")->value.asFloat() - 90.0f) > 1e-5f)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: metadata component missing after rebuild\n";
            return false;
        }

        // SetComponentRecordCommand 覆盖改值/撤销/重做；最后停在保存时的 90 度。
        std::optional<scene::ComponentRecord> spinAfter = restoredSpin;
        spinAfter->find("degreesPerSecond")->value = scene::PropertyValue::fromFloat(180.0f);
        state.commands.execute(std::make_unique<SetComponentRecordCommand>(
            scene, restoredCharacter, "effects.spin", restoredSpin, spinAfter, "Modify Component"));
        const auto spinEquals = [&scene, restoredCharacter](float expected) {
            const auto record = scene.readComponent(restoredCharacter, "effects.spin");
            if (!record)
                return false;
            const scene::NamedProperty* property = record->find("degreesPerSecond");
            return property && std::abs(property->value.asFloat() - expected) <= 1e-5f;
        };
        if (!spinEquals(180.0f))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: component command apply\n";
            return false;
        }
        state.commands.undo();
        if (!spinEquals(90.0f))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: component command undo\n";
            return false;
        }
        state.commands.redo();
        if (!spinEquals(180.0f))
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: component command redo\n";
            return false;
        }
        state.commands.undo();
        state.commands.clear();
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
        // 这里只断言"流送 chunk 清干净"，不能比较全局 MeshComponent 数量：
        // 上面的 buildFromDescription() 会 clearEntities() 重建整个场景，体素世界等
        // 未进序列化的内容本来就会被清掉（见 docs/70-status/tech-debt.md 的体素持久化条目）。
        if (scene.hasStreamingTerrain() ||
            registry.view<TerrainChunkComponent>().size() != 0u)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: streaming manager cleanup leaked chunks"
                      << " (hasStreaming=" << scene.hasStreamingTerrain()
                      << " terrainChunks=" << registry.view<TerrainChunkComponent>().size()
                      << ")" << std::endl;
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

    // 自检会故意触发"播放中禁止写文档"这类守卫：留下的 Toast 与错误摘要不该跟着进截图，
    // 但 Console 里的日志行保留 —— 它是这条守卫真的被触发过的证据。
    // ---- 0f) E9：运行模式接管角色 ----
    {
        Scene* scene = state.services.scene;
        entt::registry* registry = state.services.registry;
        if (scene == nullptr || registry == nullptr)
        {
            std::cerr << "[Editor] runtime character selfTest FAIL: no scene bound" << std::endl;
            return false;
        }

        const glm::vec3 savedPosition = scene->getCameraTransform().getTranslation();
        setEditorMode(EditorState::EditorMode::Runtime);
        if (state.runtimeCharacter == entt::null || !registry->valid(state.runtimeCharacter))
        {
            std::cerr << "[Editor] runtime character selfTest FAIL: character not spawned" << std::endl;
            return false;
        }

        // runtimeYaw/runtimePitch 必须存相机 euler 的原始弧度值；这里多转一次
        // 弧度→度会把 yaw=π 缩成 0.055，W/S/A/D 全部反向（用户报的方向错）。
        const glm::vec3 enteredEuler = scene->getCameraTransform().getEuler();
        const bool yawPreserved = std::abs(state.runtimeYaw - enteredEuler.y) < 1e-5f;
        const bool pitchPreserved =
            std::abs(state.runtimePitch - glm::clamp(enteredEuler.x,
                -glm::radians(85.0f), glm::radians(85.0f))) < 1e-5f;

        auto* animation = registry->try_get<CharacterAnimationComponent>(state.runtimeCharacter);
        const bool skinned = animation != nullptr && animation->model != nullptr &&
            animation->model->isSkinned();
        const size_t animations = (animation != nullptr && animation->model != nullptr)
            ? animation->model->getAnimations().size() : 0u;
        // Quaternius 的 clip 名带 "CharacterArmature|" 前缀；加载时必须剥掉，
        // 否则 findAnimation("Idle"/"Walk"/...) 全部落空，角色停在绑定姿态。
        const int idleClip = (animation && animation->model)
            ? animation->model->findAnimation("Idle") : -1;
        const bool clipsFound = animation && animation->model &&
            idleClip >= 0 &&
            animation->model->findAnimation("Walk") >= 0 &&
            animation->model->findAnimation("Run") >= 0 &&
            animation->model->findAnimation("Jump") >= 0 &&
            animation->model->findAnimation("Jump_Idle") >= 0 &&
            animation->model->findAnimation("Jump_Land") >= 0;
        const bool idleActive = animation && clipsFound &&
            animation->currentClip == idleClip && animation->currentClipName == "Idle";

        TransformComponent* feet = registry->try_get<TransformComponent>(state.runtimeCharacter);
        if (feet == nullptr)
        {
            std::cerr << "[Editor] runtime character selfTest FAIL: character has no transform" << std::endl;
            return false;
        }
        feet->setTranslation(glm::vec3(4.0f, 3.0f, 4.0f));
        applyRuntimeCameraPose();
        const glm::vec3 cameraPosition = scene->getCameraTransform().getTranslation();
        const glm::vec3 toCamera = cameraPosition - feet->getTranslation();
        const glm::vec3 flatForward{ std::sin(state.runtimeYaw), 0.0f, std::cos(state.runtimeYaw) };
        const bool follows = cameraPosition.y > feet->getTranslation().y + 0.5f &&
            glm::dot(toCamera, flatForward) < -0.1f;

        setEditorMode(EditorState::EditorMode::Edit);
        const bool restored = state.runtimeCharacter == entt::null &&
            glm::length(scene->getCameraTransform().getTranslation() - savedPosition) < 1e-4f;
        if (!follows || !restored || !yawPreserved || !pitchPreserved || !skinned ||
            animations != 18 || !clipsFound || !idleActive)
        {
            std::cerr << "[Editor] runtime character selfTest FAIL: follows=" << follows
                      << " restored=" << restored
                      << " yawPreserved=" << yawPreserved
                      << " pitchPreserved=" << pitchPreserved
                      << " skinned=" << skinned
                      << " animations=" << animations
                      << " clipsFound=" << clipsFound
                      << " idleActive=" << idleActive << std::endl;
            return false;
        }
        std::cout << "[Editor] runtime character selfTest PASS | camera follows feet, exit restores pose"
                  << " | skinned=" << (skinned ? 1 : 0) << " animations=" << animations
                  << " clips=Idle/Walk/Run/Jump/Jump_Idle/Jump_Land active=Idle"
                  << std::endl;
    }

    // ---- 2h) F5-02：预制体保存 / 载入 / 实例化 / 撤销重做 ----
    {
        Scene* scene = state.services.scene;
        entt::registry* registry = state.services.registry;
        if (scene == nullptr || registry == nullptr)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: no scene bound" << std::endl;
            return false;
        }

        std::error_code code;
        std::filesystem::create_directories(".tmp", code);
        const std::string prefabPath = ".tmp/prefab-selftest.prefab.json";
        std::filesystem::remove(prefabPath, code);

        const size_t baselineMeshes = registry->view<MeshComponent>().size();
        const entt::entity parent = scene->createMeshEntity(cubePath, "PrefabSelfTestParent");
        const entt::entity child = scene->createMeshEntity(cubePath, "PrefabSelfTestChild");
        if (parent == entt::null || child == entt::null)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: could not create source entities" << std::endl;
            return false;
        }

        scene->setParent(child, parent);
        registry->get<TransformComponent>(child).setTranslation(glm::vec3(1.0f, 2.0f, 0.0f));
        scene->refreshTransformsAndVisibility();

        scene::ComponentRecord spin = scene->defaultComponentRecord("effects.spin").value_or(scene::ComponentRecord{});
        if (auto* degrees = spin.find("degreesPerSecond"))
            degrees->value = scene::PropertyValue::fromFloat(90.0f);
        if (!scene->applyComponentRecord(child, spin))
        {
            std::cerr << "[Editor] prefab selfTest FAIL: could not attach effects.spin" << std::endl;
            return false;
        }

        // 原生组件：走专用字段（isCharacter/characterRadius），不能重复进 components。
        if (!scene->addComponent(child, "native.characterController"))
        {
            std::cerr << "[Editor] prefab selfTest FAIL: could not attach native character" << std::endl;
            return false;
        }
        auto characterRecord = scene->readComponent(child, "native.characterController");
        if (!characterRecord)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: native character record missing" << std::endl;
            return false;
        }
        if (scene::NamedProperty* radius = characterRecord->find("radius"))
            radius->value = scene::PropertyValue::fromFloat(0.42f);
        if (!scene->applyComponentRecord(child, *characterRecord))
        {
            std::cerr << "[Editor] prefab selfTest FAIL: native character apply failed" << std::endl;
            return false;
        }

        const scene::SceneDescription captured = scene->captureDescription(parent);
        if (captured.entities.size() != 2 || captured.entities[0].parentIndex != -1 ||
            captured.entities[1].parentIndex != 0 || !captured.entities[1].isCharacter ||
            captured.entities[1].components.size() != 1)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: capture ordering/parent index/native dedupe"
                      << std::endl;
            return false;
        }

        std::string error;
        if (!prefab::save(*scene, parent, prefabPath, error))
        {
            std::cerr << "[Editor] prefab selfTest FAIL: save: " << error << std::endl;
            return false;
        }

        scene::SceneDescription loaded;
        if (!prefab::load(prefabPath, loaded, error))
        {
            std::cerr << "[Editor] prefab selfTest FAIL: load: " << error << std::endl;
            return false;
        }
        if (loaded.entities.size() != 2 ||
            std::abs(loaded.entities[1].translation.x - 1.0f) > 1e-4f ||
            !loaded.entities[1].isCharacter ||
            std::abs(loaded.entities[1].characterRadius - 0.42f) > 1e-4f ||
            loaded.entities[1].components.size() != 1 ||
            loaded.entities[1].components[0].type != "effects.spin")
        {
            std::cerr << "[Editor] prefab selfTest FAIL: loaded data changed" << std::endl;
            return false;
        }

        auto command = std::make_unique<InstantiatePrefabCommand>(*scene, loaded, entt::null);
        InstantiatePrefabCommand* raw = command.get();
        if (!raw->succeeded() || raw->createdEntities().size() != 2)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: instantiate: " << raw->errorMessage() << std::endl;
            return false;
        }

        const entt::entity instanceRoot = raw->currentRoot();
        const entt::entity instanceChild = raw->createdEntities()[1];
        state.commands.execute(std::move(command));

        const auto* hierarchy = registry->try_get<HierarchyComponent>(instanceChild);
        const auto instanceSpin = scene->readComponent(instanceChild, "effects.spin");
        const float* spinDegrees = nullptr;
        if (instanceSpin)
        {
            if (const auto* property = instanceSpin->find("degreesPerSecond"))
                spinDegrees = &property->value.floatValue;
        }
        const auto instanceCharacter =
            scene->readComponent(instanceChild, "native.characterController");
        const float* instanceRadius = nullptr;
        if (instanceCharacter)
        {
            if (const auto* property = instanceCharacter->find("radius"))
                instanceRadius = &property->value.floatValue;
        }
        if (hierarchy == nullptr || hierarchy->parent != instanceRoot ||
            !scene->hasComponent(instanceChild, "effects.spin") ||
            spinDegrees == nullptr || std::abs(*spinDegrees - 90.0f) > 1e-4f ||
            instanceRadius == nullptr || std::abs(*instanceRadius - 0.42f) > 1e-4f ||
            registry->view<MeshComponent>().size() != baselineMeshes + 4)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: instance hierarchy/component/mesh mismatch" << std::endl;
            return false;
        }

        state.commands.undo();
        if (registry->valid(instanceRoot) || registry->view<MeshComponent>().size() != baselineMeshes + 2)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: undo left instance entities" << std::endl;
            return false;
        }

        state.commands.redo();
        // 重做是重新实例化（新实体 ID），不能拿撤销前的旧 ID 判定。
        const entt::entity redoRoot = raw->currentRoot();
        if (!registry->valid(redoRoot) ||
            registry->view<MeshComponent>().size() != baselineMeshes + 4)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: redo did not recreate instance" << std::endl;
            return false;
        }

        state.commands.undo();
        scene->destroyEntity(child);
        scene->destroyEntity(parent);
        scene->refreshTransformsAndVisibility();
        state.commands.clear();
        state.selection.clear();
        std::filesystem::remove(prefabPath, code);

        if (registry->view<MeshComponent>().size() != baselineMeshes)
        {
            std::cerr << "[Editor] prefab selfTest FAIL: cleanup did not restore mesh count" << std::endl;
            return false;
        }

        std::cout << "[Editor] prefab selfTest PASS | save/load/instantiate/undo/redo" << std::endl;
    }

    // ---- 2h) 帮助 → 示例：预设场景能替换世界并自动取景 ----
    {
        Scene* scene = state.services.scene;
        entt::registry* registry = state.services.registry;
        if (scene == nullptr || registry == nullptr)
        {
            std::cerr << "[Editor] example presets selfTest FAIL: no scene bound" << std::endl;
            return false;
        }

        // 先验证菜单路径：脏文档必须只登记意图，取消后连示例 id 一起清掉。
        state.documentDirty = true;
        requestExample("voxel-glass");
        const bool gated = state.pendingDocumentAction == EditorState::PendingDocumentAction::Example &&
            state.pendingExampleId == "voxel-glass";
        cancelPendingDocumentAction();
        const bool cancelled = state.pendingDocumentAction == EditorState::PendingDocumentAction::None &&
            state.pendingExampleId.empty();
        state.documentDirty = false;
        if (!gated || !cancelled)
        {
            std::cerr << "[Editor] example presets selfTest FAIL: dirty document gating"
                      << " gated=" << gated << " cancelled=" << cancelled << std::endl;
            return false;
        }

        if (!loadExample("voxel-glass"))
        {
            std::cerr << "[Editor] example presets selfTest FAIL: loadExample returned false" << std::endl;
            return false;
        }
        scene->waitForVoxelBuilds();

        uint64_t glassFaces = 0;
        for (auto [entity, chunk] : registry->view<VoxelChunkComponent>().each())
        {
            (void)entity;
            glassFaces += chunk.glassFaceCount;
        }
        const glm::vec3 camera = scene->getCameraTransform().getTranslation();
        const bool cameraPlaced = glm::length(camera - glm::vec3(16.0f, 22.0f, 54.0f)) < 0.01f;
        const bool worldPlaced = registry->view<VoxelWorldComponent>().size() == 1;
        if (glassFaces == 0 || !cameraPlaced || !worldPlaced || state.documentDirty)
        {
            std::cerr << "[Editor] example presets selfTest FAIL: glassFaces=" << glassFaces
                      << " cameraPlaced=" << cameraPlaced
                      << " worldPlaced=" << worldPlaced
                      << " dirty=" << state.documentDirty << std::endl;
            return false;
        }
        std::cout << "[Editor] example presets selfTest PASS | glass faces=" << glassFaces << std::endl;

        // 再加载一次 voxel-world：用户报的“打开示例全黑”就是这条路径。
        // 9 个 chunk 必须同时有 MeshComponent 和 WorldBoundsComponent，
        // 后者缺失时 updateVisibility() 会把它们全部剔除（draw calls=0）。
        newScene(false);
        if (!loadExample("voxel-world"))
        {
            std::cerr << "[Editor] example presets selfTest FAIL: voxel-world loadExample returned false"
                      << std::endl;
            return false;
        }
        scene->waitForVoxelBuilds();
        entt::entity worldRoot = entt::null;
        for (auto [entity, worldComp] : registry->view<VoxelWorldComponent>().each())
        {
            (void)worldComp;
            worldRoot = entity;
        }
        size_t worldMeshes = 0;
        size_t worldBounds = 0;
        size_t worldParented = 0;
        for (auto [entity, chunk] : registry->view<VoxelChunkComponent>().each())
        {
            (void)chunk;
            if (registry->all_of<MeshComponent>(entity))
                ++worldMeshes;
            if (registry->all_of<WorldBoundsComponent>(entity))
                ++worldBounds;
            const auto* hierarchy = registry->try_get<HierarchyComponent>(entity);
            if (hierarchy != nullptr && hierarchy->parent == worldRoot)
                ++worldParented;
        }
        const glm::vec3 worldCamera = scene->getCameraTransform().getTranslation();
        const bool worldCameraPlaced =
            glm::length(worldCamera - glm::vec3(16.0f, 46.0f, 118.0f)) < 0.01f;
        if (worldMeshes != 9 || worldBounds != 9 || worldParented != 9 || !worldCameraPlaced)
        {
            std::cerr << "[Editor] example presets selfTest FAIL: voxel-world meshes=" << worldMeshes
                      << " bounds=" << worldBounds << " parented=" << worldParented
                      << " cameraPlaced=" << worldCameraPlaced << std::endl;
            return false;
        }
        std::cout << "[Editor] example presets selfTest PASS | voxel-world meshes=" << worldMeshes
                  << " bounds=" << worldBounds << " parented=" << worldParented << std::endl;

        // 清掉示例，避免后面的截图/测试带着一个 3x3 体素世界。
        newScene(false);
    }

    state.dismissToast();
    state.lastErrorSummary.clear();

    return true;
}

} // namespace editor
