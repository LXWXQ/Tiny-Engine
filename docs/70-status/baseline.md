---
id: status/baseline
title: 三场景性能基线
type: status
scope: tools/bench.ps1, src/RHI/FrameStats.h, src/RHI/FrameStats.cpp, src/RHI/RenderGraph.cpp, src/RHI/Pipeline.cpp
read-when: 改渲染图 / 屏障 / 管线绑定之前要一份可对比的数字，或怀疑性能回退时
size: ~80 行
updated: 2026-11-23
---

# 三场景性能基线

## 怎么跑

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench.ps1
```

默认 3 场景 × 3 次 × 600 帧，输出 `build/bench/<yyyyMMdd-HHmmss>/baseline.md`，
路径同时写入 `build/bench/latest.txt`。单场景快跑用
`-Scenario lights-512 -Runs 1 -Frames 120`。

判定分两层：exit 码、stderr 字节数、`[Validation] errors/warnings` 是**硬失败**；
FPS 波动超 `-MaxVariancePct` 只是**警告**，加 `-Strict` 才升级成失败——因为波动先反映机器忙，不先反映代码坏了（见下节实测）。

## 三个场景

| 场景 | 附加开关 | 压的是什么 |
|---|---|---|
| `default` | 无 | 1 draw call / 8192 三角形，测固定开销 |
| `voxel-world` | `--voxel-world 1` | 106 draw calls / 336889 三角形，几何与提交压力 |
| `lights-512` | `--voxel-world 1 --light-stress 512 --clustered-lighting --clustered-gpu-build` | 512 盏点光 + GPU 建簇，光照压力 |

与 [ue-borrowing-plan](../80-roadmap/ue-borrowing-plan.md) P0-1 的口径偏差：计划里写的 `cube-field`
（1 万立方体）在引擎里没有生成器，这一轮用 `--voxel-world 1` 当几何压力场景；`editor-default`
用 `--selftest` 的 `default` 代替（编辑器面板不在 selftest 路径上）。两处偏差要以这份表和脚本里的场景表为准。

## 基线（2026-11-23，Debug，RTX 2070，600 帧 × 3 次）

| 场景 | FPS（中位） | 波动 % | 帧 ms | passes/帧 | 屏障/帧 | sameState | crossQueue | 绑定/帧 | 冗余 % | draw calls | desc binds | 三角形 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| default | 160.82 | 2.78 | 6.22 | 21 | 45 | 19 | 0 | 22.5 | 46.7 | 1 | 2 | 8192 |
| voxel-world | 96.15 | 1.18 | 10.40 | 25 | 61 | 34 | 0 | 38.5 | 50.6 | 106 | 4 | 336889 |
| lights-512 | 88.93 | 6.42 | 11.24 | 27 | 61 | 36 | 2 | 39.5 | 49.4 | 106 | 4 | 336889 |

列的含义：

- `屏障/帧`：`vkCmdPipelineBarrier2` 的调用次数。它恒等于 `entries`，因为 `emitBarrierIfNeeded` 对每个 mip × layer 各发一条——这个重复量正是 T-03 要合的。
- `sameState`：状态本来就相同、且是读访问，因此跳过的次数（不发屏障是对的）。
- `crossQueue`：跨队列资源交给 timeline 信号后跳过的次数。`lights-512` 的 2 是簇表 ranges / indices 两段，也是 T-02 通路是否走通的判据。
- `绑定/帧` 与 `冗余 %`：管线绑定次数，以及"与上一次绑的是同一条管线"的占比。冗余占比是 T-09 状态缓存能消掉的上限，不是它的目标值。

## 波动与判据

同一台机器、同一二进制，只改运行窗口，波动差一个量级：

| 窗口 | default | voxel-world | lights-512 |
|---|---|---|---|
| 120 帧（0.9~1.6 s） | 13.65% | 2.90% | 6.53% |
| 600 帧（3.7~6.8 s），批次 A | 0.91% | 1.21% | 1.88% |
| 600 帧，批次 B | 2.78% | 1.18% | 6.42% |

同一场景的**跨批次**中位数也差约 4%（default 166.85 vs 160.82 FPS）。所以：

- 判据用**同批次 A/B 比例**，不用绝对 FPS，也不用跨批次中位数。
- 计划里"三次波动 < 2%"这条，在本机只有 600 帧窗口且机器空闲时才成立；它是机器属性，不是代码属性。
- 想拿"机器是否空闲"的证据时加 `-Strict`，平时不加，否则噪声会误伤提交。

## 什么时候重跑

改渲染图、屏障发射、管线绑定这三处之前跑一次留底；改完再跑一次同批次对比。
两次的数字并排算比例，结论与偏差写进 [milestones](milestones.md)。