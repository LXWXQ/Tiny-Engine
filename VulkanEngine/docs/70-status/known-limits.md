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

## 编辑器（D-8）

- **单选**：`Selection` 接口按集合设计，但 UI 只做单选。
- **材质参数不可撤销**：Inspector 里改 metallic/roughness/baseColor 不进命令栈。
  它们是外观调参，撤销需求弱；真正需要的是"材质库 + 引用的撤销"，那是另一个量级。
- **播放模式没有逻辑**：`PlayState` 状态机与快照/恢复已就位，但没有脚本/物理推进点。
  以后加逻辑时在 `EditorLayer::updatePlayState()` 里接。
- **无 docking**：ImGui 1.92.6 是非 docking 分支，布局是手写的固定位置。
  换成 docking 分支后 `drawPanels()` 里的布局分支可以整段删掉。
- **实体 ID 不稳定**：删除后撤销，实体 ID 会变。任何跨帧缓存 `entt::entity`
  的地方（选中态除外，它会被显式更新）都不能假设 ID 稳定。

## 视口交互（D-10）

- **只有单轴手柄**：没有平面手柄（XY/YZ/XZ）与屏幕空间手柄。
- **旋转以四元数为准，Inspector 显示欧拉角**：万向节附近欧拉数值可能跳变，
  但物体矩阵不会死锁；保存/加载仍走欧拉角（版本 1 格式），极端姿态保存后会
  有等价但不同的欧拉表示。父节点带非均匀缩放时局部轴可能不严格正交。
- **拾取已精确到三角形**，但 Sponza 整栋仍是一个实体，所以点任意墙面都选中它
  —— 要选到内部构件需要先把 glTF 节点拆成独立实体（需改 `GLTFLoader` 保留节点树）。
- **旋转绕物体原点而非包围盒中心**：手柄视觉中心在 OBB 中心，
  但 `orientation` 是绕自身原点的旋转，两者在物体原点偏离几何中心时有偏差。
- **选中框是 OBB**：局部 AABB × 世界矩阵，跟随旋转/缩放；拾取粗筛仍用世界 AABB，
  所以点选范围会比 OBB 略大（对细长/旋转物体尤其明显）。

## 配置与相机

- **退出会自动保存相机姿态**（`exit also saves`）。在编辑器里移动相机后正常关闭，
  下次启动会回到那个位置。这是刻意行为（引擎要记住视角），但如果想回到默认机位，
  需要手动改 `config/engine.json` 的 `camera` 段或删除该文件。
- **yaw 不做归一化**：长时间转视角后 `rotation.y` 会累积成很大的数值
  （实测出现过 -1101 rad）。功能上无影响（三角函数是周期性的），
  但配置文件里看着很怪。
