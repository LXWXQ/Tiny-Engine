---
id: editor/ux-audit
title: 界面友好性现状审计
type: system
scope: src/Editor/, src/Core/ImGuiSystem.cpp, main.cpp
read-when: 计划一轮 UX 改造前，先确认哪些问题仍然存在、哪些已经修掉
size: ~190 行
updated: 2026-11-03
---

# 界面友好性现状审计

## 1. 这篇解决什么

把"对陌生人不友好"从感觉变成一张**有行号、有判据、有优先级**的清单。

用法有两种：

- 改之前：按 P0 顺序挑一批，不要按"哪个好改"挑。
- 改之后：把对应条目的状态从 `open` 改成 `fixed`，写清提交哈希与验证方式。
  条目只增不删，因为"修好了又退回去"需要能被看见。

判据定义在 [ux-rules](ux-rules.md)，本篇只记录事实与影响。

## 2. 审计口径

| 项 | 说明 |
|---|---|
| 审计对象 | `build/<Config>/TinyEditor.exe`，冷启动，默认布局 |
| 审计身份 | 陌生人：没读过 `README.md`、没用过本编辑器 |
| 通过标准 | 从启动到"放一个物体 + 存盘 + 重开"不超过 10 分钟，全程无需读代码 |
| 证据等级 | A = 可复现的命令或行号；B = 代码结构推断；C = 经验判断（本篇不用 C） |
| 行号基准 | 2026-11-03 工作区状态；行号会漂移，定位以函数名为准 |
| 显示环境 | 主环境 1920x1080 / 100% 缩放；U10 另需 1366x768 与 150% 缩放各复验一次 |

## 3. P0：阻断首个闭环

这六条任意一条不修，陌生人就走不完一遍完整流程。**开工顺序看 [ux-playbook](ux-playbook.md) §4 的批次表**，不要按这里的编号顺序排 —— 编号只是条目 ID，代表登记顺序，不代表陌生人撞上它的先后。

### UX-01 冷启动是一个没有任何文字的空画面

| 项 | 内容 |
|---|---|
| 现象 | 启动后中央视口全黑或只有天空，Hierarchy 空树，没有任何提示 |
| 证据 | `src/Core/Application.cpp:132` 注释"编辑器模式默认不加载 Sponza"；`loadDefaultScene` 在编辑器模式恒为 false |
| 影响 | 陌生人第一反应是"程序没跑起来"或"显卡有问题"，不会想到"场景是空的" |
| 判据 | U5 空状态可继续 |
| 修法指向 | 视口与 Hierarchy 各加一段三段式空状态，按钮复用 `createTerrainFromUi()` |
| 状态 | **fixed**（2026-11-03）：视口中央说明卡（`ViewportPanel.cpp`，判据 `isSceneEmpty`：天空盒/相机不算内容）+ Hierarchy 空树提示 + 两个按钮走命令栈；回归用例 `b2-empty-state` |

### UX-02 没有帮助入口，未知参数被静默忽略

| 项 | 内容 |
|---|---|
| 现象 | `TinyEditor.exe --help` 不会打印任何东西；43 个参数只写在 `main.cpp` 顶部注释里 |
| 证据 | `main.cpp` 全文无 `help` / `usage` / `unknown argument` 分支；参数解析是 1 个 `if` + 42 个 `else if (arg == ...)`，共 43 个参数 |
| 影响 | 参数写错时程序照常启动，用户以为参数生效了；排查方向完全错 |
| 判据 | U1 入口存在、U4 失败可行动 |
| 修法指向 | 加 `--help`（打印参数表并按组分节）；未知参数打印一行 `unknown argument: xxx` + `--help` 提示，退出码 2 |
| 状态 | **fixed**（2026-11-03）：`kUsageLines` 表驱动 `--help`（退出码 0、不建窗口），未知参数在创建窗口前退出码 2 并把提示写 stderr；回归用例 `b1-help` / `b1-unknown-arg` |

### UX-03 错误只写内存日志，界面不变化

| 项 | 内容 |
|---|---|
| 现象 | 保存失败、打开失败、创建失败，界面无任何变化 |
| 证据 | `src/Editor/EditorLayer.cpp` 中 7 处 `pushLog(Error)` + 3 处 `pushLog(Warning)`；`ConsolePanel` 可被 View 菜单关闭（`drawMenuBar()` 的 View 分支） |
| 影响 | Console 关掉后错误彻底消失；用户只会重复点同一个按钮 |
| 判据 | U3 结果反馈、U4 失败可行动 |
| 修法指向 | 状态栏常驻错误摘要 + 3 秒 Toast；Toast 可点击展开 Console 并定位到该行 |
| 状态 | **fixed**（2026-11-03）：`EditorState::notify` 统一出口 —— 错误同时进 Toast（5 秒，可点「查看」）、状态栏常驻红色摘要（30 秒后变淡）、工具栏「错误 N」徽标（Console 关着时也有入口）；三者都调 `openConsoleAtLastError()` 打开 Console 并滚到该行高亮。回归 `b3-notices` |

