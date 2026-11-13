---
id: systems/imgui
title: ImGui 面板与后端
type: system
scope: src/Core/ImGuiSystem.h, src/Core/ImGuiSystem.cpp
read-when: 修改调试面板、ImGui 初始化、或遇到 UI 文字/字体问题
size: ~80 行
updated: 2026-11-13
---

# ImGui 面板与后端

## 1. 后端初始化（1.92.6-docking）

项目使用 `v1.92.6-docking`，在 `ImGuiSystem` 中启用 `ImGuiConfigFlags_DockingEnable`，但不启用 `ImGuiConfigFlags_ViewportsEnable`（避免多原生窗口与 Vulkan 交换链耦合）。动态渲染下字段位置与老教程不同：

```cpp
init_info.UseDynamicRendering = true;
init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat;
```

两个坑：

1. 字段在 **`PipelineInfoMain` 下面**，不在顶层。网上大量示例写顶层
   `init_info.PipelineRenderingCreateInfo`——那是老版本，这里编译不过。
2. `pColorAttachmentFormats` 指向的 `swapchainFormat` 必须是**活到 shutdown 的成员**
   （`ImGuiSystem::swapchainFormat`），不能是临时变量。

## 2. 渲染位置

ImGui 有两条绘制路径，不能混用同一套假设：

- **非编辑器**：`ImGuiSystem` 在 `PresentPass` 的同一个 `vkCmdBeginRendering` 作用域内绘制，
  UI 在 tonemap 之后提交，不会被后处理糊掉。
- **编辑器**：`PresentPass` 只把场景写进离屏 `editorViewport`；`UiCompositePass` 采样这张纹理，
  再把 ImGui draw data 提交到交换链。两个 pass 都持有 `ImGuiSystem*`，交换链重建时必须
  同时更新 `presentPassPtr` 与 `uiCompositePassPtr`，漏一个就是悬空指针。

viewport/scissor 由 ImGui 后端自己设置，pass 里不用补。交换链重建的纹理重注册顺序见
[viewport](../90-editor/viewport.md) 的「交换链重建」小节。

## 3. 文字必须 ASCII

**ImGui 内置字体只有 ASCII 字形**，中文会整条渲染成 `???`。
因此：

- 面板标签、按钮、状态字符串一律英文；
- `EngineConfig::getLastStatus()` 返回英文；
- 日志（`std::cout`）可以是中文，因为不进 ImGui。

## 4. 编辑器面板与交换链重建

编辑器不使用旧的 `ImGuiSystem` 调参面板；面板由 `EditorLayer` 的 `drawPanels()` 管理：

- 全屏 `##DockHost` + `TinyEditorDockSpace`，默认布局由 `DockBuilder` 拆分；
  左 Hierarchy/Assets，中 Viewport，右 Inspector/Render（同节点 tab），右下 History，底 Console。
  详见 [panels](../90-editor/panels.md)。
- `Render` 是组件目录（12 单一性 + 15 对象 + 1 系统天空盒），场景组件参数在 Inspector 编辑；
  底部「引擎默认值」仍保留 `Save/Reload engine.json` 与提交路径调试开关。
- 工具栏/状态栏固定高度，DockHost 扣掉 `kToolbarHeight`/`kStatusBarHeight`；`config/imgui.ini`
  持久化 DockNode。

交换链重建顺序是硬约束（`FirstApp::recreateSwapchain()`）：

1. `vkDeviceWaitIdle()` 后、销毁旧 `ImGuiSystem` **之前**，先调用
   `removeEditorViewportImGuiTextures()`，让旧 descriptor pool 还活着时注销视口纹理；
2. 用新尺寸重建 swapchain / render targets，再 `imguiSystem.reset()` + 新建 `ImGuiSystem`；
3. 同时 `presentPassPtr->setImGuiSystem(newPtr)` 与 `uiCompositePassPtr->setImGuiSystem(newPtr)`，
   再用新 pool 调 `createEditorViewportTextures()` 重新注册每个 frame slot 的纹理。

漏掉第 1 步会把旧 pool 的 `VkDescriptorSet` 传给新后端，随后
`ImGui_ImplVulkan_RemoveTexture()` 对不属于新 pool 的 set 调 `vkFreeDescriptorSets`，
validation layer 会在 `VkLayer_khronos_validation.dll` 以 `0xC0000005` 崩溃；漏掉第 3 步则是
`UiCompositePass` 持有已析构的 `ImGuiSystem*`。

## 5. 已知不一致

`ImGuiSystem` 硬编码 `init_info.MinImageCount = 2`，而 `Swapchain` 用
`minImageCount + 1`。ImGui 只在 `ImageCount < MinImageCount` 时报错，所以当前不触发。
见 [weak-points](../10-architecture/weak-points.md) #8。
