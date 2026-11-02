---
id: architecture/gpu-driven-submission
title: GPU 驱动提交：实例缓冲、compute 剔除与间接绘制
type: architecture
scope: src/Render/SceneGpuBatch.h, src/Render/SceneGpuBatch.cpp, shaders/scene_cull.comp, shaders/hiz_build.comp, shaders/hiz_downsample.comp, shaders/mrt.vert, shaders/mrt.frag, src/Render/GeometrySystem.cpp
read-when: 改几何提交路径、加实例属性、调视锥/遮挡剔除、改合并缓冲、接粒子/体素等大批量实例、调试 indirect draw 或 set 3 描述符
size: ~160 行
updated: 2026-11-02
---

# GPU 驱动提交：实例缓冲、compute 剔除与间接绘制

F1 第一刀。目标不是「现在就更快」，而是把「CPU 逐实体决定画什么」换成「CPU 摊平数据、GPU 决定画什么」。当前场景只有 103 个实例，两条路径性能在噪声内相当；收益要等实例数上去才出现。

## 1. 边界

本刀做到：
- CPU 每帧把所有网格子网格摊平成 `InstanceRecord`（224 字节）写进 SSBO，不做任何可见性判断；
- 分组键是 **`(模型, 子网格)`**：同一子网格被多个实体复用时实例在缓冲里连续排列，一条命令带 `instanceCount = N` 画完，这才是真正的实例化（F1-a 是每子网格一条命令、instanceCount 恒为 1）；
- `scene_cull.comp` 做 6 平面视锥剔除 + Hi-Z 遮挡剔除，把结果写成间接命令（被剔除的组把 `instanceCount` 写成 0）；
- 全局合并顶点/索引缓冲（F1-b）：可批模型的几何拼进同一份 device-local 缓冲，一次绑定就能提交所有组；
- 合并模式下组按 `materialSlot` 排序，材质切换退化成指令缓存问题而不是绑定问题；
- 模型包围盒缓存以 `Model::getGeometryId()` 为键（不再用裸指针），旧条目按 600 帧不活跃淘汰。

仍未做：
- 跨模型合成**一条**命令（间接命令不带顶点缓冲绑定，要一条画完就得先有全局合并缓冲 + 统一 firstIndex 重写，见 §4.1 的取舍）；
- GPU 端 LOD 选择与屏幕空间误差；
- 粒子/体素接入（它们只需往实例缓冲里塞记录，不需要新管线）。

## 2. 数据流

```
CPU: registry.view<MeshComponent, WorldTransformComponent>()
       -> 收集可批实体 + batchModels（跳过 skybox/grid/terrain、非索引网格）
       -> mergedActive = gpuMergedBuffers && ensureMergedMesh(batchModels)
       -> 按 (model, subMeshIndex) 归组 -> 每组并集世界 AABB
       -> 排序（merged: materialSlot；否则 model 再 materialSlot）
       -> instanceScratch（224B/条，组内连续）+ GroupRecord（64B/组）
     instances / groups / commands / frameData / stats 全部是 SSBO
GPU: scene_cull.comp（每组一个 invocation）
       -> 视锥剔除 -> Hi-Z 遮挡剔除 -> 写 5 个 uint 的间接命令
Graphics: merged  -> 绑合并 VBO/IBO + 一条 vkCmdDrawIndexedIndirect(groupCount)
          非 merged -> 按模型连续区间逐段 bind + vkCmdDrawIndexedIndirect
```

关键取舍：**`firstInstance` 当全局实例下标**。间接绘制里 `firstInstance` 会成为 `gl_InstanceIndex` 的基值，顶点着色器可以直接用它索引实例缓冲，不需要额外 push constant 或额外描述符集；而 `instanceCount` 现在是组内真实实例数，一条命令画完同子网格的所有实体。

## 3. 缓冲与描述符

