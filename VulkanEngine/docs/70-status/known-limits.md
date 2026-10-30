---
id: status/known-limits
title: 已知边界
type: status
scope: 空
read-when: 判断某个需求现在能不能做
size: ~100 行
updated: 2026-10-30
---

# 已知边界

## 1. 平台与硬件

- 仅 **Windows x64**。无 Linux / macOS / Android。
- 需要 **Vulkan 1.3** 且支持 `dynamicRendering`、`synchronization2`、`timelineSemaphore`、
  `descriptorIndexing`、`fragmentStoresAndAtomics`、`maintenance4`。
- 未做多 GPU / 设备选择 UI。

## 2. 渲染特性缺失

无：阴影（CSM）、SSR、自动曝光、TAA/超分、动画、骨骼、粒子、后处理抗锯齿、
体积光、透明排序、实例化、LOD、GPU culling、间接绘制。

## 3. 架构限制

| 限制 | 后果 | 详见 |
|---|---|---|
| 跨队列资源走 `CONCURRENT` sharing | 未实现 ownership transfer；带宽不是最优 | [weak-points](../10-architecture/weak-points.md) #1 |
| glTF 几何烘焙到世界空间 | Sponza 内部节点不是独立 entity，不能局部动 | 决策 13 |
| SpatialIndex 每帧重建 | 实体多时 CPU 成本线性上升 | [scene-ecs](../30-systems/scene-ecs.md) §3 |
| 线性渲染图 | 无自动重排/别名化/死代码剔除 | 决策 2 |
| 材质槽位只增不减 | 反复换场景会吃满 256 槽 | [tech-debt](tech-debt.md) #3 |
| 单场景单相机 | 无多视口 / 分屏 | — |

## 4. 可观测性限制

- 回归最长只跑 120 帧，**没有小时级稳定性测试**。
- 交互式关窗路径（用户点 X）没有单独回归——历史 VMA 崩溃就藏在析构路径上。
- 截图类测试依赖"窗口在前台"，脚本做了检查但那是脚本级保障。
- 没有自动化视觉金标准（`Screenshot.ps1` 有 diff 能力，但没有纳入默认回归的金标准图）。

## 5. 内容与资产

- `3rdParty/` 与 `Resources/` **不入库**，需 `SetupDependencies.ps1` + 手动准备。
- 当前场景固定为 Sponza + 一个测试球 + 天空盒，没有场景描述格式。
- 材质只支持 4 张 PBR 贴图（albedo / normal / metallic / roughness），
  无法线贴图实际采样、无 emissive / AO 的独立通道（emissive 贴在 albedo 里）。

## 6. 工程化限制

- 无 CI（全部靠本地 `Validate.ps1`）。
- 无 Release 构建的性能基线（所有数字都是 Debug）。
- 无单元测试框架，测试都以"进程内自检 + stdout 标记"的形式存在。
