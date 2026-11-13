---
id: editor/panels
title: 面板与布局
type: system
scope: 现有面板、布局实现方式、以及没有 docking 时的注意事项
read-when: 写新面板、改布局、或面板位置错乱
size: ~500 行
updated: 2026-11-10
---

# 面板与布局

## 现有面板

| 面板 | 职责 |
|---|---|
| Hierarchy | 实体树，选中 / 拖拽重挂 / 右键删除；runtime `TerrainChunkComponent` 默认隐藏 |
| Inspector | 选中实体时编辑实体组件（变换/地形参数走命令栈；地形 Generate 单独重建）；无选中时编辑场景级组件（scope=Scene） |
| Console | 分级过滤的日志，自动滚动 |
| Assets | 扫描 `Resources/Models`，双击创建实体（走命令栈，可撤销） |
| Viewport | 离屏纹理 + 点选 + 三轴手柄（见 [viewport](viewport.md)、[gizmo](gizmo.md)） |

Hierarchy 用 `HierarchyComponent::children` 画真正的父子树：`Voxel World` / 流送地形 manager 作为父节点，
体素 chunk 挂在世界下并按 `chunkZ, chunkX` 稳定排序，加载后默认展开（`ImGuiTreeNodeFlags_DefaultOpen` +
`DrawLinesFull` 树线），不再与 `Voxel World` / `Camera` 并列成根。搜索过滤仍按匹配项拍平，便于快速定位。

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

## Inspector 空选中：场景组件（F5-06）

没有选中实体时，Inspector 不再只显示“没有选中对象”，而是显示 **Scene Components**：
场景环境、阴影、Bloom、SSAO、SSR、运动模糊、TAA、水面、距离雾。这 9 个组件
（`scene.*`，`scope=Scene`）挂在 `Scene` 的隐藏设置实体上，不出现在 Hierarchy 或实体列表里。

- 顶部 `+ Add Scene Component` 只列 Scene scope 且未挂载的组件，支持按名称/分类/id 搜索；
  添加走 `SetComponentRecordCommand`，可撤销。
- 每个组件有 `enabled` 与功能参数；控件与实体组件共用 `drawPropertyWidgets`，同样有范围、
  拖动速度、悬停提示、右键重置、复制/粘贴/重置/移除。
- **移除组件 = 关闭该功能**；`enabled=false` 保留参数但同样关闭。组件值通过
  `Scene::syncSceneSettings()` 写回渲染器读取的 `EngineSettings` 缓存。
- 场景组件属于场景文档：编辑进入撤销栈，保存时写到 `.scene.json` v4 顶层
  `sceneComponents`；预制体只存实体，不携带这些功能。
- Render 面板里有“在 Inspector 中编辑场景组件”按钮：清空选中并把焦点带回这里。

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

## 布局：DockSpace + DockBuilder

项目使用 ImGui **1.92.6-docking**，`EditorLayer::drawPanels()` 不再给每个面板手算
`SetNextWindowPos/Size`，而是建一个全屏 host window `##DockHost`，再把面板停靠进
`TinyEditorDockSpace`：

```
+----------------------+-------------------------------+
| Hierarchy            | Inspector / Render（同节点 tab）|
|----------------------|-------------------------------|
| Assets               | History                       |
+----------------------+-------------------------------+
| Console                                              |
+------------------------------------------------------+
|                    Viewport（中央，无标题栏）          |
+------------------------------------------------------+
```

默认布局由 `DockBuilder` 首次生成：

- 左侧 21%，再上下 60/40：Hierarchy 上、Assets 下；
- 中央 Viewport；右侧 25%，再上下 66/34：Inspector 与 Render 同节点 tab、History 下；
- 底部 24%：Console；
- host 窗口自身 `NoTitleBar | NoCollapse | NoResize | NoMove | NoBringToFrontOnFocus |
  NoNavFocus | NoDocking | NoBackground`，零 padding/rounding/border。

`ImGui::DockSpace(..., ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar)`：
中央视口不画 dock 背景，单面板节点自动隐藏 tab 条。菜单栏/工具栏/状态栏仍由
`BeginMainMenuBar()` 与固定窗口负责，且带 `NoDocking`，不会被拖进 dock。

Render 与 Inspector 共享右侧节点。首次建布局、工具栏「渲染」开关打开、或
`View -> Render Settings` 打开时，`focusRenderPanel = true`，面板循环之后
`ImGui::SetWindowFocus("Render")` 把 Render 页签切到前台；否则它会藏在 Inspector 后面。

## 视口窗口的 flags

`Viewport` 不再是手写固定矩形，但仍然禁止用户把它当浮动窗口拖走/缩放：

```cpp
ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove |
ImGuiWindowFlags_NoResize
```

