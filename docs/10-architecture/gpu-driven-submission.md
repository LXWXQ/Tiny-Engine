---
id: architecture/gpu-driven-submission
title: GPU 驱动提交：实例缓冲、compute 剔除与间接绘制
type: architecture
scope: src/Render/SceneGpuBatch.h, src/Render/SceneGpuBatch.cpp, shaders/scene_cull.comp, shaders/mrt.vert, shaders/mrt.frag, src/Render/GeometrySystem.cpp
read-when: 改几何提交路径、加实例属性、调视锥剔除、改合并缓冲、接粒子/体素等大批量实例、调试 indirect draw 或 set 3 描述符
size: ~140 行
updated: 2026-11-02
---

# GPU 驱动提交：实例缓冲、compute 剔除与间接绘制

F1 第一刀。目标不是「现在就更快」，而是把「CPU 逐实体决定画什么」换成「CPU 摊平数据、GPU 决定画什么」。当前场景只有 103 个实例，两条路径性能在噪声内相当；收益要等实例数上去才出现。

## 1. 边界

本刀做到：
- CPU 每帧把所有网格子网格摊平成 `InstanceRecord`（224 字节）写进 SSBO，不做任何可见性判断；
- `scene_cull.comp` 做 6 平面视锥剔除，把通过的子网格写成 indirect 命令；
- `vkCmdDrawIndexedIndirectCount` 按模型分组提交，`firstInstance` 携带全局实例下标供顶点/片元着色器取矩阵与材质。

F1-b 补上的（见 §4）：
- **全局合并顶点/索引缓冲**：可批模型的顶点/索引拼进同一份 device-local 缓冲，一条 `vkCmdDrawIndexedIndirectCount` 画完，draw call 从 104 降到 2（含球体仍走旧路径那次）；

仍未做（留给后续）：
- **没有 Hi-Z 遮挡剔除**，只有视锥；
- **没有材质排序/合批**，材质参数仍按实例取（bindless Set1 的槽位 + 实例记录里的 flags）。

## 2. 数据流

```
CPU: registry.view<MeshComponent, WorldTransformComponent>()
       -> 按 Model* 分组 -> instanceScratch（224B/条）
       -> ranges（每组 firstInstance / capacity / modelSlot）
     instances / ranges / frameData / counters / stats 全部是 SSBO
GPU: scene_cull.comp（每实例一个 invocation）
       -> 视锥剔除 -> atomicAdd(counts[modelSlot]) -> 写 commands
Graphics: 合并模式 -> 绑合并顶点/索引缓冲 + 一条 vkCmdDrawIndexedIndirectCount
          回退模式 -> 每个模型一次 vkCmdDrawIndexedIndirectCount
```

关键取舍：**用 `firstInstance` 当全局实例下标**。间接绘制里 `firstInstance` 会变成 `gl_InstanceIndex` 的基值，因此顶点着色器可以直接用它索引实例缓冲，不需要额外的 push constant 或额外的 set。代价是「命令条数 = 可见子网格数」，而不是常规意义的一次 multidraw 画完整个模型。

## 3. 缓冲与描述符

| 缓冲 | 用法 | 内存 |
|---|---|---|
| instances | STORAGE，`instanceCapacity * 224` | host visible，每帧覆盖写 |
| commands | STORAGE + INDIRECT，`instanceCapacity * 5 * 4` | device local |
| counters | STORAGE + INDIRECT + TRANSFER_DST，`modelCapacity * 4` | host visible，每帧清零 |
| ranges | STORAGE，`modelCapacity * 16` | host visible |
| frameData | STORAGE：viewProjection + 6 平面 | host visible |
| stats | STORAGE + TRANSFER_DST，`2 * uvec4` | host visible，回读用 |

- 容量不足时 `vkDeviceWaitIdle` 后整体重建（翻倍），不做两代资源并行 —— 容量变化是低频事件。
- 图形侧只有一个额外布局：`set 3, binding 0`（readonly SSBO，VERTEX|FRAGMENT）。set 0/1/2 保持原样。
- 统计缓冲扩成 8 个 uint：`stats[0] = (submitted, visible, culled, models)`、`stats[1] = (visibleTriangles, visibleVertices, 0, 0)`。