### UX-04 初始化失败时，双击启动的用户什么也看不到

| 项 | 内容 |
|---|---|
| 现象 | 缺少 Vulkan 1.3 或必需特性时，程序直接退出 |
| 证据 | `main.cpp` 末尾 `catch (const std::exception&)` 只写 `std::cerr << "致命错误: "`；Windows 双击启动没有控制台 |
| 影响 | 陌生人的结论是"这是个坏程序"，而不是"我的显卡缺 maintenance4" |
| 判据 | U4 失败可行动、U9 降级可见 |
| 修法指向 | 启动早期捕获初始化异常，弹一个原生 MessageBox（或最小 ImGui 窗口）说明缺哪个特性、去哪看 `--help`；同时写 `startup-error.log` 到可执行文件同目录 |
| 状态 | **部分 fixed**（2026-11-03）：`main.cpp` 的 catch 已把「阶段 + 消息 + 工作目录 + 提示」追加进可执行文件旁的 `startup-error.log`（`tiny::writeStartupErrorLog`，`src/Core/StartupLog.cpp`），并在 stderr 打印文件路径；设备丢失且驱动未启用 `VK_EXT_device_fault` 时，`Device::reportDeviceFault` 会打印原因与下一步并调用同一个写入函数。仍缺：原生 MessageBox（无控制台时仍要靠用户主动找文件）。回归：`b1-window-fit`（断言 `[Startup] error log selfTest PASS`） |

### UX-05 中文字体缺失时静默退化为问号

| 项 | 内容 |
|---|---|
| 现象 | 找不到 `msyh.ttc` / `simhei.ttf` 时，全部中文变 `?` |
| 证据 | `src/Core/ImGuiSystem.cpp:45-65`：按四个系统路径依次尝试，全部失败则保持内置字体；注释已写明后果 |
| 影响 | 界面"能看但读不懂"，且用户无法判断是编码问题还是字体问题。注意这条的阻断性依赖环境：字体齐全的机器上不影响闭环，缺字体的机器上才是 P0 |
| 判据 | U9 降级可见 |
| 修法指向 | 首帧检测 `io.Fonts->Fonts.Size <= 1`，状态栏红色提示"字体降级：中文不可用"，tooltip 给出缺失路径与解决办法 |

### UX-06 四个高频功能只在快捷键里，菜单里没有

| 项 | 内容 |
|---|---|
| 现象 | `Ctrl+D` 复制、`Ctrl+A` 全选、`F` 聚焦、`Delete` 删除均可用，但 Edit 菜单只有 Undo/Redo 与三种手柄模式 |
| 证据 | 快捷键表 `src/Editor/EditorLayer.cpp:260`；`drawMenuBar()` 的 Edit 分支在 498-524 行 |
| 影响 | 不按 `?` 打开速查表的人，永远不知道这些功能存在 |
| 判据 | U1 入口存在 |
| 修法指向 | Edit 菜单补齐四项，`enabled` 参数用真实的 `canDuplicate()` / `!selection.empty()` |
| 状态 | **fixed**（2026-11-03）：Edit 菜单补 `Duplicate Selected` / `Delete Selected` / `Focus Selection` / `Select All`，`enabled` 用 `!state.selection.entities.empty()`；回归用例 `b4-shortcuts` |

## 4. P1：能走通，但每步都在卡

