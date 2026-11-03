---
id: editor/panels
title: 面板与布局
type: system
scope: 现有面板、布局实现方式、以及没有 docking 时的注意事项
read-when: 写新面板、改布局、或面板位置错乱
size: ~200 行
updated: 2026-11-03
---

# 面板与布局

## 现有面板

| 面板 | 职责 |
|---|---|
| Hierarchy | 实体树，选中 / 拖拽重挂 / 右键删除；runtime `TerrainChunkComponent` 默认隐藏 |
| Inspector | 选中实体的组件编辑（变换/地形参数走命令栈；地形 Generate 单独重建） |
| Console | 分级过滤的日志，自动滚动 |
| Assets | 扫描 `Resources/Models`，双击创建实体（走命令栈，可撤销） |
| Viewport | 离屏纹理 + 点选 + 三轴手柄（见 [viewport](viewport.md)、[gizmo](gizmo.md)） |

## Inspector 地形分组

地形实体多一个 `Terrain` 分组，放在 Transform 之后。参数控件（seed、分辨率、
尺寸、高度、噪声/形态、Chunk Grid X/Z、LOD Levels、LOD Distance、Surface Detail、UV Tiling、Surface Style）只修改 `TerrainComponent::config`，并通过
`SetTerrainConfigCommand` 进入合并窗口；拖一次滑块只产生一条可撤销命令。`High Detail 257` 只把分辨率预设为上限，仍需点击 Generate 重建网格。

流送 manager 在 Inspector 显示 `Streamed Terrain`：load radius、unload padding、
max loads/frame、mapped chunk 数、GPU 高度场 in-flight 数与最近 update ms。
删除该 manager 会级联删除 runtime chunks，撤销会重新 adopt 并流送。

`Generate` 调用 `Scene::requestRegenerateTerrain()`：GPU 高度场请求异步轮询，
CPU 网格交给 JobSystem，旧网格继续渲染。生成期间再次点击 Generate 会用当前参数 supersede
旧任务并触发协作取消；界面显示
`Generating on JobSystem... press Generate to replace.`。主线程完成 GPU 上传后，
成功回调把 old/new 快照包装成 `RegenerateTerrainCommand`；失败则显示
`lastError`，旧网格保留。`Randomize` 只改 seed，`Reset` 只恢复默认值，都不自动重建；
只有 Chunk Grid X/Z、LOD Levels 等几何相关参数与 `generatedConfig` 不一致时显示
"Parameters changed, press Generate."。`Surface Detail`、`UV Tiling`、
`Surface Style` 与 `LOD Distance` 立即生效，不强制重建。

## 多选与搜索（E1）

`Selection` 保留 `std::vector<entt::entity>`，新增 `add/remove/toggle/size`：

- **Hierarchy**：Ctrl 点击切换单个、Shift 点击按"本帧展平顺序"选区间、空白处清空；顶部 `Search...`
  过滤框在非空时把树拍平成匹配列表（大小写不敏感子串）。右键菜单在多选时变成 `Delete Selected` /
  `Duplicate`。
- **Viewport**：Ctrl 点击 `toggle`，普通点击替换，点空白清空（按住 Ctrl 时不清）。
- **Inspector**：选中数 > 1 时只显示 `N entities selected` + `Duplicate` / `Delete` 按钮与提示，
  不再逐组件编辑（多对象编辑留到后续版本）。
- **快捷键**：`Delete` 批量删除、`Ctrl+D` 复制、`Ctrl+A` 全选（跳过地形 chunk 与相机）、`F` 聚焦
  （沿当前视线把相机退到包围球刚好占视野的位置）。

地形 chunk / 流送管理器实体不参与这些批量操作：它们由流送系统回收，删掉会让地图出现空洞。

## 面板可见性与偏好（E4-b）

本机习惯存 `config/editor.json`（**不进版本库**，`.gitignore` 已加）。它和 `config/engine.json` 分开：
那份是引擎运行参数，进版本库当默认预设；这份是本机习惯，换台机器就该是默认值。

