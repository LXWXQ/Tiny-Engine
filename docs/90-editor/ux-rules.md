---
id: editor/ux-rules
title: 多用户友好性判据
type: validation
scope: src/Editor/, src/Core/ImGuiSystem.cpp
read-when: 改任何用户可见的按钮、菜单、提示、错误反馈或空状态之前
size: ~310 行
updated: 2026-11-03
---

# 多用户友好性判据

## 1. 这篇解决什么

把"界面友好"拆成十条**可以当场判定过或不过**的判据，每条给出本项目的实现模式。

它服务的是一个具体场景：把 `TinyEditor.exe` 交给一个**没有作者在旁边解释**的人，
让他从冷启动走到"放一个物体、存盘、再打开"。走不通的地方，就是判据不通过的地方。

三条当前事实决定了这篇必须存在：

| 事实 | 证据 |
|---|---|
| 编辑器模式默认不加载 Sponza，启动即空视口 | `src/Core/Application.cpp:132` 注释、`Scene::init()` 分支 |
| 可执行文件没有帮助入口，参数只写在 `main.cpp` 顶部注释里 | `main.cpp` 无 `--help` 分支（全文检索无匹配） |
| 所有失败路径只写内存日志，不弹窗、不改标题栏 | `src/Editor/EditorLayer.cpp` 中 7 处 `Error` + 3 处 `Warning` 的 `pushLog(...)`，无一处改状态栏 |

这不是"加几个提示"的问题：三条事实叠加的结果是——**陌生人第一次打开看到空画面，
想做的第一件事失败时，屏幕上什么都不会发生**。

## 2. 使用者分档

| 角色 | 作者是否在场 | 会问几次 | 判据按哪一档 |
|---|---|---|---|
| 作者 | 是 | 0（读代码） | 不作为验收对象 |
| 半熟人 | 口头带过 | 1 次 | 只验 U1、U4 |
| 陌生人 | 否 | 0 | **全部十条** |

判据一律按"陌生人"档判定。理由不是苛刻，而是因为陌生人的失败是**一次性**的：
他不会回来告诉你哪里卡住了，他只会关掉程序。

## 3. 判据总表

| 编号 | 判据 | 一句话判据 | 现状 |
|---|---|---|---|
| U1 | 入口存在 | 每个功能都能在菜单里点到，快捷键只是加速器 | 不满足，见 [UX-06](ux-audit.md) |
| U2 | 即时反馈 | 任何输入一帧内（≤16ms）有可见响应；超过 500ms 的操作必须有忙碌指示并可取消 | 部分不满足，见 [UX-08](ux-audit.md) |
| U3 | 结果反馈 | 成功与失败都在视线内出现，不依赖面板是否打开 | 不满足，见 [UX-03](ux-audit.md) |
| U4 | 失败可行动 | 错误 = 现象 + 原因 + 下一步 | 不满足，见 [UX-03](ux-audit.md) |
| U5 | 空状态可继续 | 空面板告诉你这里本该有什么、现在点哪里 | 不满足，见 [UX-01](ux-audit.md) |
| U6 | 状态可见 | 模式、选中、脏标记、进度都不用猜 | 部分满足，见 [UX-12](ux-audit.md) |
| U7 | 单一术语 | 一个概念一个词，同一层级不混语言（语言分层见下） | 满足（B5） |
| U8 | 破坏可逆 | 破坏性动作能撤销，或二次确认（范围：场景内容；引擎预设与播放状态按设计不进撤销栈，见 [milestones](milestones.md) 的 E3 / E5-b） | 满足 |
| U9 | 降级可见 | 字体、配置、能力的降级必须说出来 | 不满足，见 [UX-05](ux-audit.md) |
| U10 | 尺度适配 | 界面在 125%/150% 缩放与小屏上都可读可点 | 不满足，见 [UX-19](ux-audit.md) |

"现状"列只写结论，细节与行号全部登记在 [ux-audit](ux-audit.md)，避免同一事实两处维护。

## 4. 逐条判据

### U1 入口存在

**通过条件**：功能在菜单栏有可点击项；菜单项是动词短语；快捷键显示在菜单项右侧。

