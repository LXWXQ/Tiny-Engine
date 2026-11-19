---
id: debug-profiling/diagnostics
title: 诊断基础设施实现
type: debug
scope: src/Core/Diagnostics.h, src/Core/Diagnostics.cpp, src/RHI/GpuTimer.*, src/RHI/ScreenshotCapture.*, src/RHI/RenderDocHook.*, src/RHI/Breadcrumbs.*, src/RHI/PipelineStats.*, src/RHI/RenderGraphAlias.*
read-when: 修改验证层回调、GPU 计时、截图、抓帧、排查 device lost，或看管线统计与别名报告
size: ~190 行
updated: 2026-11-19
---

# 诊断基础设施实现

## 1. 验证层结构化（P0-1）

### 1.1 为什么改

原本 `Device` 的 `debugCallback` 直接写 `std::cerr`，与"stderr 0 字节"的验收基线
天然冲突——只要验证层报一条 info，基线就假失败。

### 1.2 现在

```
Device::debugCallback
   -> diag::recordValidationMessage(severity, messageId, message)
        -> 原子计数 error/warning
        -> 可选写 JSONL（惰性打开，每条 flush）
退出时 diag::printValidationSummary() -> "[Validation] errors=N warnings=M"
```

- **绝不写 stderr**。
- JSONL 格式：`{"severity":"ERROR","id":"VUID-...","msg":"..."}`。
- `--validation-jsonl <path>` 空则完全不落盘（不创建文件）。

### 1.3 验证层特性链

`--validation-features` 的值通过 `VkValidationFeaturesEXT` 挂到
`VkInstanceCreateInfo::pNext`。**必须真的传到 `Device`**：

> 历史 bug：曾因 `std::make_unique<Device>(window)` 没传 options 而**静默失效**——
> 加了 flag 却没有任何效果。现在用 `--build-info` 和
> `[Diag] validation features enabled:` 双重确认。

## 2. GPU 计时（P0-4）

- 每个 Pass 前后 `vkCmdWriteTimestamp2`（需要 `synchronization2` + `VK_EXT_calibrated_timestamps`
  给的 `timestampPeriod`）。
- 按 frame slot 各一份 query pool（避免跨帧覆盖）。
- 退出写 `gpu_timing.csv`：`frame,pass,gpu_ms`。
- **关闭时**：不创建 query pool、不插 timestamp。
- 守卫计数：`[Diag] zero-overhead guard ... queryPool=N`。

剔除预热帧很重要：冷启动 GPU 升频，头几十帧明显偏慢。

## 3. 无头截图（P1-4）

- 目标帧把交换链图像读回（`vkCmdCopyImageToBuffer`）并写 PNG（用已有 `stb_image_write`）。
- 该模式下交换链额外带 `TRANSFER_SRC` usage。
- 默认目标帧：selftest 时取"总帧数的约一半且不超过 60"；交互模式取第 60 帧。
- **关闭时**：不建 readback buffer（守卫计数 `screenshotBuffer=0`）。

## 4. RenderDoc in-app 抓帧（P1-2）

- 编译期 `TINY_ENABLE_RENDERDOC`，默认 OFF。
- 开启后在指定帧调 `TriggerCapture()`。
- **未注入 RenderDoc 时是空操作**，输出 `[Diag] RenderDoc not injected`。
- 关闭时连 `RENDERDOC_GetAPI` 都不调用。

## 5. Tracy（P1-1）

- `src/Core/Profiler.h` 提供 `TINY_ZONE_SCOPED(name)` / `TINY_PROFILE_FRAME_MARK()`。
- 关闭时展开为 `((void)0)`。
- `src/RHI/GpuProfiler.*` 提供每 pass 的 GPU zone，已集成进 RenderGraph。
- 默认 OFF；ON 路径已用 `Validate.ps1 -EnableProfiling` 验证过。

## 6. 能力自报（P0-6）

`diag::printBuildInfo(options)` 输出三类信息：

```
[BuildInfo] compiled: gpuTiming=1 validationFeatures=1 buildInfo=1 screenshot=1 tracy=0 renderdoc=0
[BuildInfo] runtime : gpuTiming=0 validationBestPractices=0 validationSync=0 ...
[BuildInfo] policy: debug capabilities are off by default; when off they allocate nothing and record no GPU commands
```

