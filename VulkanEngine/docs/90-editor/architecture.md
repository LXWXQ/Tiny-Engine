---
id: editor/architecture
title: 编辑器架构与接缝
type: architecture
scope: EditorContext / EditorState / EngineServices / EditorLayer 的职责划分
read-when: 加新面板、给编辑器接新能力、或怀疑编辑器越界碰了引擎内部
size: medium
updated: 2026-10-30
---

# 编辑器架构

## 四个对象

```
EngineServices   引擎能做什么（不可变能力集合，initialize 时填一次）
EditorState      编辑器现在是什么状态（每帧在变）
EditorPanel      一个面板（纯 UI + 读状态 + 提交命令）
EditorLayer      拥有上面三者，负责菜单栏 / 布局 / 快捷键 / 播放状态
```

`EditorState` 内嵌一份 `EngineServices`，所以面板只拿 `EditorState&` 就够了，
不需要在参数里到处传引擎指针。

## 为什么用 std::function 而不是 FirstApp*

`EngineServices` 里全是回调：

```cpp
std::function<void()> saveConfig;
std::function<std::string(const char*, const char*)> openFileDialog;
```

这样编辑器**拿不到** `vkDevice`、`VkCommandPool`、`swapchain`。
它只能表达"我想保存配置"、"我想弹出打开文件对话框"，由引擎决定怎么做。

如果直接把 `FirstApp*` 递进去，编辑器的任何代码都能 `app->vulkanDevice->...`，
边界当天就会被突破，之后再也没人守得住。

## 每帧调用顺序

```
Application::update()   -> editorLayer->beginFrame(dt)   处理快捷键 / 播放状态
Application::render()   -> imguiSystem->prepareDrawData()
                              └─ frameInfo.onBuildEditorUI() -> editorLayer->drawUI()
                        -> renderer->endFrame()
                              └─ editorLayer->endFrame()
```

`drawUI()` 必须在 PresentPass 录制**之前**调用，否则面板上改的参数会晚一帧生效。

## 编辑器模式如何接管 UI

`FrameInfo` 上加了两个回调：

```cpp
std::function<void()> onBuildEditorUI;
std::function<bool()> isEditorMode;
```

`ImGuiSystem::prepareDrawData()` 检查 `isEditorMode()`：

- **false** → 画内置的 "Sentinel Engine Core" 面板（运行时模式，与改造前逐字节一致）
- **true**  → 调 `onBuildEditorUI()`，内置面板不再绘制

两套面板不并存，是为了避免它们同时修改同一批 `EngineSettings` 造成控制权歧义。

## 脏标记

`EditorState::markDirty()` 只置位 `documentDirty`。**不**自动写盘 ——
保存必须由用户显式触发（Ctrl+S / File 菜单）。自动保存会让人无法丢弃实验性改动。

## 加一个新面板

1. 继承 `EditorPanel`，实现 `name()` 与 `draw(EditorState&)`
2. 在 `EditorLayer::createDefaultPanels()` 里 `registerPanel<YourPanel>()`
3. 在 `EditorLayer::drawPanels()` 的布局分支里给默认矩形（可选）
4. 在 View 菜单加快捷开关（可选）
