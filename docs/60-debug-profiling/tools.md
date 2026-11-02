---
id: debug-profiling/tools
title: 工具速查
type: debug
scope: tools/
read-when: 排查性能/画质问题，或要找"该用哪个工具"
size: ~170 行
updated: 2026-10-30
---

# 工具速查

## 1. 自研工具（默认可用，厂商中立）

| 工具 | 命令 | 产出 |
|---|---|---|
| 主回归 | `tools/Validate.ps1` | `build/validation/<ts>/`（构建日志 + 每项测试的 stdout/stderr） |
| 轻量剖析包 | `tools/Profile.ps1` | `build/profiles/<ts>/`（`env.txt` + `gpu_timing.csv` + 日志 + `manifest.json`） |
| 一键证据包 | `tools/DebugBundle.ps1` | `build/bundles/<ts>/`（manifest + summary + run + validation + deep + baseline_diff） |
| 深度校验 | `tools/DeepValidation.ps1` | `summary.json`（sync / bp / gpuav / printf 各自结果） |
| 基线比对 | `tools/Baseline.ps1 -Action Save/Diff` | `build/baseline/<name>/`（GPU 时间 / 验证层 / 视觉三维度） |
| 截图 + 金标准 diff | `tools/Screenshot.ps1` | PNG + 差异分数 |
| RenderDoc CLI 抓帧 | `tools/Capture.ps1` | `.rdc` |
| 依赖安装 | `tools/SetupDependencies.ps1` | `3rdParty/` |
| 剖析依赖安装 | `tools/SetupProfiling.ps1` | `3rdParty/tracy`、`3rdParty/renderdoc/renderdoc_app.h` |
| 文档校验 | `tools/DocCheck.ps1` | 文档系统一致性报告 |

## 2. 引擎内置能力

| 能力 | 开关 | 产出 |
|---|---|---|
| 能力清单 | `--build-info` | 编译进的 & 本次开启的能力 |
| 验证层结构化 | Debug 默认 | 退出打印 `[Validation] errors=N warnings=M` |
| 验证层增强 | `--validation-features bp,sync,printf,gpuav` | 分类消息 |
| 验证层落盘 | `--validation-jsonl <path>` | JSONL（每行 `{severity,id,msg}`） |
| 每 pass GPU 计时 | `--gpu-timing [--gpu-timing-csv P]` | CSV（`frame,pass,gpu_ms`） |
| 无头截图 | `--screenshot P [--screenshot-frame N]` | PNG |
| 诊断自检 | `--diagnostics-test [P]` | 计数 + JSONL，不依赖 Vulkan |
| 设备故障 | `--device-fault-test` | `[DeviceFault]` 能力就绪 |

## 3. 外部工具（可选，厂商中立优先）

| 工具 | 用途 | 获取 |
|---|---|---|
| RenderDoc | 帧捕获 + 回放（主力） | 独立安装；`TINY_ENABLE_RENDERDOC=ON` 后可用 `--capture-frame N` |
| Tracy | CPU/GPU 时间线 | `tools/SetupProfiling.ps1` + `TINY_ENABLE_TRACY=ON` |
| SPIRV-Tools | `spirv-val` 已接入构建 | 随 Vulkan SDK |
| vulkaninfo | 环境快照 | 随 Vulkan SDK |

厂商专用（**不进默认回归**，换卡会失效）：`nsys` / `ncu`（NVIDIA）、
RGP/RGD/RGA（AMD）、GPA（Intel）。需要时自行安装，只做人工深挖。

## 4. 问题 -> 工具 决策表

| 症状 | 首选 | 次选 |
|---|---|---|
| 花屏 / 随机闪烁 | `--validation-features sync` | RenderDoc 看 layout/绑定 |
| 黑屏 / 某 Pass 无输出 | 验证层 + `--validation-jsonl` | RenderDoc event 浏览 |
| 崩溃 / device lost | `--device-fault-test` + 验证层 | RenderDoc 最后一帧 |
| 着色器越界 / 读到垃圾 | `--validation-features gpuav` | RenderDoc 资源查看 |
| 着色器数值不对 | `--validation-features printf` | RenderDoc 像素历史 |
| 帧率低于预期 | `--gpu-timing` | Tracy GPU zone |
| 怀疑 CPU 瓶颈 | Tracy CPU zone | `nsys` |
| GPU 空转 / 队列串行 | `nsys` 时间线 | Tracy |
| pipeline / descriptor 开销 | `--validation-features bp` | `nsys` API 统计 |
| 视觉回归 | `Baseline.ps1 -Action Diff` | RenderDoc |
| 内存泄漏 | `-DTINY_VMA_LEAK_CHECK=ON` | `VK_EXT_memory_budget` |

## 5. 注意事项

1. **`tools/*.ps1` 必须纯 ASCII 无 BOM**（PowerShell 5.1 按 ANSI 读无 BOM 脚本）。
2. **路径含空格时参数必须过 `Quote-NativeArg`**（见
   [pitfalls](pitfalls.md) 坑 19）。
3. **GPU 计时必须剔除预热帧**（`Baseline.ps1` 默认丢前 30 帧），否则基线系统性偏慢。
4. **`tracy-csvexport` 默认只导 CPU zone**，GPU zone 要加 `-g`。
5. Tracy / RenderDoc 缺失时 CMake 只 warning 并自动禁用，`--build-info` 是最终事实。
