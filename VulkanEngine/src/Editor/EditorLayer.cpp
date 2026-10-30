#include "Editor/EditorLayer.h"
#include "Editor/SceneSerializer.h"
#include "Editor/Commands.h"

#include "Editor/panels/HierarchyPanel.h"
#include "Editor/panels/InspectorPanel.h"
#include "Editor/panels/ConsolePanel.h"
#include "Editor/panels/ViewportPanel.h"
#include "Editor/panels/AssetBrowserPanel.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"

#include <imgui.h>
#include "AssetPaths.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdio>

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

    createDefaultPanels();

    for (auto& panel : panels)
        panel->onAttach(state);

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

    handleShortcuts();
    updatePlayState(dt);
}

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
        newScene();
    }
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
    {
        openSceneDialog();
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
        const entt::entity selected = state.selection.primary();
        if (selected != entt::null && state.services.registry->valid(selected))
        {
            const entt::entity cameraEntity = state.services.scene->getCameraEntity();
            if (selected != cameraEntity)      // 相机实体不允许删
            {
                state.selection.clear();
                state.commands.execute(std::make_unique<DeleteEntityCommand>(
                    *state.services.registry, selected));
                state.markDirty();
            }
        }
    }
}

void EditorLayer::updatePlayState(float /*dt*/)
{
    // 播放状态目前没有"游戏逻辑"可推进（场景是静态的），
    // 但状态机本身是框架的一部分：以后加脚本/物理时，推进点就在这里。
    if (state.playState == PlayState::Playing)
    {
        // 预留：调用 scene->tickSimulation(dt) 之类
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

    drawPanels();
    layoutNeedsReset = false;

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
        if (ImGui::MenuItem("New Scene", "Ctrl+N")) newScene();
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) openSceneDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveScene();
        if (ImGui::MenuItem("Save As...")) saveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Reload Config") && state.services.reloadConfig)
            state.services.reloadConfig();
        if (ImGui::MenuItem("Save Config") && state.services.saveConfig)
            state.services.saveConfig();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) io.WantCaptureKeyboard = false;  // 由窗口关闭处理
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
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
            layoutNeedsReset = true;
        ImGui::EndMenu();
    }

    // 右侧状态条：脏标记 + 播放状态 + 撤销栈深度
    const float rightWidth = 420.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth);

    if (state.documentDirty)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "* unsaved");
    else
        ImGui::TextDisabled("saved");

    ImGui::SameLine();
    const char* playLabel = (state.playState == PlayState::Editing) ? "Editing"
                          : (state.playState == PlayState::Playing) ? "Playing" : "Paused";
    ImGui::TextDisabled("| %s", playLabel);

    ImGui::SameLine();
    ImGui::TextDisabled("| undo %d", static_cast<int>(state.commands.undoCount()));

    ImGui::SameLine();
    ImGui::TextDisabled("| %s", (state.gizmo.mode == GizmoMode::Translate) ? "move"
                                 : (state.gizmo.mode == GizmoMode::Rotate) ? "rotate" : "scale");
}

void EditorLayer::drawPanels()
{
    // 关键约束：ImGui::SetNextWindowPos/Size 是**单槽位**的 ——
    // 连续调用多次只有最后一次生效，而且会被"下一个 Begin 的窗口"消费。
    // 所以绝不能先把三个面板的位置都设置一遍再统一绘制
    // （那样 Console 的矩形会套到 Hierarchy 上，另外两个面板退回默认位置）。
    // 正确做法：在**每个面板 Begin 之前紧邻**设置它自己的矩形。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float menuBarHeight = ImGui::GetFrameHeight();

    const float leftWidth = 280.0f;
    const float rightWidth = 340.0f;
    const float bottomHeight = 200.0f;

    const float top = viewport->WorkPos.y + menuBarHeight;
    const float availableWidth = viewport->WorkSize.x;
    const float availableHeight = viewport->WorkSize.y - menuBarHeight;

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
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + availableWidth - rightWidth, top), cond);
            ImGui::SetNextWindowSize(ImVec2(rightWidth, availableHeight), cond);
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
}

