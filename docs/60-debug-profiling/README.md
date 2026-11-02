---
id: debug-profiling
title: 调试与剖析索引
type: debug
scope: src/Core/Diagnostics.*, src/RHI/GpuTimer.*, src/RHI/ScreenshotCapture.*, tools/
read-when: 加调试能力、排查画面/性能问题、或改代码前想避开已知坑
size: ~60 行
updated: 2026-10-30
---

# 60-debug-profiling 调试与剖析

| 文档 | 回答的问题 | 优先级 |
|---|---|---|
| [pitfalls](pitfalls.md) | 哪些坑已经踩过？现象/判据/修法是什么？ | **改代码前必读** |
| [zero-overhead](zero-overhead.md) | 加调试能力怎么做到"关掉时零开销"？ | 加任何调试能力前必读 |
| [tools](tools.md) | 有哪些工具？各自解决什么问题？ | 排查时查表 |
| [diagnostics](diagnostics.md) | 验证层结构化、GPU 计时、截图怎么实现？ | 改这三块时 |

## 铁律

所有调试 / 剖析能力都**随用随开**：默认关闭，关闭时不分配、不插 GPU 命令、不落盘。
是否"编译进来"由 CMake 的 `TINY_ENABLE_*` 决定；是否"本次开启"由命令行 flag 决定。
判断标准不是"绝对零指令"，而是**"不能有可测量的渲染性能回退"**。

每次运行结束会打印 `[Diag] zero-overhead guard PASS|SKIP|FAIL | queryPool=N | screenshotBuffer=N`。
默认（无 flag）必须是 `PASS | queryPool=0 | screenshotBuffer=0`。