| 缓冲 | 用法 | 内容 / 内存 |
|---|---|---|
| instances | STORAGE，`instanceCapacity * 224` | host visible，每帧覆盖写 |
| groups | STORAGE，`groupCapacity * 64` | host visible，每帧覆盖写 |
| commands | STORAGE + INDIRECT，`groupCapacity * 5 * 4` | device local，compute 每帧重写 |
| frameData | STORAGE，256 字节 | viewProjection + view + 6 平面 + `vec4 screen` + params，host visible |
| stats | STORAGE + TRANSFER_DST，`2 * uvec4` | host visible，回读用 |

- **没有 counters 缓冲**：每个组只服务自己那 5 个 uint，compute 直接把 `instanceCount` 写成 0 或 N，不需要原子计数与 reset。
- 容量不足时 `vkDeviceWaitIdle` 后整体重建（翻倍），不做两代资源并行 —— 容量变化是低频事件。重建时 `createFrameBuffers()` 会先 `vkResetDescriptorPool`，否则二次分配直接失败。
- 图形侧只有一个额外布局：`set 3, binding 0`（readonly SSBO，VERTEX|FRAGMENT）。set 0/1/2 保持原样。
- 统计：`stats[0] = (submitted, visibleGroups, culledGroups, models)`、`stats[1] = (visibleTriangles, visibleVertices, occlusionTests, occlusionCulled)`。

## 4. 着色器

`scene_cull.comp`（`local_size_x = 64`，一次 dispatch 处理所有组）：

| binding | 内容 |
|---|---|
| 0 | `GroupRecord groups[]`（boundsMin, boundsMax, draw, counts） |
| 1 | `uint commands[]`（组内 5 个 uint，位于 `groupIndex * 5`） |
| 2 | `FrameData`（viewProjection + view + planes[6] + params） |
| 3 | `uvec4 stats[2]` |
| 4 | `sampler2D hizPyramid`（全 mip 链视图） |

push constant 是 `uvec4 params = (groupCount, hizWidth, hizHeight, mipLevels)`。

剔除分两步：
1. **视锥**：组并集世界 AABB 的 8 个角点变换后逐平面测 `dot(n, c) + d < -r`（半径取 AABB 外接球）。
2. **Hi-Z 遮挡**（`params.z != 0` 时才做）：8 角点投影求屏幕矩形与最近线性深度；按 `level = clamp(ceil(log2(max(rectPx))), 0, mips-1)` 选级，取矩形 4 角的 `textureLod` 最大值当「该区域最远已渲染深度」；`nearest > farthest + 0.05` 判定被遮挡。角点里有 `clip.w <= 1e-5`（跨近平面）直接不剔除，避免误杀。

被剔除的组把 5 个 uint 清零；可见的组写 `(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)`。

`mrt.vert` / `mrt.frag` 的 `gpuScene` 分支：非 0 时从实例缓冲取 `model`/`normal` 覆盖 push 的矩阵，片元用 `flat fragInstanceIndex` 取 flags/misc/uvTransform 覆盖材质参数（地形分支仍读 push）。

注意 push constant 尺寸：C++ 侧因 `glm::mat4` 的 16 字节对齐补齐到 **224** 字节，GLSL 侧块只有 212 字节。`static_assert` 用 `kMaterialOffset + 96`，push range 传 `sizeof` 是合法的。

## 4.1 合并缓冲（F1-b）

间接命令**不带顶点缓冲绑定**，所以一条命令只能服务同一份顶点/索引缓冲。F1-b 把可批模型的几何拼进一份全局缓冲：

