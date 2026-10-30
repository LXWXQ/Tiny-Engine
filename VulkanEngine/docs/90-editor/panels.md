---
id: editor/panels
title: 面板与布局
type: system
scope: 现有面板、布局实现方式、以及没有 docking 时的注意事项
read-when: 写新面板、改布局、或面板位置错乱
size: small
updated: 2026-10-30
---

# 面板与布局

## 现有面板

| 面板 | 职责 |
|---|---|
| Hierarchy | 实体树，选中 / 拖拽重挂 / 右键删除 |
| Inspector | 选中实体的组件编辑（变换走命令栈） |
| Console | 分级过滤的日志，自动滚动 |
| Assets | 扫描 `Resources/Models`，双击创建实体（走命令栈，可撤销） |
| Viewport | 把离屏纹理当 ImGui 图片画出来（见 [viewport](viewport.md)） |

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

`io.IniFilename = SENTINEL_IMGUI_INI_PATH` → `config/imgui.ini`。

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