## 4. 着色器

`scene_cull.comp`（`local_size_x = 64`）：

| binding | 内容 |
|---|---|
| 0 | `InstanceRecord instances[]` |
| 1 | `uint commands[]`（按 5 个 uint 手写，避开 std430 结构体对齐歧义） |
| 2 | `uint counts[]`（每模型一个计数器） |
| 3 | `uvec4 ranges[]`（firstInstance, capacity, modelSlot, 0） |
| 4 | `mat4 viewProjection; vec4 planes[6]` |
| 5 | `uvec4 stats[2]` |

剔除用世界空间包围球近似：局部 AABB 中心/半径经 `model` 缩放后逐平面测 `dot(n, c) + d < -r`。命令字段固定为 `(indexCount, 1, firstIndex, vertexOffset, firstInstance)`。


## 4.1 合并缓冲（F1-b）

间接命令**不带顶点缓冲绑定**，所以 F1-a 的每条命令只能服务同一个模型。F1-b 的做法是把可批模型的几何拼进一份全局缓冲，让所有模型的命令都能用同一次绑定提交：

| 步骤 | 做法 |
|---|---|
| 收集 | `buildInstances` 先扫一遍同条件的实体，得到去重的 `std::vector<const Model*>`（跳过 skybox/grid/terrain 与非索引网格） |
| 签名 | FNV-1a 覆盖每个模型的指针 + 顶点数 + 索引数；签名不变直接复用 |
| 拼接 | 顶点与索引分别 `insert` 进两个 vector；**索引保持原样**，每个模型记 `{firstIndex, baseVertex}` |
| 上传 | device-local 缓冲 + staging + `cmdBeginSingleTime/cmdEndSingleTime`；屏障 `TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ | INDEX_READ` |
| 命令 | 实例记录的 `draw = (mergedFirstIndex + firstIndex, indexCount, baseVertex, 0)` |
| range | 只写 `ranges[0] = (0, instanceCapacity, 0, 0)`，所有实例共用一个 counter |
| 提交 | 绑合并 VBO/IBO + 一条 `vkCmdDrawIndexedIndirectCount` |

`baseVertex` 通过 indirect 命令的 `vertexOffset` 施加，语义与 `vkCmdDrawIndexed` 的 vertexOffset 一致，所以索引缓冲不需要重写（Sponza 78.7 万索引，省掉一次全量加法）。

源数据来自 `Model` 新增的 CPU 副本（`getCpuVertices()/getCpuIndices()`）。地形模型保留 `isTerrainChunk` 子网格且 LOD + 裙边体积大，**不保留 CPU 副本**，也不参与合并；任一可批模型缺 CPU 几何时 `ensureMergedMesh` 返回 false，整帧退回 F1-a 的每模型路径（`--no-merged-buffers` 可强制回退）。

实测：`[SceneGpu] merged buffers built | vertices=192496 indices=786801 vertexMB=8 indexMB=3`；相机朝向 Sponza 时 `draw calls 2 / triangles 270459 / vertices 811377`，与 CPU 路径逐像素一致（`max diff = 0`）。

## 5. 管线接线

1. `GeometryPass::init()` 先建 `SceneGpuBatch`，再把 `getInstanceSetLayout()` 与裸指针交给 `GeometrySystem`（成员声明顺序保证 `SceneGpuBatch` 比 `GeometrySystem` 活得久）。
2. `GeometryPass::addToGraph()` 在 `Geometry` 之前注册无附件 pass `SceneGpuCull`，执行 `sceneGpuBatch->prepare()`：摊平实例、清计数、写 ranges/frameData、dispatch、一条 `VkMemoryBarrier`（SHADER_WRITE -> INDIRECT_COMMAND_READ | SHADER_READ）。
3. `GeometrySystem::render()` 里，普通网格（非 skybox/grid/terrain 且索引数 > 0）在当前帧 `useGpuPath()` 为真时直接 `continue`，交给 GPU 批。
4. `GeometrySystem::render()` 开头（set 0/1/2 之后）**必须**再绑一次 set 3：所有图形管线都静态使用 set 3，走旧路径的地形/天空盒绘制也受同一条 VUID 约束，漏绑会报 `VUID-vkCmdDrawIndexedIndirectCount-None-08600`。
5. 提交块：push 一份 `gpuScene = 1` 的 `SimplePushConstantData`（矩阵留单位、材质无效），绑 `geometryPipeline`，`recordDraw()` 逐模型 bind 顶点缓冲 + `vkCmdDrawIndexedIndirectCount`。

