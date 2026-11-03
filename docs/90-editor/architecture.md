---
id: editor/architecture
title: 编辑器架构与接缝
type: architecture
scope: EditorContext / EditorState / EngineServices / EditorLayer 的职责划分
read-when: 加新面板、给编辑器接新能力、或怀疑编辑器越界碰了引擎内部
size: ~80 行
updated: 2026-11-03
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

- **false** → 画内置的 "Tiny Engine Core" 面板（运行时模式，与改造前逐字节一致）
- **true**  → 调 `onBuildEditorUI()`，内置面板不再绘制

两套面板不并存，是为了避免它们同时修改同一批 `EngineSettings` 造成控制权歧义。

## 播放期间文档只读（E3b）

播放是运行期状态，不是文档内容 —— 所以 `Playing` / `Paused` 期间 `EditorState::documentWritable()` 返回 false：

- `saveScene()` 拒绝写盘，日志 `[Editor] save blocked: 播放中不写入文档（按 F7 停止后保存）`；否则 F5 跑出来的位姿会被 Ctrl+S 存进 `.scene.json`；
- `newScene()` / `loadSceneFromPath()` 同样拒绝，否则会替换掉 `playSnapshot`，Stop 时把用户恢复到别人家的场景；
- File 菜单里 New / Open / Save / Save As 在播放期间置灰，但快捷键仍会打一条 Warning 日志 —— 显式提示，不静默失败；
- `playSnapshot` 只在内存里，`stopPlay()` 后立即清空，不参与任何序列化路径。

播放状态切换本身（F5 / F6 / F7）**不进撤销栈**：它不是"文档被修改"，塞进栈里会让运行期的 Ctrl+Z 语义变得不可预测；播放前的改动已经在快照里。

## 未保存修改的确认（E4）

所有"会替换文档"的入口先走 `requestDocumentAction()`：

- 文档干净，或正在播放（E3b 的只读语义）→ 直接执行；
- 有未保存修改 → 只把意图记进 `state.pendingDocumentAction`，下一帧由 `drawUnsavedPrompt()` 弹出模态框；
- 三个按钮：保存并继续（保存失败就停在框里，不吞掉意图）/ 放弃修改 / 取消；
- 点窗口关闭按钮也会被主循环拦截，转成同一条确认流程（`EditorLayer::requestExitConfirmation()`）；
- 菜单与快捷键共用这一个路口，不会再各写一套判断。

窗口标题由编辑器算好再请引擎设置：`EngineServices::setWindowTitle` / `requestExit` 是两个回调，编辑器仍然拿不到 GLFW 窗口。
## 脏标记

`EditorState::markDirty()` 只置位 `documentDirty`。**不**自动写盘 ——
保存必须由用户显式触发（Ctrl+S / File 菜单）。自动保存会让人无法丢弃实验性改动。

## 加一个新面板

1. 继承 `EditorPanel`，实现 `name()` 与 `draw(EditorState&)`
2. 在 `EditorLayer::createDefaultPanels()` 里 `registerPanel<YourPanel>()`
3. 在 `EditorLayer::drawPanels()` 的布局分支里给默认矩形（可选）
4. 在 View 菜单加快捷开关（可选）