```json
{
  "version": 1,
  "panels": { "Hierarchy": true, "Inspector": true, "Console": false },
  "gizmoMode": 0,
  "recentScenes": [ "Resources/Scenes/main.scene.json" ]
}
```

- **键是面板名**（`EditorPanel::name()`）：加面板只需 `registerPanel<T>()` + View 菜单 +1 行，
  偏好文件自动跟着走；缺失的键按 `true` 处理，所以删掉某个面板不会留下"隐形"状态。
- **写盘是延迟合并的**：`endFrame()` 把 `EditorState` 投影到 `EditorPreferences`，变化才标脏，
  再等 0.75s 落盘 —— 连点 View 里的开关只产生一次 IO。
- **读失败不是错误**：首次运行没有文件、JSON 坏了、字段类型不对，都退回默认值并留一条日志，
  不会让编辑器起不来（`gizmoMode` 会 clamp 到 0..2）。
- **File → Open Recent**：最近 8 条（`kMaxRecentScenes`），去重 + 置顶；打开时会先走
  `requestDocumentAction` 的未保存确认，所以它不会绕过脏文档保护。

## 快捷键速查（E4-c）

Help → Keyboard Shortcuts（或直接按 `?`）弹出速查表，`Esc` 关闭。菜单、弹窗、自检读的是同一张
`kShortcutEntries` 表（`src/Editor/EditorLayer.cpp` 匿名命名空间）：加一个快捷键时只改表 + 真正处理它的
分支，不会出现"文档写了但没实现"。自检会校验表项非空、键不重复，并打印条目数。

`?` 用 `Shift+Slash` 判定，所以单独的 `/` 不会被抢；所有快捷键在 `io.WantTextInput` 为真时让路 ——
在搜索框/命名框里打字不会误触删除或复制。

### 中文字形

ImGui 自带字体只有 ASCII，之前编辑器里的中文日志、模态框按钮、提示全部显示成 `?`。
`src/Core/ImGuiSystem.cpp` 在 `CreateContext()` 之后从系统字体里挑第一个可用的装进去：

1. `C:/Windows/Fonts/msyh.ttc`（微软雅黑）
2. `C:/Windows/Fonts/msyhl.ttc`
3. `C:/Windows/Fonts/simhei.ttf`
4. `C:/Windows/Fonts/simsun.ttc`

加载成功打印 `[ImGui] font: <path>`；都找不到就保持内置字体（英文照常，中文仍为问号），
**不随仓库分发字体**，避免授权问题。ImGui 1.92 起字形按需加载，不需要声明 GlyphRanges。

## 布局：为什么是手写的

本项目的 ImGui 是 **1.92.6 非 docking 分支**（没有 `DockSpace`）。
所以布局靠 `SetNextWindowPos` / `SetNextWindowSize` 手动排：

```
+-------------------------------+------------------+
| Hierarchy (280px) |  3D 视口  | Inspector (340px)|
|                   |           |                  |
+-------------------------------+------------------+
| Console (200px)                                  |
+--------------------------------------------------+
```

菜单栏用 `BeginMainMenuBar()` 固定在顶部。

## 视口窗口必须 NoMove / NoResize

`Viewport` 是"计算出来的固定区域"，不是可拖动的浮动窗口。标志里少了这两个：

```cpp
ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
```

后果是**在视口里按住左键拖拽会把整块视口拖走**（想转视角或拖手柄时最容易触发），
而且位置会被 `imgui.ini` 记住，下次启动依然是错的。

布局里视口用 `ImGuiCond_Always` 而不是 `FirstUseEver` —— 它是算出来的，
不该被历史 ini 影响。其余面板（Hierarchy / Inspector / Assets / Console）
仍然可自由拖动缩放。

## 坑：SetNextWindowPos 是单槽位的

```cpp
// 错的：三次调用只有最后一次生效，Console 的矩形会套到 Hierarchy 上
ImGui::SetNextWindowPos(hierarchyPos);
ImGui::SetNextWindowPos(inspectorPos);
ImGui::SetNextWindowPos(consolePos);
for (panel : panels) panel->draw();

// 对的：每个 Begin 之前紧邻设置它自己的矩形
for (panel : panels) {
    if (name == "Hierarchy") ImGui::SetNextWindowPos(...);
    panel->draw();
}
```