`SimplePushConstantData` 末尾是 `uint32_t gpuScene`：非 0 时 `mrt.vert` 从实例缓冲取 `model`/`normal` 覆盖 push 的矩阵，并输出 `flat fragInstanceIndex`；`mrt.frag` 用它取 flags/misc/uvTransform 覆盖材质参数（地形分支仍读 push）。

注意 push constant 尺寸：C++ 侧因 `glm::mat4` 的 16 字节对齐补齐到 **224** 字节，GLSL 侧块只有 212 字节。`static_assert` 用 `kMaterialOffset + 96`，push range 传 `sizeof` 是合法的。

## 6. 开关与阈值

`--gpu-scene` 强制走 GPU 路径，`--no-gpu-scene` 整条关掉，`--no-merged-buffers` 保留 GPU 路径但退回每模型 indirect。阈值 `gpuSceneMinInstances = 256`：当前 103 实例默认仍走 CPU 路径。


| 设置 | 默认 | 作用 |
|---|---|---|
| `gpuSceneEnabled` | true | 关掉后 `prepare()` 直接返回，完全走 CPU 路径 |
| `gpuSceneMinInstances` | 256 | 实例数低于阈值仍走 CPU 路径：几十条间接绘制比直接绘制慢 |
| `gpuSceneForce` | false | 忽略阈值，强制走 GPU 批（验证用） |

`SceneGpuBatch::useGpuPath(frameInfo)` 是唯一判定点：`available && gpuSceneEnabled && (gpuSceneForce || stats.submitted >= gpuSceneMinInstances)`。`stats.submitted` 在 `buildInstances()` 之后由 CPU 写入本帧真实条数（GPU 回读的 `submitted` 滞后两帧，不参与判定）。

命令行：`--gpu-scene` 强制、`--no-gpu-scene` 关闭。

## 7. 遥测

- `readStats()` 读 host 可见统计缓冲前必须 `vmaInvalidateAllocation(...)`，否则永远读到 0（与 `GeometrySystemGpu.cpp:93` 同一坑）。
- 可见数/剔除数/可见图元数是 GPU 回读，**滞后两帧**；日志与面板要按「上一帧或更早」理解。
- `frameInfo.telemetry.drawCalls += stats.visible`：命令组数量不等于 draw call 数，组内每个可见实例都是一条间接绘制。

## 8. 验证

- 像素级等价：临时把相机放到 `position [0,6,45] / rotation [0,pi,0]`，分别出 `--gpu-scene` 与 `--no-gpu-scene` 截图，忽略 ImGui 面板区域后 `max diff = 0`。改动提交路径后应重做这一步。
- 回归：`tools/Validate.ps1` 的 `f1-gpu-scene` 用 `--selftest 40 --gpu-scene`，要求 `[SceneGpu] GPU batch ready`、`[SceneGpu] indirect batch | models=`、`[Validation] errors=0 warnings=0`。
- 计时：本机 120 帧短跑波动可达 40 FPS，比较路径必须用 300 帧以上并多轮取中位；当前 103 实例下两条路径差异在噪声内。

## 9. 已知限制

- 包围盒缓存以 `const Model*` 为键（`unordered_map<const Model*, std::vector<SubMeshBounds>>`，超过 64 个整表丢弃）。模型销毁后地址被复用可能命中过期包围盒；`forgetModel()` 已实现但还没有调用方。
- 合并缓冲按模型集合整体重建：模型增删或规模变化会触发一次全量重传（低频；大资产场景要观察重建耗时）。
- 合并缓冲要求可批模型带完整 CPU 顶点/索引副本（Sponza 约 11MB 常驻），地形模型不保留。
- 只有视锥剔除，没有遮挡剔除。
- 遥测滞后两帧，`drawCalls` 也是。