**为什么需要**：杜绝"以为开了其实没编译进去"。

## 7. 诊断自检（不需要 Vulkan）

```powershell
.\\build\\Debug\\TinyEngine.exe --diagnostics-test out.jsonl
# [Diag] diagnostics selfTest PASS | errors=1 warnings=1 jsonl=written
```

写入合成消息，校验计数与 JSONL 落盘。这样在没有真实验证层消息时，
"结构化输出 + 落盘"这条链路也有回归覆盖。

## 8. device fault（P2-4）

- 设备支持 `VK_EXT_device_fault` 时启用，`Renderer` 在 `VK_ERROR_DEVICE_LOST`
  时调 `vkGetDeviceFaultInfoEXT` 输出 `[DeviceFault]`。
- **限制**：该查询**只能在设备已 lost 时调用**
  （`VUID-vkGetDeviceFaultInfoEXT-device-07336`）。所以健康设备上只能校验
  "扩展已启用 + 入口点已解析"，真实故障路径靠 Renderer 的 device-lost 分支覆盖。

## 9. GPU breadcrumbs（P2-5）

`VK_EXT_device_fault` 只回答"哪条指令踩了哪块内存"，回答不了"崩在第几帧的哪个
Pass"。`Breadcrumbs`（`src/RHI/Breadcrumbs.*, src/RHI/PipelineStats.*`）补的就是这一层：每个 Pass 前后往
一个 4 字节游标写序号，CPU 侧留一张"序号 -> 帧号/Pass 名"的表，device lost 之后
读游标查表。核心洞察是**真相在 CPU 侧，GPU 只带一个游标**。

三种落地方式，按可用性自动降级：

1. `VK_NV_device_diagnostic_checkpoints`：`vkCmdSetCheckpointNV` 写
   `(slot << 32) | counter`，崩溃后 `vkGetQueueCheckpointData2NV` 逐队列取回。
   硬件缓冲，无命令开销，渲染通道内外都能用。取回顺序不保证，但 counter 单调，
   所以"每槽取最大"就是最后执行到的那条。
2. `VK_AMD_buffer_marker`：`vkCmdWriteBufferMarkerAMD(TOP_OF_PIPE, ...)` 写游标缓冲。
   用 `TOP_OF_PIPE` 是刻意的 —— 语义是"命令流到了这里"，不是"这个 Pass 做完了"。
3. 都没有时退化成 `vkCmdUpdateBuffer`：它在渲染通道内非法，所以只能按 Pass 粒度。

- 槽位 = 帧槽 × {graphics, compute}，共 4 个（`Breadcrumbs::kSlotCount`），
  两帧在飞各有独立游标，不会互相覆盖。
- 游标缓冲是 **HOST_VISIBLE 常驻映射**的。device lost 之后不能再提交任何命令，
  只能直接读 CPU 映射的那块内存 —— 这是设计前提，不是优化。
- 触发点：`Renderer::endFrame` 三处 `VK_ERROR_DEVICE_LOST` 分支调
  `Device::reportDeviceFault(queue)`，函数**第一件事**就是跑 breadcrumbs，
  所以没有 `VK_EXT_device_fault` 的机器上也能拿到"崩在哪个 Pass"。
- 输出：`[Breadcrumbs] 崩溃点 slot=N frame=M "PassName"` + 之前 8 条 trail，
  并走 `tiny::writeStartupErrorLog` 落盘（`main.cpp` 的错误对话框会指向该文件）。
- 纯逻辑自检：`Breadcrumbs::selfTest()`（槽位映射、标记编解码往返、名字截断），
  挂在 `runHeadlessSelfTest` 里，不需要 GPU 也能跑。
- **坑**：`Breadcrumbs::shutdown()` 必须在析构里调。`Device` 只是
  `breadcrumbs.reset()`，隐式析构不会调用它，游标缓冲会活到
  `vmaDestroyAllocator` 之后，触发 VMA 的 `Some allocations were not freed` 断言
  （见 [troubleshooting §3.2](../40-build/troubleshooting.md)）。

## 10. 管线统计（TM-06）