**失败现象**：功能只能靠按键触发。用户的实际行为是先扫菜单，扫不到就认为不存在，
之后不会再试第二次。

**本项目反例**：`Ctrl+D` 复制、`Ctrl+A` 全选、`F` 聚焦、`Delete` 删除都实现并写进了
速查表（`src/Editor/EditorLayer.cpp:260` 起的 `kShortcutEntries`），但 `Edit` 菜单只有
Undo / Redo / Move / Rotate / Scale（同文件 `drawMenuBar()` 的 Edit 分支）。

**检查方式**：把 `kShortcutEntries` 每行的 action 拿去 `drawMenuBar()` 里搜。
搜不到就是 U1 违规。这是一条可以用三十行脚本自动化的检查。

**实现模式**：

```cpp
// 菜单项是唯一入口，快捷键是同一入口的加速器。
// 菜单、快捷键、速查表三处必须调用同一个函数，否则会出现
// "菜单点了没反应"这种最难查的分裂。
const bool canDuplicate = !state.selection.empty();
if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canDuplicate))
    duplicateSelection(state);
```

`MenuItem` 的第四个参数不要写死 `true`。灰掉的菜单项本身就是一种反馈：
它告诉用户"这个动作存在，但现在不适用"，比隐藏菜单项更有信息量。

### U2 即时反馈

**通过条件**：按下到画面变化不超过 100ms。超过 200ms 的操作必须有忙碌指示。

**失败现象**：点了没反应，用户重复点击，或者以为程序卡死。

**本项目风险点**：地形生成是同步完成的，耗时只在其后的日志里出现
（`lastGenerationMs`，见 `src/Editor/EditorLayer.cpp` 的 `createTerrainFromUi()`）。
生成期间界面没有进度、没有禁用按钮，用户唯一的感受是"卡了一下"。

**实现模式**：

1. 状态先改，重活后做。点击的当帧就把选中、脏标记、日志行写进去，
   让用户立刻看到"系统收到了"。
2. 超过 200ms 的活，状态栏出现 `生成地形中 3/8`，并把触发按钮 `BeginDisabled`。
3. 不要在重活外面套 Modal 转圈：ImGui 的 Modal 会吞掉输入，而生成过程往往还需要能取消。

### U3 结果反馈

**通过条件**：每个动作结束后都有一次可见结果；成功也要说。

**失败现象**：失败只 `pushLog(Error)`。Console 面板可以被 View 菜单关掉，
关掉之后错误就彻底消失——用户看到的只是"我按了保存，然后什么都没发生"。

**本项目反例**：保存失败、打开失败、创建失败、播放恢复失败，全部走
`state.pushLog(LogEntry::Level::Error, ...)` 一类调用，共 10 处（7 Error + 3 Warning），
没有一处改变状态栏或弹窗。

**实现模式**：双通道。

| 通道 | 位置 | 生命周期 | 用途 |
|---|---|---|---|
| 状态栏 | 底部左侧 | 常驻，直到下一次操作 | 当前结果、错误摘要 |
| Toast | 工具栏下方浮层 | 3 秒，可点击跳到 Console | 一次性动作的回执 |

```cpp
// 最小 Toast：不进 ImGui 窗口栈，不抢焦点，不阻塞输入。
struct Toast
{
    std::string text;
    double hideAt = 0.0;
    bool isError = false;
};

void pushToast(EditorState& state, std::string text, bool isError)
{
    state.toast.text = std::move(text);
    state.toast.isError = isError;
    state.toast.hideAt = ImGui::GetTime() + 3.0;
}
```

Modal 只留给"必须二选一才能继续"的场景（未保存提示就是正确用法）。
把普通成功/失败做成 Modal，是把工作流打断，比没有反馈更糟。

### U4 失败可行动

**通过条件**：错误信息三要素齐全——发生了什么（对象 + 动作）、为什么（错误码或路径）、
下一步（按哪个键、改哪个配置、去哪看）。

**本项目反例**：`"[Editor] save failed: " + error`，其中 `error` 来自
`SceneSerializer` 的 `"write failed: " + filepath`（`src/Editor/SceneSerializer.cpp:299`）。
用户知道写失败了，但无法区分是权限、磁盘满、还是路径不存在。