旧的 `NoSavedSettings` 已移除，否则 ImGui 不会把视口的 DockId 写进 `config/imgui.ini`，
重启后视口会掉出 dock。位置和大小现在由 dock 节点决定，`NoMove/NoResize` 只限制
浮动状态下的拖动。

## 窗口布局持久化

`io.IniFilename = TINY_IMGUI_INI_PATH` → `config/imgui.ini`。

为什么不放默认的 `imgui.ini`（工作目录）：仓库根目录只允许 `README.md`，
而且工作目录随启动方式变化（双击 / IDE / 脚本），会到处生成 ini。

首帧 `dockLayoutChecked` 在 `DockSpace()` 之前检查一次
`DockBuilderGetNode(dockspaceId) != nullptr`；没有保存布局才调 `DockBuilder` 拆分。
`View -> Reset Layout` 同时置 `layoutNeedsReset = true` 与
`forceDockLayoutReset = true`，下一帧强制重建默认 DockNode。删除
`config/imgui.ini` 等价于恢复默认布局。
## 编辑 / 运行模式（E8）

视口左上角只有一枚可点击的模式徽标（「编辑模式」/「运行模式」）。
工具栏和状态栏不再重复显示；快捷键 `F8`，旧的分辨率/手柄提示已移除：

| 模式 | 左键 | 右键 | 选中 | 手柄 | 面板 |
|---|---|---|---|---|---|
| 编辑 | 选中（`Ctrl` 切换多选、`Shift` 区间） | 无 | 允许 | 可用 | 可改参数 |
| 运行 | 挖方块 / 地形笔刷 | 在命中面放方块 | 禁止（进入时清空） | 不显示 | 只读 |

- 这是**输入归属**开关，和 `PlayState`（F5 播放）是两件事：播放管场景快照回滚，运行模式只管鼠标键盘落到谁身上；
- 进入运行模式会 `selection.clear()`，所以 Inspector 自然变成空态，不需要额外禁用控件；
- 层级面板、视口点选、快捷键全选/复制/删除都看 `state.allowsSelection()`，运行模式下不会改动选中；
- 自检打印 `[Editor] editor mode selfTest PASS | runtime blocks selection, edit allows`。

运行模式的具体操作（E9）：

- 进入时从相机沿视线 ray-march（2..180m）找地面，角色落在命中点；找不到才退化为相机前方 2m + 重力；
- `WASD` 走、`Shift` 跑、`Space` 跳；控制器实体由 `Scene::createCharacterEntity` 建，外观由 `attachCharacterModel` 挂 `Character.glb`（缩放 0.5），落地/上台阶由 `updateCharacterController` 负责；
- 动画状态机在 Idle / Walk / Run / Jump / Jump_Idle / Jump_Land 之间切换，详见 [character](../30-systems/character.md)；
- 按住鼠标中键拖动看四周（左右改 yaw、上下改 pitch，pitch 夹在 ±85 度）；
- 视口左键挖方块（或跑地形笔刷）、右键在命中面放方块，笔刷逻辑与编辑模式完全同一份代码；
- 第三人称相机：焦点在角色脚底 + 1.25m，相机退到焦点后方 4m；`F8` 回编辑模式时销毁角色并把相机还原到进入前的位姿；
- 自检打印 `[Editor] runtime character selfTest PASS | camera follows feet, exit restores pose`，并附带 `skinned=1 animations=18 clips=... active=Idle`。

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

## 工具栏、状态栏与 DockHost

两者都不是 `EditorPanel`，而是 `EditorLayer` 直接画的**固定矩形**：位置和高度写死，
不使用 `SetNextWindowPos(..., FirstUseEver)`，也不进 `panels` 列表；同时带
`ImGuiWindowFlags_NoDocking`，防止被拖进 DockSpace。

| 区域 | 高度 | 内容 |
|---|---|---|
| 工具栏 | 34 px | Play/Stop、Move/Rotate/Scale、Snap、Focus、Undo/Redo；右侧 `saved \| Editing \| 245 FPS`；模式徽标在视口左上角 |
| 状态栏 | 26 px | 选中数量、当前手柄、吸附开关、最近错误；右侧 FPS/帧耗时/draw calls/三角形/undo 深度 |

`##DockHost` 的位置和高度扣掉这两条：

```cpp
const float top = viewport->WorkPos.y + kToolbarHeight;
const float availableHeight = viewport->WorkSize.y - kToolbarHeight - kStatusBarHeight;
```

`BeginMainMenuBar()` 在第一帧还没有把主视口的 `WorkPos.y` 下推（那帧是 0），
所以首帧 `WorkPos.y <= 0.5f` 时先不建 DockBuilder 布局，等下一帧再拆；这不是
旧版的 `layoutNeedsReset` 防抖，DockNode 一旦建好就不再每帧重排。

## Render 组件目录（E5-b / F5-06 / F5-09）

