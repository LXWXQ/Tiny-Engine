#pragma once

// ===========================================================================
// 编辑器的可变状态（单一实例，由 EditorLayer 持有）
//
// 与 EngineServices 的分工：
//   EngineServices —— 引擎能做什么（不可变的能力集合，启动时填一次）
//   EditorState    —— 编辑器现在是什么状态（每帧都在变）
//
// 面板只拿到 EditorState&（里面含 EngineServices&），不直接持有引擎对象。
// 这样面板之间共享状态的路径是显式的，不需要全局变量。
// ===========================================================================

#include "Editor/CommandStack.h"
#include "Editor/EditorContext.h"
#include "Editor/Gizmo.h"
#include "Scene/SceneDescription.h"
#include "Terrain/TerrainBrush.h"

#include <deque>
#include <string>
#include <vector>

namespace editor
{

struct EditorState
{
    EngineServices services;

    // ---- 选中 ----
    Selection selection;

    // ---- 三轴操作手柄 ----
    // 放在 EditorState 而不是 ViewportPanel 里，是因为"当前是平移还是旋转"
    // 属于编辑器全局状态：菜单栏、快捷键、视口都要读写同一个值。
    Gizmo gizmo;

    // ---- 撤销 / 重做 ----
    CommandStack commands;

    // ---- 日志 ----
    std::deque<LogEntry> log;
    static constexpr size_t kMaxLogEntries = 2000;
    bool scrollLogToBottom = false;

    void pushLog(LogEntry::Level level, const std::string& text)
    {
        log.push_back(LogEntry{ level, text });
        while (log.size() > kMaxLogEntries)
            log.pop_front();
        scrollLogToBottom = true;
    }

    // ---- 场景文档 ----
    std::string documentPath;      // 空 = 还没另存过
    bool documentDirty = false;
    void markDirty() { documentDirty = true; }

    // 播放期间文档只读：运行期状态不回写磁盘，也不允许 New/Open 把播放快照盖掉。
    // 这是状态机的约束而不是 UI 限制，所以放在 state 上，菜单、快捷键、面板都能查。
    bool documentWritable() const { return playState == PlayState::Editing; }

    // ---- 播放状态机 ----
    PlayState playState = PlayState::Editing;

    // 进入 Playing 时的场景快照。Stop 时用它恢复。
    scene::SceneDescription playSnapshot;
    bool hasPlaySnapshot = false;

    // ---- 面板可见性（View 菜单控制）----
    bool showHierarchy = true;
    bool showInspector = true;
    bool showConsole = true;
    bool showStatistics = true;

    // ---- 视口 ----
    // 3D 画面目前直接铺满整个交换链，面板浮在它上面。
    // 这个矩形记录"没有被面板遮挡的中央区域"，后续做离屏视口时以它为准。
    float viewportX = 0.0f;
    float viewportY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    bool viewportHovered = false;

    // 供面板判断"这一帧是否应该处理快捷键"
    bool wantsKeyboard = false;

    // 本帧有面板改过 TransformComponent，需要立刻重算世界变换与包围盒。
    //
    // 【为什么需要这个标志】UI 是在 render() 里构建的，而 Scene::update()
    // （负责 updateWorldTransforms + rebuildSpatialIndex）在 update() 里已经跑完了。
    // 所以拖拽 gizmo 改完 transform 后，WorldTransformComponent 与
    // WorldBoundsComponent 还停在**上一帧**的值 —— 表现就是
    // "拖动时白色包围盒和物体分开、手柄也跟着漂"。
    // 置位后由 EditorLayer::drawUI() 在本帧结束前统一刷新一次。
    bool transformRefreshPending = false;

    // ---- B-07 地形笔刷 ----
    // 视口中 Ctrl+左键应用；中心点由射线命中地形决定。
    bool terrainBrushEnabled = false;
    terrain::TerrainBrushSettings terrainBrush;

    // 本帧对应的 frame-in-flight 槽位。视口纹理按槽位各一张，
    // ViewportPanel 需要用它挑出正确的那张。
    uint32_t frameSlot = 0;

    // 视口面板这一帧实际画出的像素尺寸。与期望值不同时（首帧 / 窗口缩放）
    // 通过 EngineServices::requestViewportSize 请求引擎重建。
    uint32_t requestedViewportWidth = 0;
    uint32_t requestedViewportHeight = 0;
};

} // namespace editor
