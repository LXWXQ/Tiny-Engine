---
id: systems/imgui
title: ImGui 面板与后端
type: system
scope: src/Core/ImGuiSystem.h, src/Core/ImGuiSystem.cpp
read-when: 修改调试面板、ImGui 初始化、或遇到 UI 文字/字体问题
size: ~110 行
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

ImGui 在 `PresentPass` 的**同一个 `vkCmdBeginRendering` 作用域**内绘制，
因此 UI 永远在 tonemap 之后、不会被后效糊掉。

viewport/scissor 由 ImGui 后端自己设置，PresentPass 里不用补。
ImGui 后端在 swapchain 重建时要重新初始化。

## 3. 文字必须 ASCII

**ImGui 内置字体只有 ASCII 字形**，中文会整条渲染成 `???`。
因此：

- 面板标签、按钮、状态字符串一律英文；
- `EngineConfig::getLastStatus()` 返回英文；
- 日志（`std::cout`）可以是中文，因为不进 ImGui。

## 4. 面板内容

当前面板可调：Bloom（开关/强度/阈值/膝点）、SSAO（开关/半径/偏置/kernel）、
光照（方向/颜色/曝光）、renderScale、窗口标题，以及配置的 Save / Reload。

改参数后当帧生效——因为 `FrameInfo::settings` 是**引用**，指向 `Scene` 里的同一份对象
（见 [decisions](../10-architecture/decisions.md) 决策 15）。

## 5. 已知不一致

`ImGuiSystem` 硬编码 `init_info.MinImageCount = 2`，而 `Swapchain` 用
`minImageCount + 1`。ImGui 只在 `ImageCount < MinImageCount` 时报错，所以当前不触发。
见 [weak-points](../10-architecture/weak-points.md) #8。
