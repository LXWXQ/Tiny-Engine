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

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

namespace editor
{

// B5：Console 的时间列。墙上时间比"第几帧"好核对 —— 用户看到错误时会去看时钟。
inline std::string currentLogClock()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_MSC_VER)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return std::string(buffer);
}

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
        log.push_back(LogEntry{ level, currentLogClock(), text });
        while (log.size() > kMaxLogEntries)
            log.pop_front();
        scrollLogToBottom = true;
    }

    // ---- B3：通知（Toast / 最近错误 / 未读错误）----
    //
    // 规则：Console 可以被 View 菜单关掉，所以「错误只写进 Console」等于没写。
    // 所有 Error / Warning 都同时进 Toast（短暂）与状态栏摘要（常驻到下一次成功），
    // 并在 Console 关闭时累计未读数，让工具栏上有个能点的标记。
    //
    // Info 也会进 Toast —— 但只给 2 秒，作用是「做了有回应」（UX-10）。
    struct EditorNotice
    {
        std::string text;
        LogEntry::Level level = LogEntry::Level::Info;
        float remaining = 0.0f;   // 秒；<= 0 表示不显示
    };

    EditorNotice toast;
    std::string lastErrorSummary;
    float lastErrorAge = 0.0f;      // 秒；越老颜色越淡
    size_t lastErrorLogIndex = 0;   // 指向 log 里的下标，Console 用它高亮
    uint32_t unreadErrors = 0;
    bool focusConsoleRequest = false;
    // B6：Ctrl+F 把焦点放进 Console 的搜索框（只请求一次）。
    bool consoleSearchFocusRequest = false;
    // UX-08：地形创建是同步的（一次 build 会占满一帧），用两阶段请求：
    // 点击当帧只登记，下一帧把「生成中」画出来并 present，再下一帧才真正执行。
    // 这样用户至少看得到"它在干活"，而不是界面莫名卡住。
    bool terrainCreatePending = false;

    // tech-debt #19：异步地形构建的跟踪目标。实体当帧出现，几何后台合成；
    // 每帧 poll 一次，完成时把结果播报出来（成功给顶点数/耗时，失败给原因）。
    entt::entity pendingTerrainBuild = entt::null;
    int terrainCreateWaitFrames = 0;
    // 本帧刚产生的错误不能算「已读」：Console 画出来不代表用户看到了新加的那一行。
    // 每帧开头 tickNotices() 清掉，notify(Error) 置位。
    bool errorRaisedThisFrame = false;

    void notify(LogEntry::Level level, const std::string& text)
    {
        pushLog(level, text);
        toast.text = text;
        toast.level = level;
        toast.remaining = (level == LogEntry::Level::Info) ? 2.0f : 5.0f;
        if (level == LogEntry::Level::Error)
        {
            lastErrorSummary = text;
            lastErrorAge = 0.0f;
            lastErrorLogIndex = log.size() - 1;
            ++unreadErrors;
            errorRaisedThisFrame = true;
        }
    }

    // 成功动作顺手清掉「最近错误」——否则修好之后红色摘要还挂着，等于永远报错。
    void notifySuccess(const std::string& text)
    {
        pushLog(LogEntry::Level::Info, text);
        toast.text = text;
        toast.level = LogEntry::Level::Info;
        toast.remaining = 2.0f;
        lastErrorSummary.clear();
        lastErrorAge = 0.0f;
        lastErrorLogIndex = 0;
    }

    void tickNotices(float deltaSeconds)
    {
        errorRaisedThisFrame = false;
        const float dt = deltaSeconds > 0.0f ? deltaSeconds : 0.0f;
        toast.remaining = toast.remaining > dt ? toast.remaining - dt : 0.0f;
        lastErrorAge += dt;
    }

    void markErrorsRead() { unreadErrors = 0; }
    void dismissToast() { toast.remaining = 0.0f; }

    // UX-17：打开失败后要能「重试」或「换一个文件」，而不是让用户再走一遍 File > Open。
    std::string failedOpenPath;
    std::string failedOpenError;
    bool hasFailedOpen() const { return !failedOpenPath.empty(); }
    void clearFailedOpen()
    {
        failedOpenPath.clear();
        failedOpenError.clear();
    }

    // ---- 场景文档 ----
    std::string documentPath;      // 空 = 还没另存过
    bool documentDirty = false;
    void markDirty() { documentDirty = true; }

    // 播放期间文档只读：运行期状态不回写磁盘，也不允许 New/Open 把播放快照盖掉。
    // 这是状态机的约束而不是 UI 限制，所以放在 state 上，菜单、快捷键、面板都能查。
    bool documentWritable() const { return playState == PlayState::Editing; }

    // ---- 未保存修改的确认 ----
    // 非 None 表示有一个文档操作被"脏"挡住，等待用户在模态框里选。
    enum class PendingDocumentAction { None, New, Open, Exit, Example };
    PendingDocumentAction pendingDocumentAction = PendingDocumentAction::None;
    // 帮助 → 示例：被未保存确认挡住时要加载哪个预设（空 = 没有待执行示例）。
    std::string pendingExampleId;

    // ---- 编辑 / 运行模式 ----
    // 编辑模式：视口左键只做选中、手柄可拖、面板可改数据。
    // 运行模式：物体不可选中（进入时清空选中），鼠标左键交给当前工具
    // （体素挖 / 地形笔刷），右键放方块 —— 就是"像游戏一样操作世界"。
    // 与 PlayState 是两件事：PlayState 管场景快照回滚，这里只管输入归属。
    enum class EditorMode { Edit, Runtime };
    EditorMode mode = EditorMode::Edit;
    bool allowsSelection() const { return mode == EditorMode::Edit; }
    bool isRuntime() const { return mode == EditorMode::Runtime; }

    // ---- 播放状态机 ----
    PlayState playState = PlayState::Editing;

    // 进入 Playing 时的场景快照。Stop 时用它恢复。
    scene::SceneDescription playSnapshot;
    bool hasPlaySnapshot = false;

    // ---- 面板可见性（View 菜单控制）----
    // E4-b：最近打开的场景（由 EditorPreferences 维护，File -> Open Recent 直接用）。
    std::vector<std::string> recentScenes;

    // 排队中的"打开"动作要打开哪个文件；空 = 弹文件对话框。
    std::string pendingDocumentPath;

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
    // 视口中 Alt+左键应用；中心点由射线命中地形决定。
    bool terrainBrushEnabled = false;
    terrain::TerrainBrushSettings terrainBrush;

    // ---- F3 体素笔刷 ----
    // 视口中 Alt+左键挖掉命中方块、Alt+Shift+左键在命中面外放一块（Ctrl 留给多选）。
    // voxelWorld 为 null 时这些操作无目标。
    bool voxelBrushEnabled = false;
    int voxelBrushBlock = 1;   // voxel::BlockId 的整数值
    entt::entity voxelWorld = entt::null;

    // ---- E9 运行模式接管角色（第一人称） ----
    // 进入运行模式时创建一个不可见的角色实体，相机跟着它的脚底 + 眼高走；
    // 退出时销毁角色并还原进入前的相机位姿。
    entt::entity runtimeCharacter = entt::null;
    glm::vec3 runtimeSavedCameraPosition{ 0.0f };
    glm::vec3 runtimeSavedCameraEuler{ 0.0f };
    float runtimeYaw = 0.0f;     // 度，与相机 euler 一致
    float runtimePitch = 0.0f;   // 度

    // 本帧对应的 frame-in-flight 槽位。视口纹理按槽位各一张，
    // ViewportPanel 需要用它挑出正确的那张。
    uint32_t frameSlot = 0;

    // 视口面板这一帧实际画出的像素尺寸。与期望值不同时（首帧 / 窗口缩放）
    // 通过 EngineServices::requestViewportSize 请求引擎重建。
    uint32_t requestedViewportWidth = 0;
    uint32_t requestedViewportHeight = 0;
};

} // namespace editor