`RenderSettingsPanel`（窗口名 `Render`）现在是**组件目录**，与 Inspector 共享右侧
DockNode 的 tab。首次建布局、工具栏「渲染」开关打开或 `View -> Render Settings`
打开时，`focusRenderPanel` 会把 Render 页签切到前台。

目录顺序与分组：

| 分区 | 数量 | 行为 |
|---|---|---|
| 单一性组件 | 12 | `scope=Scene`，每场景一份、无 Transform；已挂载显示「已挂载 · 点击编辑」 |
| 对象组件 | 15 | `scope=Entity`，创建带 Transform 的新实体并挂组件 |
| 系统对象 | 1 | 天空盒（`RenderTag.isSkybox` 实体），只查看/选中，不伪造创建参数 |

分区内按组件 `category` 首次出现顺序分组，两列表格；顶部搜索匹配 label/category/
type/description。点击可添加项打开「创建组件」模态框：

- 单一性组件：未挂载时显示默认参数 + 「创建并挂到场景」；已挂载时显示当前参数 +
  「应用修改 / 移除组件 / 取消」，走 `SetComponentRecordCommand`；
- 对象组件：名称/位置/旋转（度）/缩放 + 组件参数，走
  `CreateComponentEntityCommand`，创建后选中并进入撤销栈；
- 系统托管项（Camera/Terrain/Streamed Terrain/Voxel World）显示「系统托管，不能创建」；
- 天空盒弹窗说明它是启动时创建的球体实体，可「在 Inspector 中编辑」。

底部「引擎默认值」仍保留 `Save to engine.json` / `Reload engine.json` 与「提交路径
（调试）」开关；这些是本机运行参数，不进撤销栈、不让文档变脏。画质参数本身属于
场景组件，随 `.scene.json` v4 保存。

`RenderSettingsPanel::clampKernelSize()` 仍保留为纯函数并参与自检：夹到 4..64 的
偶数。自检：`[Editor] render settings selfTest PASS | kernel clamped to 4..64 even`。
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

## 体素笔刷（F3）

- 入口：工具栏「体素」开关，或 Inspector 的 `Voxel Brush` 段（`Enable (Alt+LMB)` + `Block` 下拉）。
- 操作：`Alt + 左键` 挖掉命中方块，`Alt + Shift + 左键` 在命中面的外侧放当前方块；
  射线用 `Scene::raycastVoxelWorld()`（DDA），笔刷与地形笔刷互斥（`voxelBrushConsumed`）。
- 每次改动一条 `VoxelEditCommand(before, after)`：`Ctrl+Z` 逐格撤销，重做重新挖/放。
- 没有体素世界时面板提示「创建 -> 体素世界」；`--voxel-world [r]` 启动的世界会在
  `EditorLayer::beginFrame()` 里被自动认领（打开存档同理）。
- 边界格（chunk 内 local 0/31）会连带重建相邻 chunk，笔刷不会在接缝处留下缺面。
- **修饰键分工**：`Ctrl + 左键` 永远是「加选/减选」（E1 多选），笔刷一律用 `Alt`；
  地形雕刻笔刷同样用 `Alt + 左键`。视口点中 chunk 实体时先解析到世界管理器
  （`Scene::resolveVoxelChunkOwner()`），避免选中一个随时会被重建的内部实体。

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

## 停靠后的布局调整（B4 / UX-14）

Dock 布局由 `DockBuilder` 生成一次，之后完全交给 ImGui 的 DockNode 持久化；
`drawPanels()` 不再对所有面板套 `ImGuiCond_Always` / `FirstUseEver`。窗口缩放、
最大化、拖拽分隔条时，DockNode 自己重算矩形，面板不会像旧的手写矩形那样错位。

`View -> Reset Layout` 是唯一强制重建入口：置 `layoutNeedsReset` 与
`forceDockLayoutReset`，下一帧重新拆分并 `DockBuilderFinish`。日常不要每帧重建，
否则用户拖过的比例会被覆盖。

`config/imgui.ini` 是**本机缓存**（已在 `.gitignore` 里，不进版本库）：删掉它就等于
恢复默认布局；Render 面板的尺寸/位置也由 DockNode 记录，不再依赖
`SetNextWindowSize(FirstUseEver)`。
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
  (key, ctrl, shift) 三元组不重复，打印 `[Editor] shortcut sheet selfTest PASS | entries=20 conflicts=0`。
- `?` 就是 `Shift+/`：表里存的是 `ImGuiKey_Slash + shift`，所以显示成 `Shift+Slash`，
  描述里补一句「（Shift+/ 就是 ?）」。

菜单入口（UX-06）和快捷键是两套入口、同一个实现：Edit 菜单的
`Duplicate Selected` / `Delete Selected` / `Focus Selection` / `Select All` 与快捷键
调的是同一组函数（`duplicateSelection` / `deleteSelection` / `focusSelection` /
`selectAllEntities`），不存在「菜单能点、快捷键不生效」的分叉。