`SetNextWindowPos` 只影响**下一个 `Begin` 的窗口**。

## 窗口布局持久化

`io.IniFilename = TINY_IMGUI_INI_PATH` → `config/imgui.ini`。

为什么不放默认的 `imgui.ini`（工作目录）：仓库根目录只允许 `README.md`，
而且工作目录随启动方式变化（双击 / IDE / 脚本），会到处生成 ini。

首帧用 `ImGuiCond_Always` 强推默认布局，之后用 `FirstUseEver` 尊重用户拖动。
`View -> Reset Layout` 可以强制重排。

## 撤销历史（E4-d）

右栏上 65% 是 Inspector，下 35% 是 `History`：把 `CommandStack` 的每一层摊开成一行，
点任意一行就跳到那个深度。

- 数据来自 `CommandStack::undoEntries()` / `redoEntries()`，面板只读，不直接改栈；
- 跳转统一走 `CommandStack::jumpTo(targetUndoCount)`，它**一步一步** undo/redo，
  而不是整段搬运。原因：命令的 revert/apply 可能带 GPU 副作用（材质 factor 走 SSBO），
  整段搬运会跳过中间状态，把 GPU 缓冲留在半途；
- 播放中禁用跳转并给出提示（文档只读，E3b）；
- 深度 0 表示「撤销到底」，当前状态单独占一行并显示 `<当前状态>`，redo 行排在后面。

`jumpTo` 会把目标 clamp 到 `undoCount + redoCount`，越界不会崩。

## 写一个新面板

```cpp
class MyPanel : public EditorPanel
{
public:
    const char* name() const override { return "MyPanel"; }
    void draw(EditorState& state) override
    {
        if (!ImGui::Begin("MyPanel", &visible)) { ImGui::End(); return; }
        // ...
        ImGui::End();
    }
};
```

然后在 `EditorLayer::createDefaultPanels()` 里 `registerPanel<MyPanel>()`。

## 工具栏与状态栏（E5-a）

两者都不是 `EditorPanel`，而是 `EditorLayer` 直接画的**固定矩形**：位置和高度写死，
不使用 `SetNextWindowPos(..., FirstUseEver)`，也不进 `panels` 列表。原因很简单 ——
它们的矩形要参与面板布局计算，必须可预测；如果让 ImGui 记忆位置，用户拖一次工具栏，
整块工作区就会错位。

| 区域 | 高度 | 内容 |
|---|---|---|
| 工具栏 | 34 px | Play/Stop、Move/Rotate/Scale、Snap、Focus、Undo/Redo；右侧 `saved \| Editing \| 245 FPS` |
| 状态栏 | 26 px | 选中数量、当前手柄、吸附开关；右侧 FPS/帧耗时/draw calls/三角形/undo 深度 |

布局公式（`src/Editor/EditorLayer.cpp` 的 `drawPanels()`）：

```cpp
const float top = viewport->WorkPos.y + kToolbarHeight;
const float availableHeight = viewport->WorkSize.y - kToolbarHeight - kStatusBarHeight;
```

**两个坑**（都踩过，写在这里省下一次调试）：

1. `ImGui::BeginMainMenuBar()` 在第一帧还没有把主视口的 `WorkPos.y` 下推（那帧是 0），
   于是第一帧算出来的面板矩形会整体偏高一个菜单栏高度，而 `imgui.ini` 会把这个错位置记住。
   所以 `layoutNeedsReset` 要多留一帧：`if (viewport->WorkPos.y > 0.5f) layoutNeedsReset = false;`。
2. 菜单栏右侧原来也写了一遍「保存状态 / 播放状态 / undo 深度」，和工具栏、状态栏三处重复。
   E5-a 起菜单栏只显示文档名（脏时加 `*` 和 `(unsaved)`），状态只在一处出现。

## 渲染设置面板（E5-b）