| 步骤 | 做法 |
|---|---|
| 收集 | `buildInstances` 先扫一遍同条件的实体，得到去重的 `std::vector<const Model*>`（跳过 skybox/grid/terrain 与非索引网格） |
| 签名 | FNV-1a 覆盖每个模型的 `getGeometryId()` + 顶点数 + 索引数；签名不变直接复用 |
| 拼接 | 顶点与索引分别 `insert` 进两个 vector；**索引保持原样**，每个模型记 `{firstIndex, baseVertex}` |
| 上传 | device-local 缓冲 + staging + `cmdBeginSingleTime/cmdEndSingleTime`；屏障 `TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ | INDEX_READ` |
| 命令 | 组记录 `draw = (mergedFirstIndex + firstIndex, indexCount, baseVertex, firstInstance)`，`counts.z = 1` |
| 提交 | 绑合并 VBO/IBO + 一条 `vkCmdDrawIndexedIndirect(cmd, commands, 0, groupCount, 20)` |

`baseVertex` 通过 indirect 命令的 `vertexOffset` 施加，语义与 `vkCmdDrawIndexed` 的 vertexOffset 一致，所以索引缓冲不需要重写（Sponza 78.7 万索引，省掉一次全量加法）。

源数据来自 `Model` 新增的 CPU 副本（`getCpuVertices()/getCpuIndices()`）。地形模型保留 `isTerrainChunk` 子网格且 LOD + 裙边体积大，**不保留 CPU 副本**，也不参与合并；任一可批模型缺 CPU 几何时 `ensureMergedMesh` 返回 false，`useGpuPath()` 随之返回 false，整帧退回 **CPU 逐子网格路径**（`--no-merged-buffers` 可强制回退）。F1-d 起不再保留「逐模型 bind + 每组一条 indirect」的中间路径：跨模型 single draw 需要一份共享顶点/索引缓冲，两条 GPU 提交路径保留其一即可。

实测：`[SceneGpu] merged buffers built | vertices=192496 indices=786801 vertexMB=8 indexMB=3`；相机朝向 Sponza 时 `draw calls 2 / triangles 270459 / vertices 811377`，与 CPU 路径逐像素一致（`max diff = 0`）。

## 4.2 稳定帧快速路径（F1-d）

从 F1-a 起，`buildInstances()` 每帧都会重算 103 个子网格的世界矩阵与每组并集 AABB，静态场景下实测 **439µs/帧** —— 这是最后一块可以省掉的 CPU 开销。

做法是每帧只算一个 64 位「结构签名」（FNV-1a），覆盖所有影响实例记录的输入：

| 输入 | 为什么在签名里 |
|---|---|
| `entt::entity` | 增删实体要重排分组 |
| `WorldTransformComponent.sourceRevision` | 变换变了才需要重算矩阵与 AABB |
| `MaterialComponent.materialSetIndex` + `material` 指针 | 材质槽/棋盘格回退变了要重建记录 |
| `Model::getGeometryId()` + 子网格数 | 换模型或模型内容变化 |
| `BoundsComponent::getLocal()` 的 6 个 float | `checkerScale` 由它算出来 |
| `EngineSettings::gpuMergedBuffers` | 合并/非合并会改分组与命令写法 |

签名命中时跳过摊平与 `instances/groups` 上传，只写 256 字节的 `frameData`（viewProjection / view / 6 平面 / `vec4 screen` / params），然后照常 dispatch。每个帧槽位额外记 `(signature, capacityGeneration)`：容量增长会重建缓冲，等于两个槽位都要重传一次。命令缓冲不需要清 —— compute 每帧对每组整块重写（可见写命令，不可见写 0）。

实测（Sponza，103 组）：重建帧 `flattenUs=439`，稳定帧 `flattenUs=9`，日志 `stable=60/60 rebuilds=0`。

**改代码时注意**：新增任何会进入 `InstanceRecord` 的每实体字段（自定义颜色、LOD 偏置、可见性开关……），必须同步加进签名，否则会出现「场景变了但缓冲还是旧的」。
## 4.3 屏幕空间误差剔除（F1-e）