**实现模式**：错误在产生点带上系统错误分类，在 UI 层补一句动作建议。

```text
保存失败：Resources/Scenes/demo.scene.json
原因：Access denied（目录只读）
可以：File > Save As 换到可写目录，或检查该目录权限
```

底层要保留 `std::error_code` 或平台错误码，不要一路 `std::string` 拼到顶层——
拼到最后只剩下"失败"两个字。

### U5 空状态可继续

**通过条件**：任何空面板、空视口，都显示两句：这里本该有什么；现在点哪里能得到。

**本项目反例**：

- 编辑器启动不加载 Sponza，视口是空的，没有一行字解释。
- Hierarchy 为空时只是一棵空树。
- Assets 扫描不到 `Resources/Models` 时只写 `std::cerr`
  （`src/Editor/panels/AssetBrowserPanel.cpp` 的 `refresh()`），界面上是一片空白。

**实现模式**：三段式，且按钮必须调用与菜单相同的函数。

```cpp
if (state.scene->entityCount() == 0)
{
    ImGui::TextDisabled("场景是空的");
    if (ImGui::Button("创建一个地形"))
        createTerrainFromUi();
    ImGui::SameLine();
    ImGui::TextDisabled("或从 Assets 双击一个模型");
}
```

空状态不是"欢迎页"，不要做成一次性弹窗。它应该一直存在，直到场景里真的有东西。

### U6 状态可见

**通过条件**：当前 gizmo 模式、选中数、脏标记、播放状态、后台任务，全部在状态栏可读，
不需要打开任何面板。

**现状**：大半已满足，`drawStatusBar()` 已给出选中数、模式、吸附、FPS、draws、tris、undo 深度。

**缺口**：没有"正在做什么"的位置。生成地形、加载场景这类会阻塞的操作，
状态栏没有任何变化。建议把状态栏分三段固定用途：

| 位置 | 内容 |
|---|---|
| 左 | 选中、模式、吸附 |
| 中 | 临时任务与进度（空闲时留空） |
| 右 | 性能与撤销深度 |

中间段留空是必要的：它一出现内容，用户就知道"有别的事在跑"。

### U7 单一术语

**通过条件**：同一概念在菜单、tooltip、状态栏、日志里用同一个词；同一层级不混语言。

**语言分层（B5 起执行）**：

| 层级 | 用哪种语言 | 理由 |
|---|---|---|
| 命令与动作（菜单项、按钮、tooltip） | 中文 | 用户读它做决定，中文最短的理解路径 |
| 区域名（`Hierarchy` / `Inspector` / `Console` / `Assets` / `History` / `Viewport` / `Render`） | 英文 | 它同时是 `EditorPanel::name()`、`imgui.ini` 的键、`config/editor.json` 里偏好 map 的键；翻译会拆掉这三处的一致性 |
| 行业标准参数名（FPS、draw calls、Base Color、Metallic、Roughness、Bloom、SSAO） | 英文 | 译成中文反而要在脑内翻译回文档和引擎术语 |
| 日志与提示（Console、Toast、状态栏文案） | 中文 | 它是解释，不是标识 |

短横线或 `·` 做分隔，不用 `|`：状态栏以前用 `|` 把五组数字串成一行，读起来像命令行。

**词汇表**：

| 概念 | 界面用词 | 禁止的说法 |
|---|---|---|
| 三轴操作器 | 手柄（解释）/ Move-Rotate-Scale 三键（`移动` / `旋转` / `缩放`） | handle、操纵器 |
| 场景未保存 | 未保存（`* 未保存` / `已保存`） | dirty、modified |
| 选中集 | 选中对象（`%zu 个对象选中`） | selection、当前选择 |
| 快捷键 | 键盘快捷键 | 热键、快捷方式 |

注意：`docs/30-systems/imgui.md` 第 3 节仍写着"文字必须 ASCII"，而
`src/Core/ImGuiSystem.cpp:45-65` 已加载系统中文字体（E4-c）。这是文档漂移，
按 [doc-rules](../00-meta/doc-rules.md) 第 5.2 节应回写该页，不是 UI 设计问题。

### U8 破坏可逆

**通过条件**：删除、覆盖、停止播放都能撤销，或有一次明确确认。