| 编号 | 现象 | 证据 | 判据 | 状态 |
|---|---|---|---|---|
| UX-07 | ~~同一屏三种语言层级：按钮英文、tooltip 中文、状态栏中文；概念词不统一~~ | `EditorLayer.cpp` 工具栏 714 行起；状态栏 818 行起 | U7 | **fixed**（2026-11-03）：语言分层写进 ux-rules 的 U7（命令与提示中文、区域名与行业参数英文），词汇表收敛 gizmo / unsaved / selection；菜单、工具栏、状态栏同一套用词 |
| UX-08 | 地形生成同步阻塞，无进度、不禁用按钮 | `createTerrainFromUi()` 完成后才记录 `lastGenerationMs` | U2、U6 | open |
| UX-10 | 成功动作没有回执：创建成功、保存成功都只在 Console 留一行 | 同 UX-03 的 `pushLog(Info)` 调用 | U3 | **fixed**（2026-11-03）：`notifySuccess()` 给 2 秒 Toast（保存/打开/新建/创建地形与角色/删除/停止播放），并顺手清掉「最近错误」 |
| UX-11 | ~~Console 无时间戳、无等级计数、无复制、无"仅看错误"快捷开关~~ | `ConsolePanel.cpp` 全文（约 70 行） | U6 | **fixed**（2026-11-03）：每行 HH:MM:SS 时间列（`LogEntry::time` + `currentLogClock()`）；Info/Warn/Error 各带计数；「仅错误」「全部」两个视图；「复制」导出当前可见行，与显示共用 `levelVisible()` 判据 |
| UX-12 | 错误发生但 Console 关着时，界面上没有任何标记 | 无错误计数徽标；`state.log` 无未读概念 | U6 | **fixed**（2026-11-03）：`unreadErrors` + 工具栏红色「错误 N」徽标；本帧新产生的错误不算已读（`errorRaisedThisFrame`），Console 真正显示过之后才清零 |
| UX-13 | ~~快捷键不可自定义，没有冲突检测，速查表与实际处理是两份代码~~ | `kShortcutEntries` 现在同时驱动 `handleShortcuts()` 与速查表（key/ctrl/shift/action）；键位文本由 `shortcutKeys()` 现算 | U1 | **fixed**（2026-11-03）：单一数据源；自检扫空项与键位冲突（`entries=18 conflicts=0`）；用户自定义键位另算一项 |
| UX-14 | ~~面板位置被 `ImGuiCond_Always` 钉死，用户拖不动~~ | `drawPanels()` 现在对所有面板都用 `layoutNeedsReset ? Always : FirstUseEver` | U6 | **fixed**（2026-11-03）：Viewport 也能拖动/缩放，位置记进 `config/imgui.ini`；`View -> Reset Layout` 重新铺一遍 |
| UX-19 | 窗口固定 1920x1080 创建，不读显示器工作区；字体固定 16px，无 DPI 缩放；面板宽度硬编码 | `src/Core/FrameInfo.h:11-12`、`src/Core/Application.cpp:980`、`src/Core/ImGuiSystem.cpp:60`、`drawPanels()` | U10 | 窗口：fixed；字体缩放：open |

### UX-09 补充

这条容易被误判为"功能缺失"。它不是：多选本身可用（批量删除、批量复制、整组拖拽都已实现），
缺的是**多选状态下的属性编辑**。对用户而言，"不支持"三个字等于死路。

批量出口已经在 E1 补齐了（`InspectorPanel.cpp:60-111`：`N entities selected` + Duplicate / Delete 按钮），
所以这条剩下的只是**措辞**：那两行英文既暴露实现状态，又和中文界面混排。

```text
已选中 3 个对象
批量操作：复制 / 删除
要改属性：在 Hierarchy 里点其中一个（多选会变成单选）
```

修法就是把 113-114 两行换成上面第三句，并把按钮那两行的英文标签一起收敛（UX-07 的同类问题）。

### UX-19 补充

这条在 P1 表里，但**在 1366x768 笔记本或 150% 缩放屏幕上应视为 P0**：
窗口比工作区大时标题栏被推出屏幕，用户既拖不动也找不到关闭按钮，
首个闭环在第一步就断了。

判定方法（窗口部分已修，2026-11-03）：启动日志里应出现
`[Window] sizing | workarea=<w>x<h> scale=<sx>x<sy> -> <w>x<h> (fits|clamped)`；
`FirstApp::computeWindowSize` 按工作区 92% 收敛，1920x1080 的固定尺寸在
1280x720 工作区会变成 1177x662。仍 open 的是**字体固定 16px、不随 DPI 缩放**：
150% 缩放下中文会偏小。回归：`b1-window-fit`。

## 5. P2：打磨项

