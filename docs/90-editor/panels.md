---
id: editor/panels
title: 面板与布局
type: system
scope: 现有面板、布局实现方式、以及没有 docking 时的注意事项
read-when: 写新面板、改布局、或面板位置错乱
size: ~125 行
updated: 2026-11-01
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

## 硬约束

- ImGui 可见文字**必须是 ASCII**：内置字体没有中文字形，会渲染成 `???`
- 面板不得自己创建 GPU 资源
- 面板不得直接改 registry，必须走命令栈
