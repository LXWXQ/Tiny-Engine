---
id: debug-profiling/zero-overhead
title: 零开销开关模型
type: debug
scope: CMakeLists.txt 的 SENTINEL_ENABLE_*, src/Core/Profiler.h, src/Core/Diagnostics.h
read-when: 新增任何调试/剖析能力之前
size: ~150 行
updated: 2026-10-30
---

# 零开销开关模型

## 1. 两层正交开关

| 层 | 由谁控制 | 关闭时成本 | 适用 |
|---|---|---|---|
| **编译期** | CMake option -> `SENTINEL_ENABLE_*` 宏 | **完全为 0**（代码不参与编译） | 重量级/带外部依赖：Tracy、RenderDoc、Debug Printf、GPU-AV |
| **运行期** | CLI flag（默认关） | 每帧每 pass 一个 hoisted `if`；热点内层零判断 | 轻量自研：`--gpu-timing`、`--screenshot` |

## 2. 当前开关表

| 能力 | 编译期默认 | 运行期默认 | 关闭时成本 |
|---|---|---|---|
| 验证层 | Debug 开 / Release 关（`NDEBUG`） | — | Release 为 0 |
| Best Practices / syncval / GPU-AV / Debug Printf | 可编译进 | 关 | 0（未启用即不创建） |
| `--gpu-timing` | 编译进（ON） | 关 | 0 分配、0 命令 + 一处 hoisted 分支 |
| `--screenshot` | 编译进（ON） | 关 | 0（不建 readback buffer） |
| `--build-info` | 编译进（ON） | 关 | 0 |
| Tracy | **默认 OFF** | 由 `TRACY_ENABLE` 决定 | 宏展开为 `(void)0` |
| RenderDoc | **默认 OFF** | `--capture-frame` | 0（连 `RENDERDOC_GetAPI` 都不调） |

## 3. 写代码时的 9 条规则

1. **默认关**：开关一律正向命名（`--gpu-timing`），不用 `--no-...`。
2. **关闭时不分配**：`if (!enabled) return;` 必须在**创建 query pool / buffer / descriptor / 线程之前**。
   禁止"先建好、只是不读取"。
3. **关闭时不插命令**：timestamp、Tracy zone、debug label 一律不写入 command buffer。
4. **关闭时不改生命周期**：不得因"关着"而少建/多建任何同步对象或资源。
   VMA 统计与帧时序必须与未接入时一致。
5. **热路径零分支**：开关判断只放在**帧边界 / 每 pass 一次**的层级，
   禁止进入 per-draw / per-object 内层循环。
6. **C++17 约束**：不能用 `[[likely]]`。靠"判断提到循环外 + 分支预测"。
7. **Release 更严**：`NDEBUG` 下验证层、Debug Printf、GPU-AV 一律不编译/不启用。
8. **可自证**：`--build-info` 打印**编译进了什么**与**本次开启了什么**，
   杜绝"以为开了其实没编译进去"。
9. **外部工具只作用于单次**：nsys / RenderDoc 经 `tools/*.ps1` 单次注入，不做全局常驻 layer。

## 4. 守卫

每次运行结束打印：

```
[Diag] zero-overhead guard PASS | queryPool=0 | screenshotBuffer=0 | validationJsonl=off
```

- 默认（无 flag）必须是 `PASS | queryPool=0 | screenshotBuffer=0`。
- 开启任一能力时变成 `SKIP`（有分配是预期的）。

## 5. 判断标准

不追求"绝对零指令"，而采用判据：

> **允许每帧每 pass 一处 hoisted 判断与少量常驻元数据，禁止任何可测量的渲染性能回退。**

实测参考（同机 Debug）：compute probe 常开占约 6.5% FPS，Bloom 关闭路径省约 17%。
所以像 probe 这类每帧固定成本的东西**必须默认关**。

## 6. 加新能力的流程

1. `CMakeLists.txt` 加 `option(SENTINEL_ENABLE_X ...)` + `target_compile_definitions`；
2. 头文件用宏包住实现，关闭时展开为空；
3. CLI 参数进 `main.cpp` -> `diag::Options`；
4. `--build-info` 的 `CompileTimeFeatures` / `Options` 各加一个字段；
5. 在 [test-matrix](../50-validation/test-matrix.md) 里加一项（至少验证"关闭时 guard PASS"）；
6. 更新本文件 §2 的开关表与 [cli-reference](../50-validation/cli-reference.md)。