| 编号 | 现象 | 证据 | 判据 | 状态 |
|---|---|---|---|---|
| UX-15 | ~~状态栏右侧把 FPS、毫秒、draws、tris、undo 五组数字挤在一行，扫读成本高~~ | `drawStatusBar()` 的 `snprintf(right, ...)` | U6 | **fixed**（2026-11-03）：状态栏分左/中/右三段 —— 左段编辑上下文、中段场景统计（绘制 / 三角形，tooltip 给精确值，数字用 `formatCount()` 缩写）、右段帧率 / 帧耗时 / 撤销深度；统计读 `getDisplayTelemetry()`（上一帧完整值，读 `getTelemetry()` 会永远是 0） |
| UX-16 | Assets 扫描不到模型目录时只写 `cerr`，面板空白无解释 | `AssetBrowserPanel.cpp:69` | U3、U5 | **fixed**（2026-11-03）：面板内显示原因 + 目录路径 + 下一步（跑 SetupDependencies.ps1 / 手动放模型），另有「没有模型」与「目录缺失」两种文案 |
| UX-17 | 打开失败后没有"重试/换路径"入口，只能重新走 File > Open | `openScene` 失败分支 | U4 | **fixed**（2026-11-03）：失败时记录 `failedOpenPath/Error` 并弹「无法打开场景」模态框，三个按钮 = 重试 / 选择其它文件 / 关闭；弹窗在帧内按需打开，避免启动路径在帧外调 `OpenPopup` |
| UX-18 | ~~菜单栏没有"最近失败"或诊断入口，出错后无处可去~~ | `drawMenuBar()` 只有 File/Edit/Create/Play/View/Help | U4 | **fixed**（2026-11-03）：`帮助` 菜单三项 —— 打开控制台并跳到最近错误 / 复制诊断信息（`copyDiagnostics()` 拼场景、FPS、draw calls、撤销深度、最近错误进剪贴板）/ 关于 Tiny Editor |
| UX-09 | 多选时 Inspector 仍是两行英文播报"Multi-entity editing is not supported yet; use the Hierarchy panel to pick one entity."，且与中文界面混排 | `InspectorPanel.cpp:113-114` | U5、U7 | **fixed**（2026-11-03）：改成中文两行「已选中 N 个对象，Inspector 一次只编辑一个。／在 Hierarchy 里点单个对象查看属性；批量删除/复制用上面的按钮。」 |

## 6. 已经满足的，不要重做

这一节同样重要：LLM 拿到"优化 UI"的指令，最常见的浪费是把已经做好的部分再做一遍，
顺手改坏。

| 能力 | 证据 | 为什么算达标 |
|---|---|---|
| 工具栏按钮有 tooltip 且带快捷键 | `drawToolbar()` 每个 `Button` 后跟 `IsItemHovered` + `SetTooltip` | U1 的"可发现性"在按钮层已满足 |
| 不适用动作会变灰而不是消失 | `BeginDisabled` / `MenuItem` 的 enabled 参数 | U1 的正例 |
| 未保存提示是三选一且失败不吞意图 | `drawUnsavedPrompt()`，保存失败时停在弹窗 | U8 的正例 |
| 快捷键速查表可 `?` 唤起，且按组分类 | `kShortcutEntries` + `drawShortcutSheet()` | U1 的兜底层已存在 |
| 最近场景、面板可见性、gizmo 模式跨会话保存 | `EditorPreferences` | U6 的正例 |
| 状态栏常驻选中数、模式、吸附、脏标记 | `drawStatusBar()` | U6 大部分已满足 |

## 7. 复现一次审计

```powershell
# 1. 构建（若已有 build/Debug/TinyEditor.exe 可跳过）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1

# 2. 冷启动，不读文档，从零走到存盘
.\build\Debug\TinyEditor.exe --editor

# 3. 逐条核对 P0 的证据是否仍然成立
Select-String -Path src\Editor\EditorLayer.cpp -Pattern "pushLog\(LogEntry::Level::Error"
Select-String -Path main.cpp -Pattern "help|usage"
Select-String -Path src\Editor\panels\InspectorPanel.cpp -Pattern "not supported"
```

第三步的三条命令分别对应 UX-03、UX-02、UX-09。命令还有输出，说明条目仍然成立；
输出为空，说明已经被修掉，把状态改成 `fixed` 并注明日期。

## 8. 维护规则

1. 条目编号不复用。修掉后保留行，状态改 `fixed`，加一列"修法"指向提交。
2. 新增条目必须带：现象、证据（文件 + 行号或命令）、影响、对应判据编号。
   缺证据的条目不要进表，宁可先放 [known-limits](../70-status/known-limits.md)。
3. P0 的定义不随进度放宽：只要"陌生人走不完首个闭环"仍成立，就还是 P0。
4. 判据本身要改，去 [ux-rules](ux-rules.md) 改，不要在这里就地解释。

## 9. 相关

- 判据定义：[ux-rules](ux-rules.md)
- 改造协议：[ux-playbook](ux-playbook.md)
- 面板实现：[panels](panels.md)
- 已知边界：[known-limits](../70-status/known-limits.md)
- 技术债：[tech-debt](../70-status/tech-debt.md)