投影后包围球半径小于阈值的组直接丢弃，判据就是屏幕空间误差（SSE）的分子：`radius * (0.5 * 视口高 * P[1][1]) / 距离 < 阈值像素`。
这是 LOD 选择的前置件 —— 等模型有 LOD 链之后，同一个投影尺寸会用来「选级」而不是「剔除」。

`FrameData` 因此多了一个 `vec4 screen`：`x` = 阈值像素（0 = 关闭）、`y` = 视口高度（取 Hi-Z 的 extent 高）、`z` = 投影矩阵 `[1][1]`。
统计写在 `stats[2]`：`screenTests`（做过 SSE 测试的组数）、`screenCulled`（其中投影太小的组数）。

实测（相机 `position [0,6,45] / rotation [0,pi,0]`）：`--gpu-scene-min-px 400` 时 `screenTests=103 screenCulled=103`（整屏被剔空），阈值降到 2 时不再剔除；默认 0 = 关闭，行为与 F1-d 完全一致。

## 4.4 外部实例（F1-e）

粒子/体素这类没有 ECS 实体的几何，每帧通过 `SceneGpuBatch::setExternalInstances(std::vector<ExternalInstance>)` 提交：

```cpp
struct ExternalInstance
{
    const Model* model = nullptr;
    uint32_t subMeshIndex = 0;
    glm::mat4 transform{ 1.0f };
    uint32_t materialSlot = 0;
};
```

它们与 ECS 实例共用同一个分组键 `(模型, 子网格)`、同一份合并缓冲、同一套剔除与同一条 indirect 命令 —— 引擎侧不需要为粒子单独开绘制路径。三处配套改动：

- `batchModels` 收集时把外部实例的模型也算进去，合并缓冲必须覆盖它们；
- 结构签名混入外部实例的数量、几何 id、子网格、材质槽与 16 个矩阵 float（粒子在动，签名每帧都会变，这是预期行为，快速路径对它们不生效）；
- 调试注入：`--gpu-scene-external <n>` 把第一个可批子网格复制 n 份当外部实例，日志 `[SceneGpu] external instances | count=<n> source=debug-injection`，`cull stats` 里有 `external=<n>`。实测 `--gpu-scene-external 4` 时 `instances=107`（103 + 4）。

## 5. 管线接线

1. `GeometryPass::init()` 先建 `SceneGpuBatch`，再把 `getInstanceSetLayout()` 与裸指针交给 `GeometrySystem`（成员声明顺序保证 `SceneGpuBatch` 比 `GeometrySystem` 活得久）。随后对两个帧槽调用 `setHiZSource(frame, gBuffer.getPosition().view, extent)`。
2. `GeometryPass::addToGraph()` 依次注册：`TerrainGpuSelection`（无附件）→ `SceneGpuCull`（无附件，`prepare()`）→ `Geometry`（4 颜色 CLEAR + depth CLEAR）→ `HiZBuild`（无附件，读 `gbPosition`）。
3. `HiZBuild` 必须在 `Geometry` **之后**：金字塔要包含本帧刚画出来的几何；写到 `(frameInFlight + 1) % kFramesInFlight` 槽，供下一帧剔除读（错帧一帧，第一帧 `params.z = 0` 关闭遮挡）。
4. `GeometrySystem::render()` 里，普通网格在当前帧 `useGpuPath()` 为真时直接 `continue`，交给 GPU 批。
5. `GeometrySystem::render()` 开头（set 0/1/2 之后）**必须**再绑一次 set 3：所有图形管线都静态使用 set 3，走旧路径的地形/天空盒绘制也受同一条 VUID 约束，漏绑会报 `VUID-vkCmdDrawIndexedIndirectCount-None-08600`。
6. 提交块：push 一份 `gpuScene = 1` 的 `SimplePushConstantData`（矩阵留单位、材质无效），绑 `geometryPipeline`，`recordDraw()` 提交。

