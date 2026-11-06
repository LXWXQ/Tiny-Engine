---
id: roadmap/roadmap
title: 主功能路线
type: roadmap
scope: 空
read-when: 开新主功能之前；或判断"某个需求排在哪一阶段"
size: ~180 行
updated: 2026-11-02
---

# 主功能路线

## 0. 顺序与理由

| 阶段 | 主功能 | 前置条件 |
|---|---|---|
| F1 | GPU 驱动渲染 | 跨队列同步已就绪（TD-02 已完成）；子资源校验已就绪（TD-05） |
| F2 | 地形系统 | F1 + JobSystem 生产接入 |
| F3 | 体素系统 | F1 + F2 的 chunk 框架 |
| F4 | 资产与场景管线 | 保留 glTF 节点层级 |
| F5 | 运行时架构升级 | F2/F3 的压力测试数据 |
| F6 | 渲染特性（阴影/SSR/自动曝光） | F1/F2 的渲染基础 |
| F7 | 物理 / 动画 / 工具 | 世界数据与坐标系统稳定 |

**不要跳**：GPU 驱动渲染是地形 chunk 数量上来后的性能前提。先做地形会立刻撞到
"CPU 逐个遍历实体提交 draw"的天花板。

---

## F1：GPU 驱动渲染

**目标**

- GPU 视锥剔除 / Hi-Z 遮挡剔除
- indirect draw / indirect dispatch
- Transform / bounds / material 走 SSBO
- 实例化：相同模型/材质的 draw 合并
- 为地形和体素 chunk 提供统一批量提交路径

**关键设计**

```
CPU:  场景实体/Transform -> GPU Scene Buffer（每帧或 dirty 时更新）
GPU:  视锥剔除 + LOD 选择 -> indirect argument buffer
Graphics: vkCmdDrawIndexedIndirect
```

**前置**

- 跨队列 compute 写、graphics 读的调度（TD-02 已有基础设施，但 **EXCLUSIVE
  ownership transfer 未实现**，见 [known-limits](../70-status/known-limits.md)）
- glTF 节点层级（否则实例化没有数据来源）

**验收**

- 104 个子网格在不增加 draw call 的情况下支持间接提交
- CPU 不再逐个遍历实体决定是否绘制；`GeometrySystem::render` 耗时显著下降
- 验证层 0 字节；`--validation-features sync` 0 errors

**进度（2026-11-02）**
- F1-a：实例缓冲摊平（224B/条）、compute 视锥剔除、`vkCmdDrawIndexedIndirectCount` 按模型提交、set 3 实例 SSBO、GPU 批遥测回读；详见 [gpu-driven-submission](../10-architecture/gpu-driven-submission.md)。
- F1-b：可批模型合并进全局顶点/索引缓冲，单 range/单 counter，一条 `vkCmdDrawIndexedIndirectCount` 画完；Sponza 的 draw call 104 -> 2，三路像素比对 `max diff = 0`。
- F1-c：分组键改成 `(模型, 子网格)` 实现真正的实例化（`instanceCount = N`）、组按材质排序、包围盒缓存改用 `Model::getGeometryId()`、Hi-Z 遮挡剔除（`R32_SFLOAT` 金字塔 + 错帧一帧 + 组级保守测试）；`--no-hiz` 可关。
- F1-d：稳定帧快速路径（结构签名命中即跳过摊平与 `instances/groups` 上传，实测 439µs → 9µs；只写 256B `frameData` 再 dispatch）+ 修掉 Hi-Z 的跨帧屏障，`--validation-features sync` 0 错误。
- F1-e：非合并模式的逐模型 bind 中间路径删除（合并缓冲是唯一 GPU 提交路径，否则整帧回 CPU）；`FrameData` 加 `vec4 screen` 做屏幕空间误差剔除（`stats[2]` 记 `screenTests/screenCulled`）；新增 `ExternalInstance` 入口，粒子/体素与 ECS 实例共用分组、合并缓冲与同一条 indirect 命令。
- 阈值 256 实例：小场景仍走 CPU 路径，`--gpu-scene` 强制、`--no-gpu-scene` 关闭、`--no-merged-buffers` 整条退回 CPU、`--gpu-scene-min-px` 开屏幕空间误差剔除、`--gpu-scene-external` 注入外部实例。
- 验收状态：draw call 104 -> 2、CPU 不再逐实体决定绘制（只摊平一次，之后靠签名跳过）、验证层 0 字节、`--validation-features sync` 0 errors —— 均已达成。
- 留待需要时：**GPU 端 LOD 选级**（需要模型 LOD 链；SSE 判据已就位，目前只用来剔除，等 F4 资产管线给出 LOD 后再改成选级）与 **粒子/体素真实接入**（`ExternalInstance` 入口已就位，等 F3 体素 / F6 特效系统来喂数据）。两条都记在 [tech-debt](../70-status/tech-debt.md) 的未关闭表里，不占 F1 的完成度。
- Hi-Z 在当前 Sponza 场景 `hizCulled = 0`：103 个子网格各自覆盖整栋建筑一大块，组级保守剔除几乎不命中；收益等大量小实例（见 [gpu-driven-submission](../10-architecture/gpu-driven-submission.md) §9）。

