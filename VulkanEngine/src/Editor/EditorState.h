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
#include "Scene/SceneDescription.h"

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

    // 本帧对应的 frame-in-flight 槽位。视口纹理按槽位各一张，
    // ViewportPanel 需要用它挑出正确的那张。
    uint32_t frameSlot = 0;

    // 视口面板这一帧实际画出的像素尺寸。与期望值不同时（首帧 / 窗口缩放）
    // 通过 EngineServices::requestViewportSize 请求引擎重建。
    uint32_t requestedViewportWidth = 0;
    uint32_t requestedViewportHeight = 0;
};

} // namespace editor
