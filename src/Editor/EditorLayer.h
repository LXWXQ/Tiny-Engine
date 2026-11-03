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
    void drawPanels();

    // 首帧强制套用默认布局。之后用户拖动过的位置由 imgui.ini 决定。
    // 菜单 View -> Reset Layout 会把它重新置位。
    bool layoutNeedsReset = true;

    void handleShortcuts();

    // ---- E4：文档安全（未保存确认 + 标题） ----
    void requestDocumentAction(EditorState::PendingDocumentAction action);
    void performDocumentAction(EditorState::PendingDocumentAction action);
    void drawUnsavedPrompt();
    void updateWindowTitle();
    void cancelPendingDocumentAction();
    bool unsavedPromptRequested = false;
    std::string lastWindowTitle;
    void updatePlayState(float dt);

    // Shared top-level Create menu entry. Uses CreateTerrainCommand and selects the result.
    entt::entity createTerrainFromUi();

    // B-07: create a kinematic character at the camera's ground position.
    entt::entity createCharacterFromUi();

    bool newScene();
    bool saveScene();
    bool saveSceneAs();
    bool openSceneDialog();
    bool loadSceneFromPath(const std::string& path, std::string& errorMessage);

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
