#pragma once

// ===========================================================================
// 编辑器主层
//
// 职责边界（重要）：
//   * 拥有"编辑器状态"（选中、命令栈、日志、文档路径、播放状态）
//   * 拥有面板列表
//   * 画菜单栏、处理编辑器快捷键、安排面板布局
//
// 它**不**拥有：设备、交换链、渲染图、场景、材质系统 —— 那些都是引擎的。
// 引擎在 Application 里构造好之后，把 EngineServices 递进来。
//
// 与引擎的调用顺序（每帧）：
//     application.update()  ->  editor.beginFrame(dt)   // 处理快捷键 / 播放状态
//     application.render()  ->  editor.drawUI()         // 构建 ImGui 绘制列表
//                           ->  editor.endFrame()       // 收尾（脏标记等）
// ===========================================================================

#include "Editor/EditorContext.h"
#include "Editor/EditorState.h"
#include "Editor/EditorPanel.h"
#include "Editor/EditorPreferences.h"
#include "Scene/SimulationClock.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace editor
{

class EditorLayer
{
public:
    EditorLayer();
    ~EditorLayer();

    EditorLayer(const EditorLayer&) = delete;
    EditorLayer& operator=(const EditorLayer&) = delete;

    // 引擎构造完成后调用一次。services 里的指针必须活到 shutdown()。
    void initialize(const EngineServices& services, const std::string& defaultScenePath);
    // --runtime-mode：启动后直接进入运行模式（CLI 截图/演练用）。
    void startInRuntimeMode();

    // 每帧：处理编辑器输入（快捷键、播放状态），必须在 ImGui::NewFrame 之后调用。
    void beginFrame(float dt, bool viewportHovered);

    // 构建所有面板与菜单栏的 ImGui 绘制列表。必须在 PresentPass 录制前调用。
    void drawUI();

    void endFrame();

    bool isActive() const { return active; }
    void setActive(bool value) { active = value; }

    // 引擎侧查询：编辑器是否吃掉了键盘输入（避免相机同时响应 WASD）
    bool wantsKeyboardInput() const { return state.wantsKeyboard; }

    // 引擎拦截窗口关闭按钮时调这里：有未保存修改就先弹确认框。
    void requestExitConfirmation() { requestDocumentAction(EditorState::PendingDocumentAction::Exit); }

    // 打开指定场景文件（供命令行 --scene 使用）
    bool openScene(const std::string& path);

    // 引擎在构建 UI 之前告知本帧的 frame-in-flight 槽位（视口纹理按槽位选）。
    void setFrameSlot(uint32_t slot) { state.frameSlot = slot; }

    EditorState& getState() { return state; }
    const EditorState& getState() const { return state; }

    // D-8 自检：验证"命令栈 + 场景描述往返 + 序列化"这三条最容易出错的路径。
    // 不建 Vulkan 设备也能跑的部分放在 SceneSerializer::roundTrip 里，
    // 这里额外验证命令栈的撤销/重做语义与 entity 快照重建。
    static bool selfTest();

    // 需要真实场景与设备才能跑的部分：CreateEntityCommand 的创建/撤销/重做。
    // 与静态 selfTest() 分开，是因为它必须在一个已初始化的编辑器实例上跑。
    bool selfTestWithScene();

private:
    void createDefaultPanels();
    void drawMenuBar();
    // E5：菜单栏下面一条工具栏（播放/手柄模式/吸附/聚焦/撤销），底部一条状态栏。
    // 都是固定矩形、不记忆位置 —— 它们是"框架"而不是可拖动面板。
    void drawToolbar();
    void drawStatusBar();
    // B3：做了有回应。Toast 是短暂的回执/报错，状态栏是常驻摘要，
    // 两者都能一键跳到 Console 的那一行。
    void drawToast();
    void drawOpenFailurePrompt();
    bool openConsoleAtLastError();
    void drawPanels();

    // 首帧强制套用默认布局。之后用户拖动过的位置由 imgui.ini 决定。
    // 菜单 View -> Reset Layout 会把它重新置位。
    bool layoutNeedsReset = true;
    // 仅 View -> Reset Layout 需要强制重建 dock 节点；首次启动只有在 ini 里没有
    // dock 布局时才铺默认布局，避免每次启动覆盖用户自己拖好的停靠。
    bool forceDockLayoutReset = false;
    // 首帧在 DockSpace 创建根节点之前检测一次：ini 里是否已有可复用的 dock 树。
    bool dockLayoutChecked = false;
    bool hasSavedDockLayout = false;
    // Render 与 Inspector 共用右侧节点；首次建布局/点击工具栏「渲染」时把 Render 页签切到前台。
    bool focusRenderPanel = false;

    void handleShortcuts();

    // ---- E4：文档安全（未保存确认 + 标题） ----
    void requestDocumentAction(EditorState::PendingDocumentAction action);
    void performDocumentAction(EditorState::PendingDocumentAction action);
    void drawUnsavedPrompt();
    void updateWindowTitle();
    void cancelPendingDocumentAction();
    void restoreInitialScene(const std::string& fallbackPath);

    // ---- 场景文件重命名 ----
    void drawRenameScenePrompt();
    void requestRenameScene();
    bool renameCurrentScene(const std::string& requestedName, std::string& errorMessage);
    bool renameSceneRequested = false;
    char renameSceneBuffer[256] = {};
    std::string renameSceneError;


    // ---- E4-c：快捷键速查（Help 菜单 / ? 键） ----
    void drawShortcutSheet();
    bool showShortcutSheet = false;
    // B5 / UX-18：诊断入口 —— 复制现场 + 关于窗。
    void copyDiagnostics();
    void drawAboutWindow();
    bool showAboutWindow = false;

    // ---- E4-b：偏好持久化（面板可见性 / Gizmo 模式 / 最近场景） ----
    void applyPreferences();
    void syncPreferencesFromState();
    void savePreferences();
    void requestOpenScene(const std::string& path);
    EditorPreferences preferences;
    std::string preferencesPath;
    bool preferencesDirty = false;
    float preferencesSaveDelay = 0.0f;
    bool unsavedPromptRequested = false;
    std::string lastWindowTitle;
    void updatePlayState(float dt);

    // Shared top-level Create menu entry. Uses CreateTerrainCommand and selects the result.
    entt::entity createTerrainFromUi();
    // UX-08：登记一次地形创建（真正的 build 在下一帧的 beginFrame 里做）。
    void requestTerrainCreate();

    // B-07: create a kinematic character at the camera's ground position.
    entt::entity createCharacterFromUi();

    bool newScene(bool announce = true);
    // 帮助 → 示例：一键加载预设场景。pendingExampleId 让未保存确认之后仍记得选的是哪个示例。
    void requestExample(const char* id);
    bool loadExample(const std::string& id);
    bool saveScene();
    bool saveSceneAs();
    bool openSceneDialog();
    // F5-02：预制体（保存选中子树 / 实例化到当前场景）。
    bool savePrefabFromSelection();
    bool instantiatePrefabDialog();
    bool loadSceneFromPath(const std::string& path, std::string& errorMessage, bool recordRecent = true);

    void setEditorMode(EditorState::EditorMode mode);
    // E9：运行模式接管角色（第一人称）。进入时生成一个不可见的角色实体并把
    // 相机交还给它，退出时销毁角色并把相机恢复到进入前的位姿。
    void enterRuntimeMode();
    void exitRuntimeMode();
    void updateRuntimeMode(float dt);
    void applyRuntimeCameraPose();
    void enterPlay();
    void pausePlay();
    void resumePlay();
    void stopPlay();

    EditorState state;
    std::vector<std::unique_ptr<EditorPanel>> panels;
    std::unordered_map<std::string, EditorPanel*> panelsByName;

    bool active = false;
    bool initialized = false;
    float lastFrameDelta = 0.0f;
    // M1-C1b：播放/运行模式的模拟时钟（固定 1/60 tick）。
    scene::SimulationClock simulationClock;

    template <typename T>
    T* registerPanel()
    {
        auto panel = std::make_unique<T>();
        T* raw = panel.get();
        panels.push_back(std::move(panel));
        panelsByName[raw->name()] = raw;
        return raw;
    }

    EditorPanel* findPanel(const char* panelName) const;
};

} // namespace editor
