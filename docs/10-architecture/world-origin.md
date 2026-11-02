---
id: architecture/world-origin
title: 浮动原点：局部坐标、绝对原点与地形噪声锚点
type: architecture
scope: src/Scene/Scene.h, src/Scene/Scene.cpp, src/Scene/CameraController.h, src/Scene/CameraController.cpp, src/Core/FrameInfo.h
read-when: 改场景坐标、接大世界、排查远距离抖动或地形接缝、调 Origin rebase 阈值
size: ~75 行
updated: 2026-11-02
---

# 浮动原点：局部坐标、绝对原点与地形噪声锚点

## 1. 为什么需要

`float` 在 1e6 量级的间距约 0.06，在 1e7 量级约 0.5。相机走远之后，世界矩阵、视锥、
深度缓冲都会开始抖。浮动原点的做法是：**场景里的坐标永远是「原点附近」的小数**，
真正走出去的那部分记在一个 `double` 的绝对原点里。

## 2. 三套坐标

| 坐标 | 类型 | 存在哪 | 含义 |
|---|---|---|---|
| 局部坐标 | `float` | registry 里所有 `TransformComponent`、相机、`WorldTransformComponent` | 相对当前原点，渲染与物理只用它 |
| 绝对原点 | `glm::dvec3` | `Scene::worldOrigin` | 累计 rebase 的位移，double 保证不丢精度 |
| 噪声锚点 | `float` | `TerrainConfig::sampleOriginX/Z` 与 `HeightField::sampleOriginX/Z` | **绝对**坐标下该高度场噪声的锚点，不随 rebase 变化 |

换算：`absolute = worldOrigin + local`。只有需要「跨 rebase 保持同一世界」的系统
（地形噪声、编辑器缝合比较）才用绝对坐标，其余一律用局部坐标。

## 3. rebase 做了什么

触发条件：相机局部坐标的水平距离超过 `EngineSettings::originRebaseThreshold`（默认 1024）。

1. 位移**吸到整块 chunk**：`delta = floor(local / chunkSize) * chunkSize`，这样流送网格的整数索引仍然是整数。
2. 遍历所有 `TransformComponent` 减去 `delta`，并 `markTransformDirty`（含相机实体）。
3. `CameraController::offsetWorldPosition(delta)`：平滑状态（`smoothPosition` / `focusPoint` / `lastAppliedPosition`）缓存的是绝对坐标，必须手动跟着挪，否则下一帧相机会被拉回旧位置。
4. 流送 chunk 的 `TerrainChunkComponent::chunkX/Z` 与 `streamingTerrainChunks` / `pendingStreamingTerrainChunks` 的 key 同步减 `chunkDelta`。
5. `worldOrigin += delta`，`originRebaseCount += 1`，打一行 `[Origin] rebased | ...`。

**不做什么**：不改 `TerrainComponent::config.sampleOriginX/Z`，也不改 `cpuHeightField->sampleOriginX/Z`。

## 4. 为什么噪声锚点保持绝对

- `HeightField::sample()` 只用 `sizeX/sizeZ` 做归一化，**完全不读 sampleOrigin**，所以查询与碰撞对 rebase 免疫。
- `sampleOrigin` 只被两处使用：生成地形时作为噪声的绝对坐标基准；编辑器缝合时把两侧局部坐标换算到同一绝对坐标系比较。
- 一旦 rebase 时把 `sampleOrigin` 一起平移，rebase 之后新建的 chunk 会用不同的噪声锚点重新采样，与老 chunk 的高度场错位，接缝立刻可见。
- 因此 chunk 创建时写的是 `config.sampleOriginX = worldOrigin.x + centerWorldX`（绝对），而不是原来的 `centerWorldX`。

## 5. 开关与命令行

| 设置 | 默认 | 命令行 |
|---|---|---|
| `originRebaseEnabled` | `true` | `--no-origin-rebase` |
| `originRebaseThreshold` | `1024.0f` | `--origin-threshold <units>` |

`--streaming-fly` 的巡航半径可用 `--streaming-fly-radius <r>` 调整。**巡航是直线而不是圆周**：
圆周上相机到原点的距离恒等于半径，rebase 之后下一帧又被写回半径处，会每帧触发一次；
直线巡航才能让局部坐标反复「走到阈值 → 被拉回原点附近」，原点也才真的在前进。

## 6. 可观测性

```
[Origin] rebased | delta=(x, y, z) origin=(X, Y, Z) rebases=<n>
[Origin] stats | rebases=<n> origin=(X, Y, Z) maxLocal=<f> threshold=<f>
```

自检断言：飞行半径大于两倍阈值时必须发生 rebase；`maxLocal` 不得超过
`threshold + 128`（否则说明 rebase 没生效或相机被写回旧坐标系）。

## 7. 已知限制

- 噪声锚点仍是 `float`：绝对坐标到 1e6 时量化步长约 0.06，地形细节会变粗，但相邻 chunk 用同一量化网格，不会产生接缝。
- chunk 索引是 `int32`，可用范围 ±2^31 个 chunk；按 32 米一块计算远超需求，但没有做回绕保护。
- 序列化仍以原点 0 为基准：存档只保存局部坐标，重开时 `worldOrigin` 归零，相机与地形会回到原点附近（地形配置里的绝对 `sampleOrigin` 会保留）。
- 编辑器 gizmo / 光照方向等都以局部坐标工作，没有做「绝对坐标显示」。
