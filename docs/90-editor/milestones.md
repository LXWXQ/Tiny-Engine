---
id: editor/milestones
title: 编辑器里程碑（E1 起）
type: status
scope: 空
size: ~110 行
read-when: 想知道编辑器做过哪些轮次与每轮的验证基线
updated: 2026-11-03
---

# 编辑器里程碑

> 引擎侧（D / F 系列）的里程碑与实测基线见 [milestones](../70-status/milestones.md)。
> 本页记录编辑器从 E1 开始的逐轮改动，编号与提交信息一一对应。

## E1：选择与批量操作（多选 / 复制 / 聚焦）
**问题**：编辑器只有单选；`Selection` 内部虽是 vector，但只暴露替换语义；删除只作用于 `primary()`，没有复制/聚焦，层级面板没有搜索。
**实现**：`Selection::add/remove/toggle/size`；`CompositeCommand`（多条子命令 = 一步撤销，revert 反序）+ `DuplicateEntityCommand`（白名单组件拷贝、子树 BFS、拒绝地形/流送/相机）；快捷键 `Delete` 批量删除、`Ctrl+D` 复制、`Ctrl+A` 全选、`F` 聚焦；Hierarchy 的 Ctrl/Shift 多选 + 搜索过滤 + 多选右键菜单；Viewport Ctrl 点击 toggle；Inspector 多选摘要与批量按钮。
**可观测**：`[Editor] selfTest PASS | commands + delete-undo + batch delete/duplicate + serialization round trip`。**回归**：新增 `e1-batch-commands`；全套 **41 项**；编辑器自检、`--selftest 60` 冒烟、Validate 全 PASS。

## E2：多对象拖拽（一组选中共用一条命令）

- 问题：Gizmo 只作用 `selection.primary()`，多选拖拽时其余物体不动。
- 实现：`SetTransformsCommand` + `TransformPose`；拖拽起点快照整组（局部 pose、世界矩阵、父世界逆），每帧用 `deltaWorld = primaryWorldNew * inverse(primaryStartWorld)` 施加到其余实体，再从矩阵列拆 T/R/S；地形 chunk / 流送 / 相机不参与。
- 可观测：`[Editor] selfTest PASS | commands + delete-undo + batch delete/duplicate + multi transform + serialization round trip`；合并窗口保证一次拖拽 = 一条撤销。
- 验证：`TinyEditor.exe --selftest 5 --empty-scene` 0/0；回归 42 项（新增 `e2-multi-transform`）。

## E3：材质参数与渲染标记进撤销栈

- 问题：Inspector 的 Base Color / Metallic / Roughness / Alpha Mode 与 Render Tag 复选框直接写组件，不进命令栈；而且这些 factor 当时根本没进 GPU，滑杆是安慰剂。
- 实现：`MaterialFactorRecord`（32 字节）+ 全局 set `binding 10` 的 SSBO，`updateMaterialFactors()` 只写 32 字节 + flush；`mrt.frag` 按 glTF 规范把 factor 乘到采样结果上；`SetMaterialCommand` / `SetRenderTagCommand` 接管面板写操作。
- 可观测：`[Editor] selfTest PASS | ... + material/render tag + ...`；Sponza 墙面从 186 降到 161（资源自带 `baseColorFactor = 0.588`，现在才真正生效）。
- 验证：编辑器自检 0/0、`--selftest 60` 154 FPS 0/0；回归 43 项（新增 `e3-material-undo`）。

## E3b：播放期间文档只读

- 问题：播放中 Ctrl+S 会把运行期位姿写进文档；New / Open 会替换 `playSnapshot`，Stop 后恢复到错误的场景。
- 实现：`EditorState::documentWritable()` 作为单一判据；`saveScene` / `newScene` / `loadSceneFromPath` 在非 Editing 状态直接拒绝并打 Warning；File 菜单相应项置灰。
- 可观测：`[Editor] play document lock selfTest PASS | save/new/open blocked while playing`。 决定：播放状态切换不进撤销栈（运行期状态不是文档内容）。

## E4-a：文档安全（未保存确认 + 窗口标题）