`RenderSettingsPanel`（窗口名 `Render`）是一个**浮动工具窗**：不在 `drawPanels()` 的固定矩形里，
位置和大小只在自己 `draw()` 里用 `ImGuiCond_FirstUseEver` 给一次，之后由用户拖动、记在
`imgui.ini`。理由：它不参与工作区划分，压着视口反而方便看改动效果（调曝光要盯着画面）。
入口有两个：工具栏的 `Render` 按钮、`View -> Render Settings`。

它直接改 `state.services.settings` 指向的 `EngineSettings` 实例 ——
这正是 `Application` 每帧塞进 `FrameInfo` 的 `scene->getSettings()`（`src/Core/Application.cpp:2107` 起），
所以**改完下一帧就生效**，不需要重新加载配置。

| 分组 | 字段 |
|---|---|
| 曝光 / 分辨率 | `exposure`、`renderScale` |
| Bloom | `bloomEnabled`、`bloomThreshold`、`bloomKnee`、`bloomIntensity` |
| SSAO | `ssaoEnabled`、`ssaoRadius`、`ssaoBias`、`ssaoKernelSize` |
| 运动模糊 | `motionBlurEnabled`、`motionBlurStrength`、`motionBlurMaxRadius` |
| 方向光 | `directionalLightDir`（写回前归一化）、`directionalLightColor` 的 rgb 与 w（强度） |
| 提交路径（调试） | `gpuSceneEnabled`、`gpuMergedBuffers`、`hizEnabled`、`terrainBatchEnabled`、`gpuSceneMinInstances`、`gpuSceneMinScreenPixels` |

两个刻意设计：

- **不进撤销栈、不让文档变脏**。这些值是引擎默认预设而不是场景内容；它们和 Play 状态一样
  属于"运行期/环境"而不是"文档"，混进撤销栈会让 Ctrl+Z 变成不可预期的东西。
- **Revert 回到"本次启动时"**，不是回代码里的编译期默认值 —— 用户试坏一排滑杆时想回的是
  刚才那套配置（来自 `config/engine.json`），不是作者的出厂值。要永久保留则按
  `Save to engine.json`（走 `EngineServices::saveConfig`）。

`ssaoKernelSize` 由 `RenderSettingsPanel::clampKernelSize()` 夹到 4..64 的偶数：滑杆允许拖到任意
整数，但采样循环拿 0 或奇数会出问题。自检：`[Editor] render settings selfTest PASS | kernel clamped to 4..64 even`。

## 语言分层（B5 / UX-07）

界面用词按「读它做什么」分四层，规则写在 [ux-rules](ux-rules.md) 的 U7：

| 层级 | 语言 | 例子 |
|---|---|---|
| 命令与动作 | 中文 | `播放` / `停止` / `撤销` / `复制选中` |
| 区域名 | 英文 | 面板标题 `Hierarchy` / `Inspector` / `Console` / `Assets` / `History` / `Viewport` / `Render` |
| 行业参数名 | 英文 | `FPS` / `draw calls` / `Base Color` / `Metallic` / `Bloom` / `SSAO` |
| 日志与提示 | 中文 | Console、Toast、状态栏文案 |

区域名保留英文不是偷懒：它同时是 `EditorPanel::name()`、`config/imgui.ini` 的键、`config/editor.json` 里偏好 map 的键。翻译面板标题会同时拆掉这三处的一致性。

## Console：时间列、计数、过滤与复制（B5 / UX-11）

`src/Editor/panels/ConsolePanel.cpp`：

- 每行第一列是 `HH:MM:SS`（`LogEntry::time`，由 `EditorState::pushLog` 里的 `currentLogClock()` 填）。`sameLine(76.0f)` 之后才是正文，所以视线扫的是时间轴。
- 顶部三个复选框 `Info` / `Warn` / `Error` 后面各跟一个计数 `(N)`，一眼能看出有多少条被过滤掉了。
- 「仅错误」= Info/Warn 关、Error 开（排查时最常用）；「全部」= 三个都开。
- 「复制」把**当前可见的**行拼成 `时间 正文` 写入剪贴板 —— 与显示共用同一个 `levelVisible()` 判据，绝不出现「看到的和复制的不一样」。
- `清空` 与 `自动滚动` 沿用原语义；清空时同时清「最近错误」摘要与未读计数。