**现状**：满足。删除与复制走命令栈（`DeleteEntityCommand` / `DuplicateEntityCommand` /
`CompositeCommand`），未保存场景用三选一 Modal（`drawUnsavedPrompt()`）。

**规则**：新增破坏性动作时先问"能不能做成 `Command`"；能，就不要用 Modal。
Modal 是最后手段，因为它要求用户读懂一段文字才能继续。

### U9 降级可见

**通过条件**：任何能力降级，必须在界面上说一次，并说明影响范围。

**本项目反例**：中文字体找不到时静默回退内置字体，中文全部变成 `?`。
`src/Core/ImGuiSystem.cpp:47` 的注释自己写了这个后果，但界面上没有任何提示。

**实现模式**：启动后第一帧检查字体数量，只有内置字体时在状态栏显示一次红色提示。

```cpp
// 只有内置字体 = 中文不可用。用户看到的是满屏问号，
// 但真正的原因是字体没加载成功，必须把原因说出来。
const bool cjkMissing = ImGui::GetIO().Fonts->Fonts.Size <= 1;
```

同类降级：`VK_EXT_device_fault` 不可用、`timelineSemaphore` 缺失、配置文件损坏回退默认，
都按这条处理。降级不是错误，静默降级才是。

### U10 尺度适配

**通过条件**：在 100% 与 150% 系统缩放下、在 1280x720 与 3840x2160 两种分辨率上，
菜单栏、工具栏、状态栏都完整可见，正文可读，控件可点。

**失败现象**：窗口比屏幕大，标题栏被推出工作区，用户既拖不动也关不掉；
或者 4K 屏上字体小到无法辨认。

**本项目反例**：

- 窗口固定为 `WIDTH x HEIGHT` 创建，不读显示器工作区，也不 clamp 到可用区域
  （`src/Core/FrameInfo.h:11-12` 定义 1920x1080，`src/Core/Application.cpp:980` 直接使用）。
- ImGui 字体固定 16px（`src/Core/ImGuiSystem.cpp:60`），全文无 `FontGlobalScale`，
  也无 `glfwGetWindowContentScale` 调用。
- 面板宽度硬编码：左 280、右 340、底 200（`drawPanels()`），不随窗口宽度收缩。

**影响**：1366x768 笔记本上窗口超出屏幕；4K + 150% 缩放下字小、面板挤。
这是"多用户"最字面的一层——别人的屏幕和你的不一样。

**实现模式**：

1. 创建窗口前查主显示器工作区，取 `min(1920, workarea_w * 0.9)`，不要直接用常量。
2. 启动时读 `glfwGetWindowContentScale`，同时做 `style.ScaleAllSizes(scale)`
   与 `io.FontGlobalScale = scale`，只做一个会导致控件与文字不同步。
3. 面板宽度改成比例 + 上下限，例如 `clamp(width * 0.15f, 220.0f, 340.0f)`。

注意：改动窗口尺寸会影响 `--screenshot` 的像素尺寸，截图基线要一起更新。
这类"改了 UI 也改了基线"的改动，必须单独一批，不要和别的 UX 条目混在一起。

## 5. 判定流程（三十秒版）

1. 关掉 Console、History、Assets，只留视口。
2. 冷启动 `TinyEditor.exe`，不读任何文档。
3. 从"我要往场景里放一个物体"开始，做到"存盘并重新打开"。
4. 每卡一次记一条；每出现一次"点了没反应"记一条。
5. 单步卡住超过 60 秒，判定对应判据不通过。
6. 把系统缩放改为 150%，重复第 1 至 3 步。菜单栏、工具栏或状态栏任一被裁掉，
   或文字小到需要凑近看，判 U10 不通过。

第 1 步是关键：默认布局下 Console 是开着的，作者会不自觉地盯着日志看，
从而看不见"界面上其实什么都没发生"。

## 6. 相关

- 现状与行号：[ux-audit](ux-audit.md)
- 改造协议与验收：[ux-playbook](ux-playbook.md)
- 面板与布局：[panels](panels.md)
- 文本与字体约束：[imgui](../30-systems/imgui.md)
- 验收标准（回归视角）：[acceptance](../50-validation/acceptance.md)