- 问题：有未保存修改时 `Ctrl+N` / `Ctrl+O` / File→Exit 会静默丢改动；标题栏从不显示当前文档与脏标记；点窗口关闭按钮也是直接退。
- 实现：`EditorState::PendingDocumentAction{None,New,Open,Exit}` + `requestDocumentAction()` 单一路口（干净或播放中直接执行，脏则只记意图），`drawUnsavedPrompt()` 一个模态框（保存并继续 / 放弃修改 / 取消；保存失败不吞意图）；`EngineServices::setWindowTitle` / `requestExit` 两个回调；标题 = `文件名 + " *" + " - Tiny Editor"`，只在变化时推送；主循环拦截窗口关闭按钮转成确认。
- 可观测：`[Editor] unsaved changes prompt selfTest PASS | dirty blocks new/open/exit until resolved`。
- 验证：编辑器自检 0/0、`--selftest 60` 202 FPS 0/0；回归 45 项（新增 `e4-unsaved-prompt`）。

## E4-b：编辑器偏好持久化（config/editor.json + Open Recent）

- 问题：关掉编辑器再打开，面板可见性和 Gizmo 模式全部回到默认；反复打开的场景每次都要重新找文件。
- 实现：`EditorPreferences`（`panelVisibility` 按面板名、`gizmoMode`、`recentScenes` 上限 8）+
  `config/editor.json`（不进版本库）；`EditorLayer::endFrame()` 单向同步 state -> preferences，
  变化后延迟 0.75s 落盘；File → Open Recent 走 `requestOpenScene()`，仍然经过未保存确认。
- 可观测：`[Editor] preferences selfTest PASS | round trip + recent dedupe/cap + broken json`、
  `[Editor] preferences file selfTest PASS | save + load + recent round trip`。
- 验证：编辑器自检 0/0、引擎冒烟 0/0；回归 46 项（新增 `e4-preferences`）。

## E4-c：快捷键速查（Help 菜单 + ? 键 + 中文字形）

- 问题：快捷键只散落在菜单文字与文档里，`F` 聚焦、`Ctrl+D` 复制、`Ctrl+A` 全选几乎发现不了；
  另外 ImGui 只有 ASCII 字形，界面上所有中文（日志、模态框按钮、提示）全是 `?`。
- 实现：`kShortcutEntries` 一张表驱动弹窗与自检；`?`（Shift+Slash）开合、`Esc` 关闭、
  窗口首次出现时居中；`ImGuiSystem` 按候选列表加载系统 CJK 字体（msyh/msyhl/simhei/simsun），
  找不到就退回内置字体。
- 可观测：`[Editor] shortcut sheet selfTest PASS | entries=16`、`[ImGui] font: C:/Windows/Fonts/msyh.ttc`。
- 验证：编辑器自检 0/0（含截图确认中文与表格排版）；回归 47 项（新增 `e4-shortcut-sheet`）。

## E4-d：撤销历史面板

- 问题：`CommandStack` 只有深度（`undoCount`/`redoCount`），用户看不到「栈里有什么」，
  也没法一次退回多步，只能连按 Ctrl+Z。
- 实现：`CommandStack::undoEntries()` / `redoEntries()` 只读暴露两条栈，
  `jumpTo(size_t)` 一步一步 undo/redo；新 `HistoryPanel`（右栏下 35%）列深度 + 命令名，
  当前状态单独一行，点击任意行跳转；播放中禁用并提示。
- 可观测：`[Editor] history jump selfTest PASS | depth 0 -> 3 -> clamp`。
- 验证：编辑器自检 0/0、截图确认（空栈 / 两条命令合并成一条两种状态）；
  回归 48 项（新增 `e4-history-jump`）。

## E5-a：工具栏 + 状态栏 + Gizmo 吸附

- 问题：播放、手柄模式、聚焦全靠菜单和快捷键；帧数据只在 Statistics 面板里；Gizmo 完全没有吸附，
  摆一个物体到「整数格」只能靠肉眼。
- 实现：`EditorLayer::drawToolbar()` / `drawStatusBar()` 两个固定矩形窗口（34 px / 26 px），
  面板布局减去它们的高度；菜单栏右侧只留文档名，避免三处重复；
  `Gizmo::snapValue()` + `snapEnabled/snapTranslate/snapRotateDegrees/snapScale`，Shift 临时吸附。
- 可观测：`[Gizmo] snap selfTest PASS | steps 0.5 / 15 deg / 0.1`。
- 验证：编辑器自检 0/0；截图确认工具栏/状态栏/面板不重叠；回归 50 项（新增 `e5-toolbar-snap`）。