## Console 搜索（B6 / UX-11 续）

Console 顶部在「全部」按钮右侧有搜索框（占位文本 `搜索日志（Ctrl+F）`，宽度 200px），
`Ctrl+F` 会把焦点直接送进来。

- 判据是纯函数 `matchesSearch(const LogEntry&, const char* needle)`：空串匹配全部；
  非空时逐字节做 ASCII 大小写折叠的子串比较（中文按字节比较，不走 locale）——
  所以 `打开失败` 能命中，`打开成功` 不会。
- 与级别开关是**与**关系，和「复制」共用同一组判据：复制出去的就是屏幕上看见的行。
- 搜索词非空时出现「清除搜索」；一行都没匹配上时正文区给出提示，而不是留一片空白。

自检 `ConsolePanel::selfTest()` 覆盖级别三态、大小写、空串、中文四组，
打印 `[Editor] console filter selfTest PASS | level switches + case-insensitive search`。

## 地形创建的两阶段提示（B6 / UX-08）

> TD-24 之后 `CreateTerrainCommand` 默认走 `deferBuild`（异步构建），这一段的两阶段提示仍然保留：
> 第一帧画「地形生成中…」、第二帧提交后台作业 —— 提交本身只要 0.28 ms，但先让用户看到反馈更稳。
> 完成后 `EditorState::pendingTerrainBuild` 的轮询会再播报一条带顶点数与耗时的成功消息。



`Scene::createTerrainEntity` → `buildTerrainModel(...)` 是同步的，一次 build 会占满一帧。
真正异步要拆渲染侧的生成流程，这一批只解决"用户看不见它在干活"：

- 请求先入队：`EditorState::terrainCreatePending` + `terrainCreateWaitFrames`，
  辅助函数 `advanceTerrainCreateRequest(pending, waitFrames)` 返回"这一帧该执行了吗"。
- `Q_beginFrame` 里先 tick 一次：入队当帧返回 false（只画忙碌），下一帧才真正建地形
  —— 于是用户至少能看到一帧"地形生成中…"。
- 入口三处都走 `requestTerrainCreate()`：`Create -> 地形` 菜单、视口空状态卡、
  工具栏忙碌提示（黄字「地形生成中…」，在错误徽标之前）。
- 重复点击不会叠出两块地形（pending 期间直接 return）。
- 自检打印 `[Editor] terrain create request selfTest PASS | busy frame first, build on the next`。

遗留：地形生成仍然是同步的，chunk 数量再上一档就要挪到后台线程或分帧。

## 状态栏三段（B5 / UX-15）

`EditorLayer::drawStatusBar()`：

| 段 | 内容 | 为什么 |
|---|---|---|
| 左 | `N 个对象选中` · 手柄模式 · 吸附 · 最近错误摘要 | 「我现在在编辑什么」；错误常驻是 B3 的承诺 |
| 中 | `绘制 N · 三角形 270.5k` | 「屏幕上画了多少东西」；数字用 `formatCount()` 缩写（`270.5k` / `2.7M`），精确值挂在 tooltip |
| 右 | `167 FPS · 5.98 ms · 撤销 0` | 「跑得快不快」；帧率以前挤在工具栏右侧，和「文档是否保存」抢位置 |

中段和右段都用 `SameLine(targetX)` 定位，并且只有 `targetX > GetCursorPosX() + 8` 时才画 —— 左段很长（错误摘要）时宁可少一段，也不重叠。

统计读 `Scene::getDisplayTelemetry()` 而不是 `getTelemetry()`：前者是 `beginFrameTelemetry()` 在每帧开头搬过来的上一帧完整值，后者是本帧正在累加的草稿 —— 编辑器先画 UI 再录制渲染，读草稿永远是 0。

## 硬约束

