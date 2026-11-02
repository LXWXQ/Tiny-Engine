---
id: architecture/synchronization
title: 同步模型：多帧、双队列、present
type: architecture
scope: src/RHI/Renderer.cpp, src/RHI/Swapchain.cpp, src/Core/FrameInfo.h, src/Core/FrameInFlight.h
read-when: 修改帧循环、队列提交、交换链或 resize 逻辑
size: ~250 行
updated: 2026-10-30
---

# 同步模型

## 1. 三个下标别搞混

`FrameInfo` 有三个容易混的字段：

| 字段 | 含义 | 用途 |
|---|---|---|
| `frameInFlightIndex` | CPU/GPU 资源槽位（0 或 1） | 索引每槽一份的 UBO / 图 / 描述符集 |
| `swapchainImageIndex` | 本次 acquire 到的交换链图像 | 决定渲染到哪张图、present 用哪个 semaphore |
| `frameNumber` | 从启动开始的逻辑帧号 | 日志、截图目标帧、自检帧计数 |

> 历史坑：`frameIndex` 这个名字曾有歧义（它装的其实是交换链图像索引）。现已拆成上面三个。

## 2. 2 帧在飞

```
CPU:  录制 N     录制 N+1
GPU:        执行 N        执行 N+1
```

每槽独立持有：graphics/compute 命令缓冲、timeline semaphore、UBO、瞬时图、描述符集。

> 为什么是 2：足以让 CPU 录制第 N+1 帧时 GPU 还在跑第 N 帧；
> 再加会线性增加所有瞬时目标的显存。等有 GPU 计时数据再决定是否升到 3。

## 3. timeline semaphore

不用 `vkWaitForFences` 串行等待，而是每条队列一个 timeline：

```cpp
// 复用槽位前，等"该槽上一次提交"完成
vkWaitSemaphoresKHR(device, &waitInfo, UINT64_MAX);
// 提交时给 timeline 一个更大的值
VkTimelineSemaphoreSubmitInfo timelineInfo{};
timelineInfo.pSignalSemaphoreValues = &newValue;
```

好处：可以精确等待"某一帧"而不是"全部完成"，材质延迟回收（TD-04）就是靠查询
已完成 timeline 值实现的。

## 4. graphics / compute 双队列

`Device` 选队列的方式：优先独立 compute family，没有就回退 graphics family。
当前机器 `graphics=0 present=2 compute=2`（独立异步计算队列）。

每个 frame slot：

```
graphics command buffer + graphics timeline
compute  command buffer + compute  timeline
```

### 4.1 跨队列调度（TD-02）

`RenderGraph` 推导出 `CrossQueueDependencies`（见
[rendergraph](rendergraph.md) §6），`Renderer::endFrame()` 据此决定：

| 情况 | submit 顺序 |
|---|---|
| compute 写、graphics 读 | 先 submit compute，graphics 侧 timeline wait |
| graphics 写、compute 读 | 先 submit graphics，compute 侧 timeline wait |
| 同帧出现两个方向 | **明确报错**（不做静默降级） |
| 无交叉 | 两条队列并行提交，互不等待 |

资源侧走 `VK_SHARING_MODE_CONCURRENT`（`crossQueue=true`），未实现 EXCLUSIVE
ownership transfer。

## 5. 交换链

### 5.1 重建触发

acquire/present 返回 `VK_ERROR_OUT_OF_DATE_KHR` 或 `VK_SUBOPTIMAL_KHR` 时置重建请求，
应用在**帧边界**（等 GPU 完成）后原地重建：

- swapchain 本身（`imageCount = minImageCount + 1`）
- `renderFinishedSemaphores`（数量随图像数变化）
- ImGui 后端
- 所有依赖尺寸的中间目标与描述符视图

### 5.2 动态分辨率

`renderScale`（0.25~1.0）只缩放**中间目标**（G-Buffer / SSAO / HDR / Bloom），
交换链尺寸不变。因此：

- 不重建管线（动态 viewport/scissor）；
- 不重建交换链；
- 需要重建上述中间目标及其描述符视图。

### 5.3 已知不一致

`Swapchain` 取 `minImageCount + 1`，而 `ImGuiSystem` 硬编码
`init_info.MinImageCount = 2`。ImGui 只在 `ImageCount < MinImageCount` 时报错，
所以当前没触发。见 [tech-debt](../70-status/tech-debt.md)。

## 6. present 转换

`RenderGraph::presentTransition()` 在最后为交换链图像发一个布局转换到
`PRESENT_SRC_KHR`。它被实现成一个**没有颜色/深度附件**的 graphics pass——
渲染图看到无附件就只发屏障，不 `vkCmdBeginRendering`。

为什么要显式声明而不是由渲染图自动处理：渲染图放弃了对"帧"的完整知识，
这样离屏渲染 / 多窗口 / 截图不需要改渲染图。

## 7. 调试同步问题

```powershell
# 同步校验（最强工具）
.\\build\\Debug\\TinyEngine.exe --selftest 30 --validation-features sync

# 期望：0 errors / 0 warnings
```

若出现 hazard，优先查：

1. 某个 Pass 漏了 `read`/`writeColor` 声明（漏声明 = 漏同步）；
2. `loadOp == LOAD` 但只声明了写访问位；
3. 交换链等待阶段不是 `ALL_COMMANDS`。

详见 [pitfalls](../60-debug-profiling/pitfalls.md) 与 [diagnostics](../60-debug-profiling/diagnostics.md)。
