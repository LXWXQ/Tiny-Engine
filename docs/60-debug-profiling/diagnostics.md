---
id: debug-profiling/diagnostics
title: 诊断基础设施实现
type: debug
scope: src/Core/Diagnostics.h, src/Core/Diagnostics.cpp, src/RHI/GpuTimer.*, src/RHI/ScreenshotCapture.*, src/RHI/RenderDocHook.*
read-when: 修改验证层回调、GPU 计时、截图或抓帧
size: ~200 行
updated: 2026-10-30
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

- 编译期 `SENTINEL_ENABLE_RENDERDOC`，默认 OFF。
- 开启后在指定帧调 `TriggerCapture()`。
- **未注入 RenderDoc 时是空操作**，输出 `[Diag] RenderDoc not injected`。
- 关闭时连 `RENDERDOC_GetAPI` 都不调用。

## 5. Tracy（P1-1）

- `src/Core/Profiler.h` 提供 `SENTINEL_ZONE_SCOPED(name)` / `SENTINEL_PROFILE_FRAME_MARK()`。
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
.\\build\\Debug\\VulkanEngine.exe --diagnostics-test out.jsonl
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