## E5-b：渲染设置面板（EngineSettings 实时可调）

- 问题：`EngineSettings` 里 exposure / bloom / SSAO / motion blur / 方向光早已可调，但编辑器完全没暴露；
  改一次要动 `config/engine.json` 再重启，调光等于盲调。
- 实现：`RenderSettingsPanel`（浮动工具窗，工具栏 `Render` 按钮 + View 菜单），直接写
  `scene->getSettings()`，下一帧生效；Revert 回会话基线、Save 写回 engine.json；
  `clampKernelSize()` 把 SSAO 核夹到 4..64 偶数。
- 可观测：`[Editor] render settings selfTest PASS | kernel clamped to 4..64 even`。
- 验证：编辑器自检 0/0；截图确认面板可读、按钮在顶部不用滚动；回归 51 项（新增 `e5-render-settings`）。

## B1（部分）：`--help` 与未知参数退出码

- 问题：[UX-02](../90-editor/ux-audit.md) —— `--help` 什么都不打印，参数写错也照常启动，用户以为参数生效了。
- 实现：`main.cpp` 里参数表改成数据（`kUsageLines`，43 条、按运行模式/窗口/截图/渲染/地形/自检/剖析/帮助分组）；
  `--help` / `-h` 打印后退出码 0，且在创建窗口与设备之前；未知参数收集到 `unknownArgs`，
  在创建窗口前打印 `unknown argument: xxx` 到 stderr 并按约定返回 2。
- 坑：`--selftest` 的帧数是唯一不用 `++i` 消费的取值参数，直接加 `else` 会把 `--selftest 60` 的 `60` 判成未知；
  用一次性标志放行。
- 可观测：`TinyEngine.exe --help`（退出码 0）、`TinyEngine.exe --not-a-flag`（退出码 2，stderr 一行）。
- 验证：回归 53 项（新增 `b1-help` / `b1-unknown-arg`，`Validate.ps1` 新增 `ExpectedExitCode` / `AllowStderr` / `MustContainStderr` / `SkipValidationMarker` 四个可选字段）。

## B2：空状态与措辞（UX-01 / UX-09 / UX-16）

- **UX-01**：视口中央说明卡（居中于画面、带两个按钮）、Hierarchy 空树提示；新增纯判据 `editor::isSceneEmpty`（天空盒/相机不算内容，有网格或地形 chunk 才算）。
- **UX-09**：Inspector 多选提示从英文播报改成中文两行，并指向批量按钮。
- **UX-16**：Assets 面板区分「模型目录不存在」与「目录里没有模型」，都给出路径与下一步。
- 回归：`b2-empty-state`（`TinyEditor.exe --selftest 5 --empty-scene`，标记 `[Editor] empty state selfTest PASS`）。

## B3：通知与失败出路（UX-03 / UX-10 / UX-12 / UX-17）

- 统一出口 `EditorState::notify / notifySuccess / tickNotices`：Toast（2/5 秒）+ 状态栏常驻摘要 + 未读错误计数。
- 工具栏红色「错误 N」徽标，点击打开 Console 并滚到最近错误行（白字高亮）；状态栏摘要同样可点。
- `openScene()` 失败不再只写日志：弹「无法打开场景」模态框，提供重试 / 选择其它文件 / 关闭。
- 已读语义：本帧新产生的错误不计已读，避免徽标闪烁。
- 回归：`b3-notices`（`[Editor] notices selfTest PASS | toast decay + error summary + unread badge`）。

## B4：菜单补齐 + 快捷键单一数据源 + 面板可拖动

- Edit 菜单补 `Duplicate Selected` / `Delete Selected` / `Focus Selection` / `Select All`，
  与快捷键调同一组函数（UX-06）。
- `kShortcutEntries` 从"给文档看的表"变成唯一数据源：`key` / `ctrl` / `shift` / `ShortcutAction`，
  `handleShortcuts()` 遍历分发（修饰键精确匹配，一帧只做一个动作），速查表键位列由
  `shortcutKeys()` 现算（UX-13）。