---

## F2：地形系统

**目标**：高度场 chunk（64x64 或 128x128 顶点）、chunk LOD + 邻居缝合（避免 T 形裂缝）、
多层材质混合、按相机距离流送、碰撞数据生成、编辑 API。

**实施顺序**：先单块、同步、顶点色 MVP，再按 [terrain-plan](terrain-plan.md) 的
阶段 6/7 上纹理分层、异步、chunk/LOD。不要一次做完整个 F2。

**当前进展**：chunk 子网格、逐块剔除、CPU 滞回选层、抖动 cross-fade、
裙边接缝、稳定帧 indirect 合批、GPU compute 高度场、独立 chunk 流送、8 槽异步高度场、流送交互硬化、拓扑级 LOD stitching、GPU 选层/culling、单 chunk GPU 顶点/索引组装、4 槽异步 GPU mesh 上传、4 个 Set2 地形材质集（按需外部 16 PNG/程序化回退、`materialSetIndex` 序列化）、kinematic 角色控制器与区域限定运行期雕刻已完成；**F2-a（2026-11-02）**：新增可持续运行的流送世界入口 `--streaming-world [radius]`（构造期建 manager，半径 1~8，chunk 32 单位 / 33x33 顶点 / 3 级 LOD）与 `--streaming-fly [speed]`（相机沿圆周飞，验证持续加载/卸载），并在 `Scene` 里补 loaded/unloaded 累计统计。剩下的是真正的无限世界：多 chunk GPU 合批（目前每 chunk 一次 indirect）与浮动原点（float 坐标 + 半径上限 8）。雕刻持久化仍待做。

**CPU/GPU 分工**

- CPU JobSystem：高度采样、chunk 网格生成、碰撞数据准备
- GPU compute：视锥剔除、LOD 选择、indirect 参数生成

**必须先补的运行时能力**

- `SpatialIndex` bucket 增量 upsert/remove、Transform/Bounds 私有字段/dirty、streaming chunk XZ region bucket、多 chunk GPU 批处理与 4 个 Set2 地形材质集已完成；下一步浮动原点 / 无限世界
- JobSystem 支持任务依赖图或至少分层 `parallelFor`（chunk 生成 -> LOD -> 上传）；
  地形当前已有 Scene 级协作取消