`--stats-pipelines` 在退出时打印本次运行的管线账本：

- 图形 / compute 各建了几条，shader 模块几个、读了多少 MB SPIR-V；
- 建管线总耗时与均值（计时覆盖"读 SPIR-V + 建模块 + 建管线"，真正慢的是驱动编译）；
- **状态种类**与**完全相同的管线**是两个数：前者是 `GraphicsStateKey` 的去重结果，
  后者还要两个 shader 路径也一致。状态相同但 shader 不同完全正常（IBL 的几条只有
  片元不同），混成一个数会得出"重复很多"的错误结论。
- `GraphicsStateKey.pipelineLayout` 必须参与哈希：不同布局的管线不能互换，漏算就会
  把真正不同的管线判成重复。

本机实测（RTX 2070，`--selftest 30`）：图形 30 条 / compute 8 条 / 68 个模块 /
0.41 MB SPIR-V；冷启动无缓存 total=50.5ms，热缓存 total=24.7ms（平均 0.65ms/条）；
30 条图形管线里状态只有 26 种，完全相同的管线 0 条。

结论：桌面有驱动磁盘缓存时，启动期建管线不过几十毫秒，**没有值得做的按需建**；
这项的价值在 Android / 冷启动 / 管线爆炸（成百上千条）的场景。零开销路径同样验证过：
不带 `--stats-pipelines` 时只累加整数，不建表，输出与基线逐字节一致。

## 11. 资源别名报告（TM-08）

`--alias-report` 在退出时打印本帧中间目标的显存账本。它只读：不改任何分配、不绑定
`VK_IMAGE_CREATE_ALIAS_BIT`，目的就是先让"能不能省"这个问题的答案变成数字。

- **生命周期**：每个纹理记两个 Pass 序号 —— `firstWrite`（第一次被打上写访问的那个 Pass）
  与 `lastUse`（最后一次读或写）。序号取 `lastFrameExecutedPasses.size() - 1`，所以被
  `setEnabled(false)` 跳过的 Pass 不占号，与调试面板看到的顺序一致。
- **装箱**：按 `firstWrite` 升序线性扫描，`best fit` 放进"已经空出来"的块里；合并前提是
  格式 / 尺寸 / mip / 层数全同（比 Vulkan 允许的更保守，松一档就要顺带验证 image view 兼容性）。
- **三类目标分开报**：渲染图自己持有的（可谈别名）、每帧重建但导入的（显存不归渲染图管）、
  跨帧常驻的（本就不该参与）。混在一起报会给出误导性的节省数字。

本机实测（RTX 2070，1920x1080，`--selftest 30 --alias-report`）：

```
[Alias] 一帧 8 个瞬时目标，合计 40.8 MiB
[Alias] 线性扫描后 8 块（最大 15.8 MiB），可省 0（0%）
[Alias]   SSAO.Raw.Frame1 [Pass 6..7] 2.0 MiB
[Alias]   SSAO.Frame1 [Pass 7..8] 2.0 MiB
[Alias]   HDR.Scene.Frame1 [Pass 8..11] 15.8 MiB
[Alias]   TAA.Output.Frame1 [Pass 11..19] 15.8 MiB
[Alias]   Bloom.Frame1.Level0 [Pass 12..19] 4.0 MiB
[Alias] 另有 13 个每帧重建的导入目标（合计 181.9 MiB）：它们才是有价值的目标，但显存不归渲染图管
[Alias] 常驻导入目标 4 个（合计 229.4 MiB，跨帧复用同一块，不该参与别名）
```

为什么是 0：TAA 输出覆盖了整条 bloom 链（`[11..19]` 对 `[12..19]`），HDR 与 TAA 在 Pass 11
上首尾相接（同一 Pass 读 HDR、写 TAA），SSAO 的两张图在 Pass 7 上重叠。三个方向都被堵死，
所以本结构下别名化省不出显存 —— 详细取舍见 [decisions](../10-architecture/decisions.md) 决策 27。

零开销路径：不带 `--alias-report` 时 `RenderGraph::endFrame` 里那一段整个不执行（连 vector
都不建），`--selftest 30` 的输出与基线一致，`[Diag] zero-overhead guard PASS` 仍然通过。