- ImGui 可见文字优先用 ASCII：内置字体只有 ASCII；E4-c 起 `ImGuiSystem` 会尝试加载系统 CJK 字体
  （msyh/msyhl/simhei/simsun），找不到就退回内置字体，那时中文会渲染成 `?`。
  所以**关键按钮/标题**不要把语义完全押在中文上。
- 面板必须自己 `ImGui::Begin/End`（不 Begin 会把内容画进上一个窗口）
- 面板不得自己创建 GPU 资源
- 面板不得直接改 registry，必须走命令栈

## 空状态（B2）

编辑器冷启动**不加载 Sponza**（`src/Core/Application.cpp` 的 `loadDefaultScene` 在编辑器模式恒为 false），
所以「画面只有天空」是正常状态，不是渲染失败。三处空状态负责把这件事说出来：

| 位置 | 判据 | 文案要点 |
|---|---|---|
| 视口中央 | `editor::isSceneEmpty(registry)`（`src/Editor/EditorSceneQuery.h`） | 「场景是空的」+ 为什么只有天空 + 下一步（双击 Assets 模型 / 创建地形）+ 两个按钮 |
| Hierarchy | `filterBuffer` 为空且 `view<NameComponent>()` 为空 | 「场景里还没有对象。」+ 两个入口 |
| Assets | `models.empty()` 且 `scanError` 非空或为空 | 缺目录给原因 + 路径 + 跑 SetupDependencies.ps1；目录空给支持的格式与 Rescan |

`isSceneEmpty` 只看 `MeshComponent` 与 `TerrainChunkComponent`：天空盒与相机**不算内容**，
否则空场景永远不空。判据本身有回归（`[Editor] empty state selfTest PASS`）。

两个按钮都走命令栈（`createTerrainThroughCommand` / `CreateEntityCommand`），所以 Ctrl+Z 能撤销。

## 通知与错误可见（B3）

Console 可以被 View 菜单关掉，所以「错误只写进 Console」等于没写。B3 的统一出口是
`EditorState::notify(level, text)`：

| 通道 | 生存期 | 用途 |
|---|---|---|
| Toast | Info 2 秒 / 警告与错误 5 秒 | 做了有回应；错误带「查看」按钮 |
| 状态栏摘要 | 常驻，直到下一次成功动作 | 余光可见；点「错误」跳 Console |
| 工具栏徽标 | 常驻到 Console 被看到 | Console 关着时的唯一入口 |

三处的点击都走 `EditorLayer::openConsoleAtLastError()`：打开 Console、置 `focusConsoleRequest`、
滚到 `lastErrorLogIndex` 并用白字高亮那一行。

已读语义有一个刻意的例外：**本帧刚产生的错误不算已读**（`errorRaisedThisFrame`）。
否则 Console 恰好开着时，徽标会在同一帧里出现又消失，用户看到一个闪一下的红色按钮。

### 打开失败的重试（UX-17）

`openScene()` 失败时写 `state.failedOpenPath / failedOpenError`，`drawOpenFailurePrompt()`
每帧检查并弹「无法打开场景」模态框：重试 / 选择其它文件 / 关闭。
不要在 `openScene()` 里直接 `ImGui::OpenPopup` —— 它可能由启动参数在 ImGui 帧之外调用。

## 面板布局条件与拖动（B4 / UX-14）

`drawPanels()` 对所有内置面板用同一个条件：

```cpp
const ImGuiCond cond = layoutNeedsReset ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
```

首帧（以及点 `View -> Reset Layout` 后的那一帧）用 `Always` 把矩形铺到位，之后交给
`config/imgui.ini` 记忆：用户可以拖动、缩放、折叠任何面板，包括视口。E5-a 之前视口用的是
`Always`，理由是「视口是算出来的固定区域」—— 那个理由只在没有记忆时才成立，代价是用户
永远挪不动它，面板一多就没法让出空间。

`Reset Layout` 的实现没变：把 `layoutNeedsReset` 置真，下一帧所有面板重新落位，旧位置被覆盖。