**进度（2026-11-03，F2-a ~ F2-h）**：`--streaming-world [radius]` 给出可运行的流送世界（chunk 32、33x33、3 级 LOD，半径 1~8，每帧最多 4 个），`--streaming-fly [speed]` 圆周飞行验证加载/卸载滞回；多 chunk 合批已落地：`TerrainMeshArena`（96 槽）+ set 3 chunk 实例表 + 一条 `vkCmdDrawIndexedIndirect` 覆盖 2048 条命令，半径 2 的 `draw calls` 从 13 降到 2，两路截图逐像素一致。F2-c 浮动原点已落地：registry 只存局部坐标、绝对位移记在 `Scene::worldOrigin`（double），相机超过 `--origin-threshold`（默认 1024）时整场景吸到整块 chunk 回原点，地形噪声锚点保持绝对所以接缝不受影响；`--streaming-fly` 改成直线巡航（圆周半径恒定会让 rebase 每帧触发）。F2-d 把容量推到半径 12（625 块）：地形 GPU 选择 64 → 384 槽、命令区 4096 → 12288、arena 96 → 384 槽（单槽 2048 顶点 / 12288 索引，约 51 MB），并加每帧生成预算 `maxUpdateMs`（默认 2 ms，`--streaming-budget`）。实测半径 2/4/8/12 = 190/164/117/68 FPS，瓶颈已从 GPU 选择槽位转移到每 chunk 的 CPU 簿记（transform / 空间索引 / 地形 record 写入）。F2-e 把地形拾取从「每 chunk 一份全分辨率 CPU 顶点副本」改成 `HeightField` 射线步进 + 二分（`Scene::raycastTerrain()` 不再要求 `MeshComponent`，`TerrainGpuMeshBuilder` 删掉 `buildPickingMirror()` 与 picking future，每 chunk 省约 38 KB CPU 内存，编辑器的流送 chunk 拾取自检 `picking=OK`）。F2-f 把地形几何彻底搬进共享 `TerrainMeshArena`（arena 归 `Scene`，chunk 落地即 `acquire()` + `copyInto()` + `Model::releaseGpuBuffers()`，Model 只剩元数据；抢不到槽位的 chunk 保留自己的 buffer 作为回退），并修掉「地形合批换掉 set 3 后场景合批没绑回来」的显示 bug。F2-g 把 `TerrainMeshArena` 从「384 个固定槽」换成「按实际尺寸分配的 bump + 空闲区间合并」（顶点 1.1M ≈ 46 MB、索引 5.9M ≈ 22.5 MB，`Slice` 记录 base/count，`release()` 前后合并；半径 2 的 25 块只占 25 次分配，飞行 600 帧 `arenaAllocs` 在 305~323 回落）。F2-h 把 chunk 坐标换成 `int64`、采样锚点换成 `double`（噪声仍是 `float`），流送半径上限 12 → 24（2401 块），arena 容量按整环尺寸原地重建；实测半径 2/12/16/24 = 190/70/36/17 FPS（**Debug 构建**），瓶颈是每 chunk 每帧的 CPU 簿记，不再是槽位或容量。F2-i 澄清了这笔账：同一场景 Release 半径 24 是 **208 FPS**、`Scene::update()` 约 62 µs/帧，Debug 与 Release 在 `try_get` 循环上差约 34~100 倍；同时把 `updateVisibility` 换成线性扫描（不再是 `SpatialIndex::query` 的退化展开 + 排序），并给流送扫描加稳态早退（70 µs → 0.4 µs）。仍未完成：**GPU 端 LOD 选级**（按用户决定推迟到 F4 资产管线，等模型 LOD 链）、**粒子/体素真实接入**（`ExternalInstance` 入口已就位）、**每 chunk CPU 簿记削减**（把静态 record 缓存成只在 LOD/材质变化时重写）。

**验收**：相机跨越多个 chunk 无明显卡顿与裂缝；LOD 切换可见；顶点量增加时 FPS 不线性崩塌。

---

## F3：体素系统

**状态**：已完成 F3-a/b/c（[voxel](../30-systems/voxel.md)）—— 32³ 方块 chunk、面剔除网格、
Amanatides-Woo DDA 拾取、挖/放 + 逐格撤销、`queryGroundAtWorld` 叠加体素分支（角色能站上方块）、
`--voxel-world [r]` 与编辑器 Voxel Brush。

**与原计划的差异**：改用块状立方体而不是 marching cubes / surface nets —— 挖掘建造要的是
离散方块（挖掉一格是整齐的坑），等值面会得到圆滑洞；LOD 与邻接缝合也不再需要（相邻 chunk
用同一套噪声按世界列算高度，边界天然连续）。