// ---------------------------------------------------------------------------
// 场景文档操作
// ---------------------------------------------------------------------------
bool EditorLayer::newScene()
{
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
        "Sentinel Scene (*.scene.json)\0*.scene.json\0All Files (*.*)\0*.*\0",
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
        "Sentinel Scene (*.scene.json)\0*.scene.json\0All Files (*.*)\0*.*\0",
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

bool EditorLayer::loadSceneFromPath(const std::string& path, std::string& errorMessage)
{
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

    // ---- 1) 命令栈：精确撤销 ----
    {
        const entt::entity entity = registry.create();
        const glm::vec3 original{ 1.0f, 2.0f, 3.0f };
        registry.emplace<TransformComponent>(entity, TransformComponent{ original, {}, { 1.0f, 1.0f, 1.0f } });

        CommandStack stack;
        const glm::vec3 target{ 4.0f, 5.0f, 6.0f };
        stack.execute(std::make_unique<SetTransformCommand>(
            registry, entity, original, target, glm::vec3(0.0f), glm::vec3(0.0f),
            glm::vec3(1.0f), glm::vec3(1.0f)));

        const glm::vec3 afterApply = registry.get<TransformComponent>(entity).translation;
        if (afterApply != target)
        {
            std::cerr << "[Editor] selfTest FAIL: transform not applied\n";
            return false;
        }

        stack.undo();
        const glm::vec3 afterUndo = registry.get<TransformComponent>(entity).translation;
        // 必须**精确**相等，不能有 epsilon —— 命令存的是旧值拷贝，不是增量。
        if (afterUndo != original)
        {
            std::cerr << "[Editor] selfTest FAIL: undo not exact ("
                      << afterUndo.x << "," << afterUndo.y << "," << afterUndo.z << ")\n";
            return false;
        }

        stack.redo();
        if (registry.get<TransformComponent>(entity).translation != target)
        {
            std::cerr << "[Editor] selfTest FAIL: redo failed\n";
            return false;
        }

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

    // ---- 3) 场景描述 <-> JSON 往返 ----
    {
        scene::SceneDescription input;

        scene::EntityDescription a;
        a.name = "Root";
        a.translation = { 1.5f, -2.25f, 0.125f };
        a.rotation = { 0.1f, 0.2f, 0.3f };
        a.scale = { 2.0f, 2.0f, 2.0f };
        a.parentIndex = -1;
        a.meshPath = SENTINEL_RESOURCE_DIR "Models/sphere.obj";
        a.isSkybox = false;
        input.entities.push_back(a);

        scene::EntityDescription b;
        b.name = "Child With Space";
        b.parentIndex = 0;                 // 引用数组下标
        b.meshPath = SENTINEL_RESOURCE_DIR "Models/DamagedHelmet/DamagedHelmet.obj";
        b.material.present = true;
        b.material.name = "glTF/Default";
        b.material.baseColorFactor = { 0.2f, 0.4f, 0.6f, 1.0f };
        b.material.metallicFactor = 0.75f;
        b.material.roughnessFactor = 0.35f;
        input.entities.push_back(b);

        scene::SceneDescription output;
        std::string error;
        if (!SceneSerializer::roundTrip(input, output, error))
        {
            std::cerr << "[Editor] selfTest FAIL: roundTrip -> " << error << "\n";
            return false;
        }

        if (output.entities.size() != 2)
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

    entt::registry& registry = *state.services.registry;
    const std::string cubePath = std::string(SENTINEL_RESOURCE_DIR) + "Models/debug_cube.obj";

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

    std::cout << "[Editor] scene selfTest PASS | create/undo/redo via command stack"
              << " (meshes=" << meshesBefore << ")\n";
    // ---- 4) 精确拾取：射线必须命中"更近"的球，而不是把相机包在里面的 Sponza ----
    // 这是 AABB 拾取失效的典型场景：Sponza 的 AABB 把相机包住，
    // 射线与它的入射距离恒为 0，于是点任何东西都选中 Sponza。
    if (state.services.scene && state.services.registry)
    {
        entt::registry& registry = *state.services.registry;
        Scene& scene = *state.services.scene;

        // 注意：不能用 sphere.obj —— 那是半径 1000 的天空盒球。
        const std::string spherePath = std::string(SENTINEL_RESOURCE_DIR) + "Models/debug_cube.obj";
        const entt::entity sphere = scene.createMeshEntity(spherePath, "PickTestCube");
        if (sphere == entt::null)
        {
            std::cerr << "[Editor] selfTestWithScene FAIL: could not create test mesh\n";
            return false;
        }

        // 把球放到相机前方 6 个单位
        const TransformComponent& cameraTransform = scene.getCameraTransform();
        const glm::vec3 forward = glm::vec3{
            std::sin(cameraTransform.rotation.y) * std::cos(cameraTransform.rotation.x),
            -std::sin(cameraTransform.rotation.x),
            std::cos(cameraTransform.rotation.y) * std::cos(cameraTransform.rotation.x)
        };
        const glm::vec3 spherePos = cameraTransform.translation + forward * 6.0f;
        registry.get<TransformComponent>(sphere).translation = spherePos;
        registry.get<TransformComponent>(sphere).scale = glm::vec3(1.0f);

        // 刷新世界变换与包围盒，否则测试读到的是上一帧的旧数据
        scene.refreshTransformsAndVisibility();

        // 从相机中心朝球心打一条射线
        const glm::vec3 rayOrigin = cameraTransform.translation;
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
            transform->translation += delta;
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
        }

        registry.destroy(sphere);
        scene.refreshTransformsAndVisibility();
    }

    return true;
}

} // namespace editor