`config/imgui.ini` 是**本机缓存**（已在 `.gitignore` 里，不进版本库）：删掉它就等于恢复默认布局。
典型事故：某次运行把 `[Window][Render]` 存成 `Collapsed=1`，之后每次启动渲染设置面板都是一条细线 ——
当场删掉那一段或点 `View -> Reset Layout` 即可，不用改代码。

## 贴图选择（E6）

Inspector 的 `Material` 段默认展开（`ImGuiTreeNodeFlags_DefaultOpen`），下面是 `贴图` 分组：Albedo / Normal /
MetalRough / Emissive / AO 五行，每行「当前文件名 + 选择…按钮」。

- 换图的完整链路：`TextureLibrary` 列出 `Resources/Texture` 下的候选 → `Scene::loadMaterialTexture(path, srgb)`
  （编辑器不持有 Device，所以加载入口放在 Scene）→ `SetMaterialTextureCommand` → `Material` 换指针后
  `DescriptorManager::refreshMaterialTextures(slot, material)` 重写材质 set。整条链走命令栈，Ctrl+Z 能还原。
- 只有 `.png/.jpg/.jpeg/.tga/.bmp` 进列表；HDR 是另一种采样（`VK_FORMAT_R16G16B16A16_SFLOAT` + 线性空间），
  混进来只会得到一张黑图，所以故意不做。
- 加载失败不静默：`notify(Error, "贴图加载失败：<path>")`，Toast + 状态栏 + Console 三处可见（B3）。

## 资产拖拽到视口（E6）

Assets 列表的每一项都是 `ImGuiDragDropSource`（载荷 `TINY_ASSET_PATH`，绝对路径字节串 + NUL）。
视口用 `ImGui::Image` 之后的 `BeginDragDropTarget` 接收，落点由纯函数
`ViewportPanel::assetDropPosition(terrainHit, hitPoint, cameraPosition, forward, 8.0f)` 决定：

- 命中地形 → 就用命中点（模型底边贴地，不用再抬高）；
- 没命中 → 相机前方 8 米（落在世界原点会让人以为"没生成"）。

生成同样走 `CreateEntityCommand`，所以 Ctrl+Z 一次撤掉；生成后自动选中并 Toast 提示文件名。

## 快捷键表（B4 / UX-13）

快捷键只有一份数据（`src/Editor/EditorLayer.cpp` 的 `kShortcutEntries`）：

```cpp
struct ShortcutEntry
{
    const char* group;        // 分组：文档 / 选择 / 变换 / 播放 / 视图
    ImGuiKey key;
    bool ctrl;
    bool shift;
    ShortcutAction action;    // 分发用的枚举
    const char* actionText;   // 速查表里那句人话
};
```

- `handleShortcuts()` 遍历这张表：修饰键**精确匹配**（所以 `Ctrl+Z` 不会顺带触发
  `Ctrl+Shift+Z`），命中一个就 `switch (entry.action)` 分发并 `return`（一帧只做一个动作）。
- 速查表的键位列由 `shortcutKeys(entry)` 现算（`Ctrl+` / `Shift+` + `ImGui::GetKeyName`），
  不另存字符串 —— 以前速查表写「Ctrl+D」、代码里判断 `ImGuiKey_D`，两边漂移过。
- `EditorLayer::selfTest()` 扫描这张表：条目数 ≥ 12、分组与描述非空、键位标签非空、
  (key, ctrl, shift) 三元组不重复，打印 `[Editor] shortcut sheet selfTest PASS | entries=19 conflicts=0`。
- `?` 就是 `Shift+/`：表里存的是 `ImGuiKey_Slash + shift`，所以显示成 `Shift+Slash`，
  描述里补一句「（Shift+/ 就是 ?）」。

菜单入口（UX-06）和快捷键是两套入口、同一个实现：Edit 菜单的
`Duplicate Selected` / `Delete Selected` / `Focus Selection` / `Select All` 与快捷键
调的是同一组函数（`duplicateSelection` / `deleteSelection` / `focusSelection` /
`selectAllEntities`），不存在「菜单能点、快捷键不生效」的分叉。