**剩余**（见 [backlog](../70-status/backlog.md#归档时的剩余队列2026-11-04)）：透明方块排序（V-01）、网格化每帧预算与取消（V-02）、体素 LOD（V-03）、体素几何走 arena 合批（V-04）、分页无限世界（V-05）。已完成：贪心合并 + 逐顶点 AO（F3-e）、程序化贴图集（F3-f）、异步构建 + 半径 12（F3-g）

**进度（2026-11-04）**：F3-a~F3-g 全部落地 —— 32³ 方块 chunk、面剔除 + 贪心合并 + 逐顶点 AO、程序化 sRGB 图集（顶点 UV 编码图块 id）、Amanatides-Woo DDA 拾取、挖/放 + 逐格撤销、体素持久化（seed + 差分）、`queryGroundAtWorld` 体素分支、运行模式接管角色（WASD/空格/中键/左右键）、异步构建（每帧最多上传 4 块，Release 半径 12 提交 7.9 ms）。

---

## F4：资产与场景管线

**目标**

- **保留 glTF 内部 node 层级**：不再把几何永久烘焙成世界空间
- 每个 primitive instance 可独立变换（门、机械部件、植被）
- 资产引用计数 + 异步加载 + 材质流送
- Prefab / 场景描述格式（供未来编辑器读写）

**当前阻塞**：`GLTFLoader.cpp` 把 node 变换直接烘焙进顶点位置
（[decisions](../10-architecture/decisions.md) 决策 13）。

**验收**：载入 glTF 后可挑任意 node 改 local transform 并实时看到；
同一 mesh 可被多个 entity 实例化而不重复占显存；反复加载/卸载不泄漏材质槽位与 GPU 资源。

---

## F5：运行时架构

- JobSystem：work stealing、任务依赖图、取消、优先级
- ECS systems：元数据组件注册表 + SystemScheduler（Simulation / updateWorldTransforms / PostTransform）已完成（F5-01）；剩余更细阶段与热重载
- SpatialIndex：dirty flag / 增量更新 / 分块索引
- 场景序列化与热重载

**注意**：这些是 F2/F3 的规模化前置。当前版本能工作，但不要在地形 chunk 数量上来后
Transform/Bounds 字段已私有并通过 setter 自动通知 Scene；streaming chunk 已有 XZ region 索引。剩余 `registry.destroy()` 约定、非 chunk 实体的统一网格与全局 JobSystem 单队列。

---

## F6：渲染特性

| 功能 | 说明 | 依赖 |
|---|---|---|
| 阴影（已完成，见 [shadow](../20-pipeline/shadow.md)） | 3 级 CSM 2048² + 3×3 PCF，CPU 逐实体提交，方向光专用 | GPU scene buffer |
| SSR | 屏幕空间反射，读 HDR + depth | HDR 管线已就绪 |
| 自动曝光 | compute 直方图 + 曝光纹理 | compute/graphics 跨队列（已有基础） |
| Compute Bloom | Bloom 金字塔迁移到 compute | TD-06 / TD-07 已完成 |
| TAA（已完成，见 [pass-order](../20-pipeline/pass-order.md)） | Halton 抖动 + 相机重投影 + 3x3 邻域 clamp；无运动向量 | HDR + G-Buffer Position（已有） |
| 时间超分 | 内部低分辨率 + 上采样 | TAA、动态分辨率（已有） |
| 切线空间法线贴图 | 启用 `getNormalFromMap()` | 可靠切线或 TBN 构造 |

---

## F7：物理 / 动画 / 工具

Jolt（物理）、ozz（动画）、Tracy（已接入）、场景编辑器。前置是世界数据与坐标系统稳定。

---

## 开新阶段前的检查清单

- [ ] 对应前置技术债真的关了吗？（查 [tech-debt](../70-status/tech-debt.md)）
- [ ] 需要扩展 RenderGraph 的队列/子资源能力吗？
- [ ] 需要新的验证入口和反例吗？（查 [test-matrix](../50-validation/test-matrix.md)）
- [ ] 会不会破坏"stderr 0 字节 + 验证层 0 errors"的基线？
- [ ] 文档里对应层（architecture / pipeline / systems）的改动范围想清楚了吗？