Hi-Z 资源归 `SceneGpuBatch` 自己管：`VK_FORMAT_R32_SFLOAT`、`SAMPLED | STORAGE`、`TILING_OPTIMAL`、**布局恒为 GENERAL**（`vkCmdPipelineBarrier` 手动切，不进 RenderGraph 跟踪），mip 数 `floor(log2(max(w,h))) + 1`，创建后立刻用 `cmdBeginSingleTime/cmdEndSingleTime` 做一次 UNDEFINED -> GENERAL 全链屏障。

resize 有两个坑，都已修：
- **不能遗留旧视图**：G-Buffer 重建会销毁旧 `VkImageView`，`destroyHiZ()` 必须 `hizPositionViews.fill(VK_NULL_HANDLE)`，否则重建描述符时把已失效的句柄写进描述符集，驱动直接崩（`--resize-test` 里表现为 access violation）。
- **录制期不要写描述符**：每帧 `vkUpdateDescriptorSets` 会让在录的命令缓冲失效（`UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkDescriptorSet`）。视图变化只在 `init()/resize()`（设备空闲）时写；尺寸变化时先 `vkDeviceWaitIdle` 再重建。

## 6. 开关与阈值

| 设置 | 默认 | 作用 |
|---|---|---|
| `gpuSceneEnabled` | true | 关掉后 `prepare()` 直接返回，完全走 CPU 路径 |
| `gpuSceneMinInstances` | 256 | 实例数低于阈值仍走 CPU 路径：几十条间接绘制比直接绘制慢 |
| `gpuSceneForce` | false | 忽略阈值，强制走 GPU 批（验证用） |
| `gpuMergedBuffers` | true | 合并顶点/索引缓冲；关掉等于整条 GPU 批退回 CPU 路径（跨模型 single draw 依赖它） |
| `hizEnabled` | true | Hi-Z 遮挡剔除；关掉只留视锥剔除 |
| `gpuSceneMinScreenPixels` | 0 | 屏幕空间误差剔除阈值（像素）；0 = 关闭 |
| `gpuSceneExternalDebugCount` | 0 | 调试注入：复制第一个可批子网格 n 份当外部实例 |

`SceneGpuBatch::useGpuPath(frameInfo)` 是唯一判定点：`available && gpuSceneEnabled && mergedActive && (gpuSceneForce || stats.submitted >= gpuSceneMinInstances)`。`mergedActive` 在 `prepare()` 里算好，合并缓冲建不起来就整帧回 CPU 路径，不会出现「命令缓冲空但实体被跳过」的情况。`stats.submitted` 在 `buildInstances()` 之后由 CPU 写入本帧真实条数（GPU 回读的 `submitted` 滞后两帧，不参与判定）。

命令行：`--gpu-scene` 强制、`--no-gpu-scene` 关闭、`--no-merged-buffers` 整条 GPU 批退回 CPU 路径、`--no-hiz` 关遮挡剔除、`--gpu-scene-min-px <像素>` 开屏幕空间误差剔除、`--gpu-scene-external <n>` 调试注入外部实例。

## 7. 遥测

- `readStats()` 读 host 可见统计缓冲前必须 `vmaInvalidateAllocation(...)`，否则永远读到 0（与 `GeometrySystemGpu.cpp:93` 同一坑）。
- 可见数/剔除数/图元数是 GPU 回读，**滞后两帧**；日志与面板要按「上一帧或更早」理解。
- 每 60 帧一行：`[SceneGpu] cull stats | groups=<n> visible=<n> culled=<n> instances=<n> hiz=<0/1> hizLevels=<n> hizEnabled=<0/1> hizTests=<n> hizCulled=<n> stable=<n>/60 rebuilds=<n> flattenUs=<n>`。`culled` 含视锥与遮挡，`hizTests/hizCulled` 单列遮挡剔除，`screenTests/screenCulled` 单列屏幕空间误差剔除，`external` 是本帧外部实例数；`stable/rebuilds/flattenUs` 是结构签名的命中率与 CPU 摊平时长（每 60 帧平均）。
- 一次性日志（ASCII，供 PowerShell 匹配）：`[SceneGpu] GPU batch ready | instancesPerFrame=.. groupsPerFrame=.. multiDrawIndirect=1`、`[SceneGpu] merged buffers built | ...`、`[SceneGpu] indirect batch | groups=.. instances=..`。

