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

---

## F2：地形系统

**目标**：高度场 chunk（64x64 或 128x128 顶点）、chunk LOD + 邻居缝合（避免 T 形裂缝）、
多层材质混合、按相机距离流送、碰撞数据生成、编辑 API。

**实施顺序**：先单块、同步、顶点色 MVP，再按 [terrain-plan](terrain-plan.md) 的
阶段 6/7 上纹理分层、异步、chunk/LOD。不要一次做完整个 F2。

**当前进展**：chunk 子网格、逐块剔除、CPU 滞回选层、抖动 cross-fade、
裙边接缝、稳定帧 indirect 合批、GPU compute 高度场、独立 chunk 流送、8 槽异步高度场、流送交互硬化、拓扑级 LOD stitching、GPU 选层/culling、单 chunk GPU 顶点/索引组装、4 槽异步 GPU mesh 上传、4 个 Set2 地形材质集（按需外部 16 PNG/程序化回退、`materialSetIndex` 序列化）、kinematic 角色控制器与区域限定运行期雕刻已完成；无限世界、多 chunk GPU 合批与雕刻持久化仍待做。

**CPU/GPU 分工**

- CPU JobSystem：高度采样、chunk 网格生成、碰撞数据准备
- GPU compute：视锥剔除、LOD 选择、indirect 参数生成

**必须先补的运行时能力**

- `SpatialIndex` bucket 增量 upsert/remove、Transform/Bounds 私有字段/dirty、streaming chunk XZ region bucket 与 4 个 Set2 地形材质集已完成；下一步多 chunk GPU 批处理与无限世界
- JobSystem 支持任务依赖图或至少分层 `parallelFor`（chunk 生成 -> LOD -> 上传）；
  地形当前已有 Scene 级协作取消

**验收**：相机跨越多个 chunk 无明显卡顿与裂缝；LOD 切换可见；顶点量增加时 FPS 不线性崩塌。

---

## F3：体素系统

**目标**：固定大小体素 chunk（如 32^3）、marching cubes / surface nets 网格生成、
chunk LOD + 邻接缝合、放置/删除方块并局部重建、与地形混合的碰撞数据。

**依赖**：F1 的剔除/indirect；JobSystem 并行 meshing；动态编辑需要先解决
**材质槽位回收的生产路径**（见 [tech-debt](../70-status/tech-debt.md) #3）。

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
- ECS systems：明确的 update 阶段（输入 / 变换 / 空间索引 / 剔除 / 渲染提交）
- SpatialIndex：dirty flag / 增量更新 / 分块索引
- 场景序列化与热重载

**注意**：这些是 F2/F3 的规模化前置。当前版本能工作，但不要在地形 chunk 数量上来后
Transform/Bounds 字段已私有并通过 setter 自动通知 Scene；streaming chunk 已有 XZ region 索引。剩余 `registry.destroy()` 约定、非 chunk 实体的统一网格与全局 JobSystem 单队列。

---

## F6：渲染特性

| 功能 | 说明 | 依赖 |
|---|---|---|
| 阴影 | CSM / 局部阴影，接入 deferred lighting | GPU scene buffer |
| SSR | 屏幕空间反射，读 HDR + depth | HDR 管线已就绪 |
| 自动曝光 | compute 直方图 + 曝光纹理 | compute/graphics 跨队列（已有基础） |
| Compute Bloom | Bloom 金字塔迁移到 compute | TD-06 / TD-07 已完成 |
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