- 自检扫空项、键位标签、三元组冲突，打印 `[Editor] shortcut sheet selfTest PASS | entries=19 conflicts=0`。
- `drawPanels()` 里 Viewport 不再用 `ImGuiCond_Always`，所有面板统一
  `layoutNeedsReset ? Always : FirstUseEver`：用户可拖动/缩放，`View -> Reset Layout` 复位（UX-14）。
- 清掉 `config/imgui.ini` 里一条 `[Window][Render] ... Collapsed=1` 的旧记录（它会让渲染设置面板
  每次启动都是一条细线）。
- 回归：`b4-shortcuts`（`[Editor] shortcut sheet selfTest PASS` + `conflicts=0`）。

## B5：语言统一 + Console 时间/计数/过滤/复制 + 状态栏三段

- UX-07 语言分层：命令与动作、日志与提示用中文；区域名（面板标题）与行业参数（FPS、draw counts、
  Base Color、Bloom、SSAO）保留英文，因为面板标题同时是 `EditorPanel::name()`、`imgui.ini` 的键、
  `config/editor.json` 的键。规则写进 `ux-rules` 的 U7，附词汇表（gizmo / unsaved / selection）。
- UX-11 Console：`LogEntry` 增加 `time`（`EditorState::pushLog` 用 `currentLogClock()` 填 `HH:MM:SS`），
  每行时间列 + 正文两段；顶部 Info/Warn/Error 三个复选框带计数；「仅错误」「全部」两个视图；
  「复制」导出当前可见行（与显示共用 `levelVisible()` 判据）；`自动滚动` / `清空` 中文化。
- UX-15 状态栏三段：左 = 选中 / 手柄 / 吸附 / 最近错误；中 = `绘制 N · 三角形 270.5k`
  （`formatCount()` 做 k/M 缩写，tooltip 给精确值）；右 = `FPS · ms · 撤销 N`。帧率从工具栏右侧
  移到状态栏，工具栏只留「保存状态 · 播放状态」。
- 关键坑：状态栏统计必须读 `Scene::getDisplayTelemetry()`。`beginFrameTelemetry()` 每帧开头把上一帧
  的完整统计搬进 `displayTelemetry` 再清 `currentTelemetry`；编辑器先画 UI 再录制渲染，读
  `getTelemetry()` 永远是 0（首版就是这样，截图里显示 `绘制 0 · 三角形 0`）。
- UX-18 帮助菜单：打开控制台并跳到最近错误 / 复制诊断信息（`copyDiagnostics()`）/ 关于 Tiny Editor。
- 自检：`[Editor] status bar selfTest PASS | counts k/M + log clock HH:MM:SS`
  （`formatCount(42)="42"`、`(270459)="270.5k"`、`(2704590)="2.7M"`，`currentLogClock()` 长度 8）。
- 回归：`b5-language-console`（`[Editor] status bar selfTest PASS`）。

## B6：收掉最后三条 UX 项（Console 搜索 / UX-08 / UX-19）

- **Console 关键字搜索**：`matchesSearch()`（空串全匹配、ASCII 大小写折叠），与级别开关、复制共用判据；
  `Ctrl+F` 进搜索框（快捷键表 19 条），无匹配时正文给出提示而不是空白；自检
  `[Editor] console filter selfTest PASS | level switches + case-insensitive search`。
- **UX-08 地形创建两阶段**：`requestTerrainCreate()` 只入队，`advanceTerrainCreateRequest()` 在下一帧才执行 build，
  于是先画出一帧「地形生成中…」（工具栏黄字 + Toast + 视口按钮旁的「生成中…」）；
  pending 期间重复点击不叠地形；自检 `[Editor] terrain create request selfTest PASS | busy frame first, build on the next`。
  遗留：build 仍是同步的（tech-debt 第 19 条）。
- **UX-19 字体与布局 DPI**：`computeUiScale()`（取较大轴、<1 夹 1、>3 夹 3）+ `Style.ScaleAllSizes` +
  字体 `16 × scale`；编辑器用 `uiScaleFactor()`（由字号倒推）放大工具栏/状态栏高度与侧栏宽度；
  自检 `[Window] ui scale selfTest PASS | 1x1 -> 1, 1.5x1.5 -> 1.5, 0.9x0.9 -> 1, 4x4 -> 3`。
- 回归：`b6-console-search` / `b2-empty-state` 两条新用例；编辑器自检 0/0、引擎 60 帧 0/0、DocCheck 通过。