## 8. 验证

- 像素级等价：临时把相机放到 `position [0,6,45] / rotation [0,pi,0]`，分别出 `--gpu-scene` 与 `--no-gpu-scene` 截图，忽略 ImGui 面板区域后 `max diff = 0`。改动提交路径后应重做这一步。
- 回归：`f1-gpu-scene`、`f1b-merged-buffers`、`f1b-merged-off`（`--no-merged-buffers` 现在验证「退回 CPU 路径」）、`f1c-hiz-off`（要求 `hizEnabled=0`）、`f1d-stable-path`（要求 `stable=`/`rebuilds=`/`flattenUs=`）、`f1d-sync-validation`（`--validation-features sync` 下 `[Validation] errors=0 warnings=0`）、`f1e-screen-space`（要求 `screenTests=`）、`f1e-external-instances`（要求 `[SceneGpu] external instances | count=4` 与 `external=4`）。
- 同步校验（syncval）曾报 180 条 `SYNC-HAZARD-WRITE-AFTER-READ`：Hi-Z 图像被上一帧剔除 dispatch 采样过，本帧写之前必须等读完成 —— 起始屏障要用 `GENERAL→GENERAL` + `srcAccess = SHADER_READ`（原来的 `UNDEFINED` 起始屏障既丢弃内容又不与读同步）。逐级屏障要给下一级 `SHADER_READ`，否则降采样采样上一级时会报 `READ-AFTER-WRITE`。
- Hi-Z 管线自检：把 `kOcclusionBias` 临时改成 −100000，`hizTests=103 hizCulled=103`、`visible=0`，证明投影、选级、采样、比较全链路是通的；改回 0.05 后当前场景 `hizCulled=0`（原因见 §9）。
- 计时：本机 120 帧短跑波动可达 40 FPS，比较路径必须用 300 帧以上并多轮取中位；当前 103 实例下两条路径差异在噪声内。

## 9. 已知限制

- **Hi-Z 是组级保守剔除**：组内任一实例可见就整组可见。Sponza 的 103 个子网格各自覆盖整栋建筑的一大块（相机在走廊里时几乎每组都有一小部分可见），所以当前场景 `hizCulled = 0` 是正确结果而不是失效 —— 收益要等「大量小实例」（粒子、植被、体素 chunk）进来。
- 包围盒缓存以 `Model::getGeometryId()` 为键，超过 64 个或 600 帧不活跃会被淘汰；`forgetModel()` 保留但没有生产调用方。
- 合并缓冲按模型集合整体重建：模型增删或规模变化会触发一次全量重传（低频；大资产场景要观察重建耗时）。
- 合并缓冲要求可批模型带完整 CPU 顶点/索引副本（Sponza 约 11MB 常驻），地形模型不保留。
- 合并模式已经是一条 `vkCmdDrawIndexedIndirect` 画完所有组（跨模型 single draw 成立）；`--no-merged-buffers` 会整帧退回 CPU 路径。`
- 屏幕空间误差目前只用来「剔除」，还不是 LOD 选择：模型没有 LOD 链，等 F2/F6 有了 LOD 才会改成按误差选级。
- 外部实例与 ECS 实体共用 `(模型, 子网格)` 分组，所以同一子网格上不同材质的实例会归到同一组、用第一条记录的材质槽。
- 外部实例每帧变化会击穿结构签名，快速路径对粒子不生效（这正是应该的：数据确实变了）。
- 遥测滞后两帧，`drawCalls` 也是。

