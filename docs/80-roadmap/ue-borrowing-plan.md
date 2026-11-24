---
id: roadmap/ue-borrowing-plan
title: UE 借鉴改造总纲（分层 / 分类 / 分阶段）
type: roadmap
scope: 空（跨代码库的改造规划；候选落点覆盖 src/RHI、src/Render、src/Core、src/Scene、shaders、tools）
read-when: 决定下一项引擎基础改造做什么、判断某个 UE 子系统值不值得引入、或给某个 F 阶段排期之前
size: ~1051 行
updated: 2026-11-24
---

# UE 借鉴改造总纲

> 参照对象：`D:\work\Engine\UE`，UE 5.8.4（`Engine/Build/Build.version`：BranchName UE5，
> CompatibleChangelist 55116800）。`Engine/Source/Runtime` 下 194 个模块。
>
> 本文只回答三件事：**抄什么、按什么顺序抄、怎么证明抄对了**。
> 单项技术的历史尝试与判定见 [borrowed-techniques](borrowed-techniques.md)（TM-01~TM-19），
> 阶段目标与验收基线见 [roadmap](roadmap.md)，本文不重复它们的正文。

---

## 0. 结论先行

### 0.1 能抄的不是目录，是机制

UE 自己也不按"小而美"组织：`Renderer/Private/SceneRendering.cpp` 282KB、
`SceneRendering.h` 127KB、`ShadowSetup.cpp` 293KB、`NaniteCullRaster.cpp` 327KB、
`HLSLMaterialTranslator.cpp` 624KB。照抄它的目录结构，等于把 230 个源文件的引擎
拆成 500 个 200 行的小文件，每次改动横跨十个文件——那是把 UE 的**熵**抄过来，不是把它的**秩序**抄过来。

可以搬的是四类东西：

| 类别 | 例子 | 为什么能搬 |
|---|---|---|
| **不变量** | 资源必须先被写才能被读；屏障只由声明的访问态推导 | 与规模无关，小引擎同样受益 |
| **数据结构** | `FPackedCluster` 的位域、`FMeshDrawCommand` 的定长命令、页表 | 是编译期/运行期的布局决策，可独立验证 |
| **分层接口** | `ISceneViewExtension` 的 19 个钩子、`ITemporalUpscaler` | 把"加特性"从"改核心"变成"注册回调" |
| **分配策略** | 稀疏页 atlas、瞬态资源池、脏区间上传 | 直接对应显存/带宽数字 |

不能搬的是 UE 为**大团队并行开发**付出的成本：UObject 反射与 GC、RHI 线程与 RHI 命令重排、
Slate/UMG、Substrate 的材质组合爆炸、Lumen 的 surface cache + mesh card 全链路。
这些在单人/小团队规模下，收益低于维护成本。

### 0.2 你的引擎缺的不是"再来一套 UE"

现状（根 `README.md` 与 [decisions](../10-architecture/decisions.md) 已声明）：
线性 RenderGraph + 子资源语义校验 + 瞬时资源"必须有人写"硬校验、2 帧在飞 + timeline、
异步 compute 双队列（跨队列单向依赖推导 + 环检测）、bindless 材质、GPU 驱动提交（F1）、
SPIR-V 反射生成 `reflect.h`（TM-12）、80 项回归。

真正缺的是三件**结构性**的东西：

1. **图能表达"外部资源 + 多提交点"**。现在 `Renderer::endFrame` 每帧只 `vkQueueSubmit` 两次
   （`src/RHI/Renderer.cpp:214`，compute 在 256 行、graphics 在 306 行），顺序靠
   `graphicsWaitsForCompute` / `computeWaitsForGraphics` 两个布尔切换。这是"整帧一刀切"，
   不是"按 Pass 重叠"。`src/Render/ClusterBuildPass.h:11-16` 已经把代价写在注释里：
   聚簇表走 ScratchArena、没登记进图，跨队列依赖建立不起来，dispatch 只能挂图形流。
2. **场景对象在 GPU 上没有持久身份**。`src/Render/SceneGpuBatch.h` 每帧摊平
   `InstanceRecord`/`GroupRecord`，剔除粒度是"组并集 AABB"。没有 primitive id、没有逐实例剔除、
   没有跨帧复用——VSM 的页失效、Nanite 的 cluster culling、MegaLights 的逐灯剔除，
   全都要求"这个实例/这盏灯有一个稳定编号"。
3. **阴影与几何的可扩展性**。3 级 2048² CSM（`src/Render/ShadowPass.h:13-27`）在
   103 个子网格下够用，但阴影分辨率的物理上限由级联数决定；一旦要求"近处 1cm 级、
   远处 1m 级"，只能上虚拟阴影图。

### 0.3 三档性价比

| 档位 | 内容 | 判断依据 |
|---|---|---|
| **立刻做**（1~3 月） | 外部资源与多提交、Pass 剔除、跨帧 RT 池、Blackboard、持久 GPUScene、MeshDrawCommand、CVar/Stats | 与现有代码同构；不引第三方；每项都有可量化的 FPS/带宽/绘制数指标 |
| **计划做**（3~9 月） | 虚拟阴影图、距离场、速度缓冲 + TSR 启发式、meshlet | 需要新子系统，但接口边界清楚，可整体回退 |
| **不做**（明确写下理由） | Lumen 全量、Substrate 全量、Nanite 全量流式、UObject/GC、RHI 线程、Slate | 见 §8 |

---

## 1. 分层模型（L0~L6）

层与层之间只有一个方向：**上层依赖下层的接口，下层不知道上层存在**。
这条规则比"分成几层"更重要——UE 的 `RenderCore`（RDG）不认识 `Renderer` 的任何 Pass，
所以 `Renderer` 才能长到 261 个文件还改得动。

| 层 | 关注点 | UE 对应 | 你的现状 | 本层改造目标 |
|---|---|---|---|---|
| **L0 设备与内存** | 命令缓冲、队列、显存分配、屏障原语、PSO | `RHI/`、`RHICore/`、`D3D12RHI/`、`VulkanRHI/` | `src/RHI/Device.*`、`Texture.*`、`Pipeline.*`、VMA、双队列 + timeline、`ScratchArena` | 外部资源注册、瞬态显存分配器、屏障批处理、PSO 预热与缺失计数 |
| **L1 帧图** | 资源生命周期、Pass 调度、跨队列依赖 | `RenderCore`（RDG） | `src/RHI/RenderGraph.*`，线性、声明式屏障、子资源校验 | Pass 剔除、跨帧池化、Blackboard、多提交点、Pass 参数块 |
| **L2 场景与可见性** | 场景 → GPU 数据的持久表示、剔除 | `Renderer/GPUScene.*`、`InstanceCulling/`、`SceneCulling/` | `src/Scene/*`、`src/Render/SceneGpuBatch.*` | 持久 GPUScene、逐实例剔除、压缩可见 ID、每视图 indirect args |
| **L3 绘制与管线** | 绘制命令、状态排序、PSO 生命周期 | `MeshPassProcessor.*`、`PSOPrecache.*` | 每 Pass 自持 `Pipeline`，逐对象写 descriptor | 定长 `MeshDrawCommand` + 排序键 + 状态缓存 + PSO 预热 |
| **L4 几何** | 细节层级、光栅化、材质解析 | `Nanite/` | 合并网格（按签名缓存）+ 组级 LOD | meshlet 层级、GPU 裁剪、可见性缓冲、材质解析 |
| **L5 光照与阴影** | 阴影、光照分配、全局光照 | `VirtualShadowMaps/`、`LightGridInjection.cpp`、`GlobalDistanceField.cpp`、`Lumen/` | 3 级 CSM、16x9x24 聚簇、IBL、SSAO、SSR | VSM、稀疏光照网格、距离场遮蔽、天光大气 |
| **L6 成像与工具** | 时域、色调、可观测性、扩展点 | `PostProcess/`、`IConsoleManager.h`、`Stats.h`、`SceneViewExtension.h` | AutoExposure、TAA、Bloom、云/雾/水、GpuProfiler、回归 | 速度缓冲、TSR 启发式、CVar、stat 注册表、ViewExtension、截图金标 |

**分类与分层的区别**：分层是"代码依赖谁"（§1），分类是"这次改动属于哪种机制"（§2），
阶段是"什么时候做"（§3）。三者交叉，但不要混为一谈——同一个阶段会同时碰三层，
同一层也会被多个阶段改。

---
## 2. 分类借鉴清单（按机制，不按模块）

每一条给四段：**UE 证据**（路径 + 符号 + 行号）、**你的现状**、**借鉴方式**、**明确不抄**。
行号均相对 `D:\work\Engine\UE\Engine\Source\Runtime\`，用于回查；不要把它们抄进注释里，UE 版本一换就失效。

### 2.1 资源状态与屏障

**UE 证据**：`RHI/Public/RHIAccess.h:10` 的 `ERHIAccess` 把「资源现在处于什么访问态」变成类型，
19 个取值含 `IndirectArgs`、`ResolveDst`、`DSVRead`、`Discard=1<<18`，并提供
`IsReadOnlyExclusiveAccess` / `IsWritableAccess` / `IsInvalidAccess` 判定；RDG 侧把同源同目标的屏障合并成批。

**你的现状**：`src/RHI/RenderGraph.h:148 Access`、`:173 BufferAccess`、`:102 SubresourceRange`
已经做到子资源粒度，`:442 emitBarrierIfNeeded` / `:443 emitBufferBarrierIfNeeded` 逐资源发射。

**借鉴方式**：把 `Access` 的语义从「layout + stage + access 掩码」升级为「访问态」，
先补你实际用到但没建模的四种：`IndirectArgs`（间接绘制缓冲）、`ResolveDst`（MSAA 解析目标）、
`Discard`（清空整图，可跳过读屏障）、`Present`。然后按 `(srcStageMask, dstStageMask)` 分组，
一次 `vkCmdPipelineBarrier2` 提交多条——不是为了好看，是为了让驱动能合并 barrier，减少 GPU 空泡。

**不抄**：19 态全量、`FRHIAccess` 的位运算辅助全家桶。先补 12 种，多一种等它真的出现在代码里再加。

### 2.2 外部资源与多提交点（最高优先级）

**UE 证据**：`RenderCore/Public/RenderGraphUtils.h:1261` 的 `FRDGExternalAccessQueue`
专门记录「图外/持久资源在本帧被怎么用」，提交前统一补屏障；
`Renderer/Private/GPUScene.h:233 BeginRender` / `:235 EndRender` 划出「这一帧 GPUScene 的图内活跃区间」；
`RenderGraphDefinitions.h:129` 的 `ERDGPassFlags::AsyncCompute` 让 Pass 自己声明去哪个队列。

**你的现状**：`importTexture`（`src/RHI/RenderGraph.h:332`）与 `importBuffer`（`:336`）
只是「把外部 handle 借进图」，没有记录「本帧谁读过它、提交前要补什么」。
后果写在 `src/Render/ClusterBuildPass.h:11-16`：聚簇三张表走 ScratchArena 普通 buffer、没进图，
跨队列 timeline 依赖建立不起来，`addComputePass` 会把 dispatch 录进异步流却无人等它，所以只能挂图形流。
`src/RHI/Renderer.cpp:214 endFrame(bool graphicsWaitsForCompute, bool computeWaitsForGraphics)`
每帧两次 `vkQueueSubmit`（compute 在 256 行、graphics 在 306 行），重叠关系只能整帧二选一。

**借鉴方式**：分两步，第一步不改提交结构就能拿到收益。

1. `ExternalResourceRegistry`：帧首 `acquire(handle, ERHIAccess)`，帧尾 `release()`；
   提交前把 acquire 屏障集中写进命令缓冲头部（graphics 一次、compute 一次）。
   这一步做完，`ClusterBuildPass` 就可以合法地改回 `addComputePass`。
2. 多提交点：把一帧拆成 `G0 -> C0 -> G1`。`G0` 只含「C0 的输入准备」，`C0` 是异步 compute 批，
   `G1` 是其余。C0 与 G1 之间用 timeline wait/signal 连接。

**注意 TM-09 的结论不适用于此**：TM-09 判定的是「EXCLUSIVE 所有权转移与单提交结构冲突」。
队列重叠需要的是**多提交**，不是**所有权转移**——在 CONCURRENT 共享模式下，
`queueFamilyIndex` 不变、无需 ownership transfer，只要在 C0 前后各插一次信号即可。
这条是本文对 TM-09 的补充，不是推翻。

**不抄**：UE 的 RHI 线程、RHI 命令重排、多 GPU 的 `FRHIGPUMask`。

### 2.3 瞬态资源与池化

**UE 证据**：`RHI/Public/RHITransientResourceAllocator.h`（按生命周期把多个资源放进同一块显存）；
`RenderCore/Private/RenderGraphResourcePool.h`（图内 RT 池）；
`RenderGraphBuilder.h:345-346 QueueTextureExtraction(FRDGTextureRef, TRefCountPtr<IPooledRenderTarget>*, ...)`
（把图内纹理「提升」成跨帧存活的对象）。

**你的现状**：TM-08 的结论是「图自持 8 个目标 40.8MiB、可省 0；导入目标 181.9MiB 不归图管」，
因此判定不做真别名化。这个结论对「别名」成立，但它同时暴露了根因：**大头根本不在图里**。

**借鉴方式**：把顺序反过来。

1. 先做**跨帧 RT 池**：键 `(format, extent, usage, samples, mipLevels)`，
   命中则复用、未命中才 `vkCreateImage`。收益与别名无关，是「不再每帧 create/destroy + 不再每帧绑内存」。
2. 再把 GBuffer、深度、Bloom 金字塔改成图持有并进池。此时 TM-08 的「可省 0」才会变成「有对象可省」。
3. 最后才谈别名化。

**不抄**：LAZILY_ALLOCATED（TM-18 实测本机该内存类型 0 个，写了也是死代码）；
为别名放宽格式兼容规则。

### 2.4 Pass 剔除

**UE 证据**：`RenderCore/Private/RenderGraphPrivate.h:85 GRDGCullPasses`（默认开启）、
`:166 STAT_RDG_PassCullCount`（剔除数进 stat）、`ERDGPassFlags::NeverCull` / `Readback` 作为不可剔除的根。

**你的现状**：`src/RHI/RenderGraph.h:372 setPassEnabled` 是**人手开关**（`--no-bloom` 之类），
`PassSwitchboard` 按 Pass 名前缀/精确匹配；这不是依赖驱动的剔除。

**借鉴方式**：帧末做一次反向遍历——从「有外部消费者」的资源（present 目标、readback、
`QueueTextureExtraction` 的产物）出发标记生产者 Pass 存活；再补一条：被 `NeverCull` 标记的 Pass 永远存活。
剔除完成后用你已有的「瞬时资源必须有人写」校验做 dry-run：如果某个 Pass 被删掉导致某资源无人写，
说明存活性分析错了，直接抛异常（这条校验现在是资产，别浪费）。

**不抄**：并行编译/执行（TM-10 实测并行准备比串行慢 4.9~5.8 倍；Pass 数量 21，还不到收益门槛）。

### 2.5 Blackboard

**UE 证据**：`RenderCore/Public/RenderGraphBlackboard.h:56` 的 `class FRDGBlackboard`，
按类型存取、帧末清空，Pass 之间传「不该进全局 UBO 的中间产物」用它。

**你的现状**：`FrameInfo&` 一路往下传（`src/Core/FrameInfo.h` 24.5KB，含 100 盏点光、
3 张阴影矩阵、雾/云/聚簇参数），任何新 Pass 想拿点上游数据，只能往这个结构里加字段。

**借鉴方式**：把「某个 Pass 的中间产物」搬进 Blackboard：
SSAO 输出、Hi-Z 金字塔、TAA 历史、聚簇表句柄、本帧的可见实例列表。
`FrameInfo` 只保留「跨 Pass 的稳定契约」（VP、相机、光照、阴影矩阵）。
判据：如果某个字段只被一个生产者写、一个消费者读，它就不该在 `FrameInfo` 里。

**不抄**：`FRDGBlackboard` 的 `TUniquePtr` 内存池细节；用 `std::any` + `type_index` 就够。

### 2.6 GPUScene（持久场景数据）

**UE 证据**：`Renderer/Private/GPUScene.h`：
`AllocateInstanceSceneDataSlots(FPersistentPrimitiveIndex, int32)` 244、
`Update(FRDGBuilder&, FRDGExternalAccessQueue&, ...)` 267、
`UploadGeneral(...)` 485、`UpdateGPULights(...)` 490、
`AddUpdatePrimitiveIdsPass(...)` 498、`AddClearInstancesPass(...)` 500、
`GetNumInstances/GetNumPrimitives/GetMaxLightId` 286-288、
`InstanceDataTileSizeLog2/TileSize` 377-380（实例数据分块，便于只上传脏块）、
`SetPrimitiveData(FPrimitiveSceneProxy*, const FPersistentPrimitiveIndex&, uint32)` 601。

**你的现状**：`src/Render/SceneGpuBatch.h` 每帧把场景摊平成 `InstanceRecord` / `GroupRecord`，
剔除粒度是「组并集 AABB」，Hi-Z 用上一帧 G-Buffer 重建、滞后两帧。

**借鉴方式**：三张持久表，代替每帧摊平。

| 表 | 内容 | 更新时机 |
|---|---|---|
| `primitiveBuffer` | 材质槽、索引缓冲、包围盒、LOD 误差、投影类型 | 静态，资产加载时写一次 |
| `instanceBuffer` | 世界矩阵、法线矩阵、primitiveId、flags、上一帧矩阵（算速度用） | 脏区间上传，按 tile 分块 |
| `visibleIdList` | 剔除后的压缩 id 列表 + 每视图 indirect args | 每帧 compute 重写 |

逐实例剔除替代组级剔除；`visibleIdList` 的容量按「上次的 1.25 倍」增长，避免每帧重分配。

**不抄**：`FGPUSceneDynamicContext` 那套多线程 Upload 任务编排（先单线程 + 脏区间，等真的卡了再说）。

### 2.7 绘制命令与状态排序

**UE 证据**：`Renderer/Public/MeshPassProcessor.h:1281` 的 `class FMeshDrawCommand`，
字段就是「画一次需要的最小状态」：`ShaderBindings`、`VertexStreams`、`IndexBuffer`、
`CachedPipelineId`、`FirstIndex`/`NumPrimitives`/`NumInstances`、`VertexParams` 与 `IndirectArgs` 的 union、
`PrimitiveIdStreamIndex`、`StencilRef`、`PrimitiveType`；
`MatchesForDynamicInstancing` 1319 用「PSO + StencilRef + 绑定 + 顶点流 + 索引缓冲 + 区间」判断两条命令能否合并；
`FMeshDrawCommandSortKey` 1536 是排序键；`FMeshDrawCommandStateCache` 905 只记录「已经绑过什么」，避免重复绑定。

**你的现状**：每个 Pass 自持 `Pipeline` 对象，逐对象写 descriptor；F1 之后 draw calls 已经压到 2
（合并缓冲 + 一条 indirect），但**状态切换没有缓存，也没有排序键**。

**借鉴方式**：定义一个 POD：

```cpp
struct MeshDrawCommand {
    uint32_t pipelineId;
    uint32_t materialSlot;
    VkBuffer indexBuffer;
    uint32_t firstIndex, numPrimitives, instanceOffset;
    uint64_t sortKey;   // (pipelineId << 32) | (depth << 16) | materialSlot
};
```

按 `sortKey` 分桶排序；每条命令执行前用 `stateCache.apply(cmd)`，只绑变化项。
这一步在立方体数量低时看不出收益，但在 VSM（每帧几十个页 Pass）和 meshlet 光栅化下是**必须**的。

**不抄**：`TChunkedArray`、`FGraphicsMinimalPipelineStateId` 的全局 PSO 表（等你真的有几万条命令再说）。

---

### 2.8 PSO 生命周期与预热

**UE 证据**：`Engine/Public/PSOPrecache.h` + `Engine/Private/PSOPrecache.cpp` /
`PSOPrecacheMaterial.cpp`。UE 的做法是「资产加载期按 usage mask 预建 + 运行时缺失计数」，
不是「第一次画的时候懒建」。

**你的现状**：TM-06 判定不做按需建管线（30 条图形管线只有 26 种状态，冷启动 50.5ms、热 24.7ms）。

**借鉴方式**：预热的判据不是**启动时间**，是**首帧 hitch**。26 种状态确实不需要预热；
但 P4 的 VSM 会把管线数抬到数百（页尺寸 × 光源类型 × 材质特性 × 是否双面），
那时第一个进入新页的物体会在同一帧里编译 3~5 条管线，就是可见的卡顿。

落地形状：

```cpp
struct PSOCacheKey { uint32_t passId, featureMask, vertexLayoutId, rtFormatHash; uint8_t samples; };
```

启动后延迟一帧，在 `JobSystem` 上按 key 批量建；缺失计数进 stat，同一 key 连续两帧缺失就打 warning。

**不抄**：`FPSOPrecacheParams` 的 usage mask 全套、异步编译的 `FRHIPipelineStateCache` 回调链。

### 2.9 虚拟阴影图（VSM）

**UE 证据**：`Renderer/Private/VirtualShadowMaps/VirtualShadowMapArray.h`：
`FVirtualShadowMap` 67、`PageSize = VSM_PAGE_SIZE` 73、`NumHZBLevels = Log2PageSize` 86、
`FVirtualShadowMapProjectionShaderData` 100、`MaxPhysicalPages` 160/368、`BeginMarkPages(...)` 378；
`VirtualShadowMapCacheManager.cpp`（102KB）负责页的分配/失效/回收；
`VirtualShadowMapClipmap.cpp:200` 构造、`:490 ComputeBoundingVolumes(const FVector)`、
`:640 UpdateCachedFrameData()`。物理图集 + 页表 + 每页 HZB 是它全部的秘密。

**你的现状**：3 级 CSM、2048²、CPU 逐实体提交、阴影图与采样器自己管、手工屏障、不进渲染图
（`src/Render/ShadowPass.h:13-27`）。

**借鉴方式**：页池 + 页表 + 每页 HZB，三件东西。

| 组件 | 参数（建议初值） | 说明 |
|---|---|---|
| 物理图集 | 8192²、128² 页、4096 页 | 与 CSM 同量级显存，但密度按需分配 |
| 页表 | 每级 clipmap 一张 `32x32` 页表 | 存页索引 + 是否已渲染 |
| 每页 HZB | 7 级（`log2(128)`） | 页内遮挡剔除，避免重复画被遮住的三角形 |
| clipmap | 3 级，沿视线方向滚动，页对齐 | 近处 1 页 ≈ 数厘米，远处 1 页 ≈ 数十厘米 |

页的标记（mark）由「本帧投影到该页的实例」决定；未被标记的页**直接复用上帧内容**——
这就是 VSM 比 CSM 省的地方：静态物体不重画。失效条件只有三条：光源变换、实例变换/变形、
材质 alpha 模式变化。

阴影几何复用 P2 的逐实例剔除结果（`visibleIdList`），不再 CPU 逐实体提交。

**不抄**：Nanite 专用的页标记路径（需要 P6 的 cluster 数据）、VSM 与虚拟纹理的双路径合并。

### 2.10 光照网格与灯数据

**UE 证据**：`Renderer/Private/LightGridInjection.cpp`：`FLightGridInjectionCS` 435、
`FLightGridInjectionSpecConstCS` 547、`struct FLightGrid` 822、`FLightGrid LightGridInjection(...)` 828、
`FLightGridSceneExtensionUpdater` 349（挂在场景更新链上，不占主渲染函数的代码）。

**你的现状**：16x9x24 固定网格、每簇 256 上限（TM-13 实测 GPU 建簇 65.37 FPS）；
灯数据在 `GlobalUbo.pointLights[100]`（`src/Core/FrameInfo.h`），所以灯数硬上限 100。

**借鉴方式**：两件事，互相独立。

1. **灯数据搬家**：`pointLights[100]` 从 UBO 搬进 GPUScene 的 `lightBuffer`，簇里只存 16bit 的 `lightId`。
   UBO 变成「灯数 + 缓冲句柄」。上限从 100 变成「缓冲能装多少」。
2. **稀疏网格**：`FLightGrid` 的本质是「每格 `offset + count` 指向一个全局 index 数组」，
   没有 per-cell 固定容量。把 256 上限换成「全局池 + 溢出计 stat」，深度切片改成对数分布。

**不抄**：light function atlas、MegaLights 的逐像素随机采样（P7 再评估）。

### 2.11 全局距离场

**UE 证据**：`Renderer/Private/GlobalDistanceField.cpp`：
`FGlobalDistanceFieldInfo::UpdateParameterData(float, bool, float, FVector)` 578、
`FCullObjectsToClipmapCS` 1466、`FCullObjectsToGridCS` 1581、
`FCompositeObjectsIntoGridPagesCS` 1640、`FCompositeObjectsIntoPagesCS` 1682、
`FInitPageFreeListCS` 1727、`FAllocatePagesCS` 1757。
稀疏页 atlas + 空闲链表 + 间接参数，与 VSM 是同一套模式。

**你的现状**：全仓 0 命中（`distanceField|sdf` 搜索无结果）。

**借鉴方式**：不要一上来做 128³ 全量。先做「相机周围 3 级 clipmap、每级 64³、页 8³」，
用途按性价比排序：

1. 天空光遮蔽（补 SSAO 在远距离的失效：SSAO 只有屏幕空间，屏幕外的遮挡物看不见）；
2. 软阴影的接触硬化（接触处锐利、远处模糊，由距离场梯度决定）；
3. 廉价 AO 补充（与 SSAO 取 max 而不是相加）。

页分配器与 VSM 共用（同为「稀疏页 + 空闲链表 + 间接参数」），这是本项最大的复用点：
写一次页分配器，VSM 与距离场各用一份，比写两套便宜一半。

**不抄**：Lumen 的 mesh card、surface cache、radiosity（这三个是 Lumen 的主体，成本远超收益）。

### 2.12 材质与着色器

**UE 证据**：`Engine/Public/MaterialShared.h:1630 class FMaterialShaderMap`、
`:2169 class FMaterial`；`Engine/Private/Materials/HLSLMaterialTranslator.cpp` 624KB（节点图 → HLSL 翻译器）；
`RenderCore/Public/ShaderParameterStruct.h` 的 `BEGIN_SHADER_PARAMETER_STRUCT` + `FShaderParametersMetadata`
在启动时校验 C++ 结构体与着色器参数布局是否一致。

**你的现状**：`src/Core/Material.h` 是 5 张贴图 + 标量 + `shaderFlags`，无变体、无参数块自动生成；
但 TM-12 已经有「SPIR-V 反射 → `build/shaders/<name>.reflect.h` + 6 条 static_assert」的机制。

**借鉴方式**：分两级，**不要一步走到节点图**。

1. **参数块自动化**（纯收益，无需设计）：把 TM-12 从 SSAO 扩到全部 Pass——反射出 `struct XxxParams`、
   `static_assert(offsetof(...) == N)`、`sizeof`，C++ 与 GLSL 共用反射结果作为唯一真值。
   现在 52 个 shader 里有几个是手写 `struct` 对齐的，就是漂移点。
2. **特性开关 → 变体键**：`ShaderVariantKey = (passId, featureMask)`，`featureMask` 是位集
   （normalMap / alphaTest / emissive / skinning / vertexColor / ...）。启动时建一遍用到的键，
   缺失时懒建并计数。不要引入 `#include` 预处理式的无限组合。

**不抄**：节点图 + HLSL 翻译器。成本约等于写一个编译器，收益只有在「面向美术的材质编辑器」成立时才兑现。

### 2.13 成像链（速度缓冲与 TSR 启发式）

**UE 证据**：`Renderer/Private/PostProcess/TemporalSuperResolution.cpp` 13 个 Pass：
`FTSRShader` 504、`FTSRConvolutionNetworkShader` 645、`FTSRMeasureFlickeringLumaCS` 744、
`FTSRMeasureThinGeometryCoverageCS` 787、`FTSRClearPrevTexturesCS` 814、`FTSRDilateVelocityCS` 826、
`FTSRDecimateHistoryCS` 884、`FTSRRejectShadingCS` 958、`FTSRDetectThinGeometryCS` 1080、
`FTSRWeightRelaxationCS` 1166、`FTSRSpatialAntiAliasingCS` 1233、`FTSRUpdateHistoryCS` 1270；
抽象接口在 `Renderer/Public/TemporalUpscaler.h`。

**你的现状**：`src/Render/TAAPass.h:19-24` 是相机重投影 + 3x3 邻域 AABB clamp；
没有逐物体运动矢量；`src/Core/FrameInfo.h:288` 的注释说明运动模糊也是靠重投影。

**借鉴方式**：真正的台阶是 **velocity buffer**，不是换算法。分四步，每步独立可验收：

1. G-Buffer 加一张 `RG16F velocity`：`velocity = project(prevVP * prevWorld) - project(VP * world)`，存 NDC 差分；
2. TAA 改用 velocity 重投影（替代「用世界位置反投影」），颜色空间换 YCoCg 做方差裁剪；
3. 加「薄几何检测」（邻域深度差超阈值时降历史权重）与「去遮挡检测」（velocity 与邻域中位数偏差大时拒斥）；
4. 最后才评估要不要 CNN 校正（`FTSRConvolutionNetworkShader:645` 是 5 层小网络，需要独立的推理路径）。

第 1 步还有一个额外收益：运动模糊从「全屏相机速度」变成「逐物体速度」，拖影才不会糊成一片。

**不抄**：完整 TSR 网络权重与训练流程；`FTSRConvolutionNetworkShader` 在 P5 阶段只做接口预留，不实现。

### 2.14 视图与扩展点

**UE 证据**：`Engine/Public/SceneViewExtension.h` 的 `ISceneViewExtension`，19 个钩子：
`SetupViewFamily` 145、`SetupView` 150、`BeginRenderViewFamily` 165、
`PreRenderViewFamily_RenderThread` 175、`PreRenderView_RenderThread` 180、
`PreRenderBasePass_RenderThread` 190、`PostRenderBasePassDeferred_RenderThread` 195、
`PrePostProcessPass_RenderThread` 210、`SubscribeToPostProcessingPass` 222、
`PostRenderViewFamily_RenderThread` 227、`GetPriority` 237、`IsActiveThisFrame` 242。

**你的现状**：单相机、Pass 注册表固定（`src/Render/RenderPipeline.h` 的 `addPass` 顺序即执行顺序），
新特性只能改 `RenderPipeline::registerAll` 与 `src/Core/Application.cpp`（154KB）。

**借鉴方式**：一个 `IViewExtension` + **6 个钩子**（不要 19 个）：

```cpp
struct IViewExtension {
    virtual void setupView(ViewInfo&) {}
    virtual void beginFrame(RenderGraph&, Blackboard&) {}
    virtual void prePass(RenderGraph&, const char* passName) {}
    virtual void postPass(RenderGraph&, const char* passName) {}
    virtual void prePostProcess(RenderGraph&, Blackboard&) {}
    virtual void endFrame() {}
    virtual int  priority() const { return 0; }
};
```

外加 `subscribePostProcess(stage, callback, priority)`：让太阳光晕、Bloom、云这类「往链里插一段」
的东西不再修改 `registerAll`。副产品是多视图（编辑器缩略图、反射捕捉、分屏）不需要改架构。

**不抄**：19 个钩子全量、`ESceneViewExtensionFlags`、`FSceneViewFamily` 的 60 个字段。

### 2.15 工程化：CVar 与 stat 注册表

**UE 证据**：`Core/Public/HAL/IConsoleManager.h:577 class IConsoleVariable`、
`:1969` 与 `:2134` 的 `class TAutoConsoleVariable`；`Core/Public/Stats/Stats.h:130 DECLARE_CYCLE_STAT`、
`:139 DECLARE_DWORD_COUNTER_STAT`、`:171 DECLARE_CYCLE_STAT_EXTERN`。

**你的现状**：命令行 flag（`--no-hiz`、`--no-ssr`、`--no-merged-buffers` 等）+ `GpuProfiler` + 80 项回归；
没有运行期可改的开关，也没有 stat 注册表。

**借鉴方式**：

1. `CVar<T>` 声明式注册：名字、默认值、范围、说明；控制台（ImGui 面板）与命令行共用一份定义。
   现在命令行 flag 与 `EngineConfig` 是两套里外不一的开关，CVar 能收口。
2. stat 注册表：Pass 名自动变成计时项；`STAT_RDG_PassCullCount` 那类计数器直接进 HUD。
   UE 的 `SCOPE_CYCLE_COUNTER` 本质是 RAII，你已经有 `GpuTimer`，加一层宏即可。

**不抄**：stats 层级（group/hierarchy）、CSV profiler、trace 导出。

---

### 2.16 小结：优先级矩阵

| 机制 | 工作量 | 前置 | 收益量级 | 建议阶段 |
|---|---|---|---|---|
| 外部资源 + 多提交点（2.2） | 中 | 无 | 高（解锁异步 compute 与多阶段特性） | P1 |
| 跨帧 RT 池（2.3） | 小 | 无 | 中（消除每帧 create/destroy） | P1 |
| Pass 剔除（2.4） | 小 | 2.2 | 中（关掉的 Pass 真不执行） | P1 |
| Blackboard（2.5） | 小 | 2.2 | 中（`FrameInfo` 不再膨胀） | P1 |
| 持久 GPUScene（2.6） | 大 | 2.2 | 高（逐实例剔除、VSM/meshlet 的前置） | P2 |
| 绘制命令 + 状态缓存（2.7） | 中 | 2.6 | 中（VSM 下从「中」变「高」） | P3 |
| PSO 预热（2.8） | 小 | 2.7 | 中（只在管线数上百后显现） | P3/P4 |
| VSM（2.9） | 大 | 2.2、2.6 | 高（阴影分辨率与距离解耦） | P4 |
| 光照网格升级（2.10） | 中 | 2.6 | 中（灯数上限从 100 解除） | P4/P7 |
| 距离场（2.11） | 大 | 页分配器 | 中（远距离遮蔽、接触硬化） | P7 |
| 材质参数块自动化（2.12-1） | 小 | TM-12 | 中（消除布局漂移） | P1/P3 |
| 着色器变体键（2.12-2） | 中 | 2.12-1 | 中（VSM/meshlet 的材质组合爆炸） | P4 |
| 速度缓冲 + TSR 启发式（2.13） | 中 | 2.6（要上帧世界矩阵） | 高（TAA 质量与运动模糊正确性） | P5 |
| ViewExtension（2.14） | 中 | 2.5 | 中（加特性不再改核心） | P5/P8 |
| CVar + stat 注册表（2.15） | 小 | 无 | 中（可观测性） | P8 |

---

## 3. 分阶段路线图（P0~P8）

排序原则：**先修「表达能力」，再修「数据规模」，最后修「画质特性」**。
反过来做（先上 VSM 或 meshlet）会在 P1/P2 上返工两遍——因为那些特性要求的
「图外资源 + 多提交 + 逐实例 id」现在都不存在。

总工期按单人 + LLM 协作估算，单位为人日；每个阶段都带独立回退开关，任何阶段失败都不影响基线。

| 阶段 | 主题 | 前置 | 工期 | 一句话收益 |
|---|---|---|---|---|
| P0 | 度量基线 | 无 | 2 | 后面每个数字都有对照 |
| P1 | 帧图 RDG 化 | P0 | 14 | 异步 compute 真正重叠；Pass 可被剔除 |
| P2 | 持久 GPUScene | P0、P1 | 12 | 逐实例剔除；场景规模脱钩 |
| P3 | 绘制命令与 PSO | P2 | 8 | 状态切换可缓存；为 VSM 铺路 |
| P4 | 虚拟阴影图 | P1、P2 | 20 | 阴影分辨率与距离解耦 |
| P5 | 成像链 | P2、P4 | 12 | 速度缓冲 + TSR 启发式 |
| P6 | meshlet 几何 | P2、P3 | 22 | 几何细节不再受 draw 数限制 |
| P7 | 光照扩展 | P4、P5 | 18 | 距离场、天光、灯数上限解除 |
| P8 | 工具与工程化 | 无（可穿插） | 持续 | 加特性不改核心 |

依赖关系（`->` 表示「必须先有」）：

```
P0 -> P1 -> P2 -> P3 -> P6
       |     |     |
       |     +-----+-> P4 -> P5 -> P7
       |                 |
       +-----------------+
```

P3 与 P4 可以并行：P3 改的是「怎么发绘制命令」，P4 改的是「往哪画、画多大」，
两者只在 `visibleIdList` 的消费端耦合。

---

### P0 度量基线（2 人日）

**目标**：把后面每个阶段的收益变成可比较的数字，而不是「感觉快了」。

**任务**

| # | 内容 | 落点 |
|---|---|---|
| P0-1（已落地） | 固化 3 个基准场景：`default`（固定开销）、`voxel-world`（`--voxel-world 1`，几何压力）、`lights-512`（512 灯 + GPU 建簇，光照压力） | `tools/bench.ps1` 场景表 |
| P0-2（已落地，本轮补的四项） | 帧时间分布（中位 / p95 / 最长）、资源创建计数（`images/frame` 与启动峰值）、显存实际占用（`vmaGetHeapBudgets`）、剔除汇总（`visibleRatio`）。各 Pass GPU 时间仍走既有的 `--gpu-timing` CSV | `src/RHI/FrameStats.*`、`src/RHI/ResourceStats.*`、`src/Render/SceneGpuBatch.cpp` |
| P0-3 | 新增两个本轮才需要的计数器：**barrier 条数**（每次 `emitBarrierIfNeeded` 计数）、**状态切换次数**（每次 `vkCmdBindPipeline` + `vkCmdBindDescriptorSets` 计数） | `src/RHI/RenderGraph.*`、`src/RHI/Renderer.*` |
| P0-4（已落地） | 输出 `build/bench/<timestamp>/baseline.md`（场景汇总 + 逐次运行明细两表），路径写入 `build/bench/latest.txt` | `tools/bench.ps1` |

**验收（已按实测改写）**：三个场景一条命令跑完并输出表格；600 帧 × 3 次的波动是 2.78% / 1.18% / 6.42%（另一批次 0.91% / 1.21% / 1.88%），跨批次中位数差约 4%。所以「波动 < 2%」只在机器空闲时成立，判据改为**同批次 A/B 比例**，脚本默认把波动降级成警告（`-Strict` 才升级成失败）。基线数字见 [baseline](../70-status/baseline.md)。

**回退**：无（纯增量）。

---

### P1 帧图 RDG 化（14 人日）

**目标**：让图能表达「外部资源」与「多个提交点」，并让 Pass 能被依赖关系剔除。
这是后面所有重量级特性的**准入条件**。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P1-1 | `ExternalResourceRegistry`：帧首 `acquire(handle, access)`、帧尾 `release()`；提交前把屏障集中写进命令缓冲头部 | `src/RHI/RenderGraph.{h,cpp}` | 解锁 P1-6 |
| P1-2 | 单测/探针：把一个 ScratchArena buffer 注册成外部资源，用 `addComputePass` 写它，验证跨队列依赖被正确建立 | `src/Render/CrossQueueProbePass.*` 扩展 | 已有探针，扩展即可 |
| P1-3 | 屏障批处理：按 `(srcStage, dstStage)` 分组，一次 `vkCmdPipelineBarrier2` 发多条；屏障条数进 stat | `src/RHI/RenderGraph.cpp` | P0-3 的计数对照 |
| P1-4 | 跨帧 RT 池：键 `(format, extent, usage, samples, mips)`；命中复用，未命中创建；帧末归还 | 新增 `src/RHI/RenderTargetPool.{h,cpp}` | 不动别名 |
| P1-5 | Pass 剔除：反向存活标记（present 目标 / readback / `NeverCull` 为根）+ 剔除后用「必须有人写」校验做 dry-run | `src/RHI/RenderGraph.cpp` | 校验已在，复用它 |
| P1-6 | 多提交点：一帧拆 `G0 -> C0 -> G1`；`Renderer::endFrame` 改成接受 `FramePlan` | `src/RHI/Renderer.{h,cpp}:214` | CONCURRENT，不做所有权转移 |
| P1-7 | `ClusterBuildPass` 从图形流改回 `addComputePass` | `src/Render/ClusterBuildPass.*` | 验证 P1-1 的收益 |

**验收**

- 512 灯场景 FPS ≥ 65.37（TM-13 的 GPU 建簇基线），且聚簇 dispatch 出现在 compute 队列（用 `--trace-queues` 或现有 breadcrumbs 验证）。
- `--no-bloom` 时 Bloom 的 7 个 Pass 不再进图（用「图内 Pass 数」计数验证：从 N 降到 N-7），而不是录了再跳过。
- barrier 条数下降 ≥ 25%（P0-3 基线对照）。
- 三个基准场景 `[Validation] errors=0 warnings=0`，stderr 0 字节，退出码 0。

**回退开关**：`--no-async-split`（退回一帧两提交）、`--no-pass-cull`（退回全执行）、
`--no-rt-pool`（退回每帧创建）。三个开关各自独立，打开时截图必须逐像素一致。

---

### P2 持久 GPUScene（12 人日）

**目标**：场景对象在 GPU 上有稳定编号，剔除粒度从「组」变成「实例」，
并把「每帧摊平」改成「脏区间上传」。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P2-1 | `primitiveBuffer`：材质槽、索引缓冲、包围盒、LOD 误差、图元类型 | 新增 `src/Render/GpuScene.{h,cpp}` | 资产加载时写一次 |
| P2-2 | `instanceBuffer`：世界矩阵、法线矩阵、primitiveId、flags、**上一帧世界矩阵** | 同上 | 上一帧矩阵给 P5 的 velocity 用 |
| P2-3 | 脏区间上传：实例数据按 256 条一块，只上传变化的块 | 同上 | 复用 `ScratchArena` 做 staging |
| P2-4 | 逐实例剔除：frustum + Hi-Z，输出压缩 `visibleIdList` + 每视图 indirect args | 扩展 `src/Render/SceneGpuBatch.*` | 主视图 / 阴影 / 反射各一份 |
| P2-5 | 退役组级合并缓冲：`GroupRecord` 只保留「合并网格的索引区间」，剔除不再按组 | 同上 | 合并缓冲的收益在 draw call，不在剔除 |
| P2-6 | 灯数据搬进 `lightBuffer`：`GlobalUbo.pointLights[100]` 改为「灯数 + 句柄」 | `src/Core/FrameInfo.h` + 光照 shader | 灯数上限解除 |

**验收**

- 1 万立方体场景：`visible` 与 `submitted` 的比值 ≤ 0.6（现在组级剔除大约 0.85）。
- 每帧实例数据的 CPU 摊平时间下降 ≥ 70%（P0 基线对照；预期从毫秒级降到微秒级）。
- draw calls 不增加（仍为 2 条 indirect / Pass）。
- 512 灯 + 1 万实例下，CPU 帧时间不高于现在（允许 ±5%，因为多了逐实例剔除）。

**回退开关**：`--no-gpu-scene-v2`（退回每帧摊平 + 组级剔除），`--no-instance-cull`（退回全提交）。

---

### P3 绘制命令与 PSO（8 人日）

**目标**：把「画一次」编码成定长命令，状态切换可缓存可排序；PSO 从「首用即建」变成「可预热、可计数」。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P3-1 | `MeshDrawCommand` POD + 排序键 + 桶排序 | 新增 `src/Render/MeshDrawCommandList.{h,cpp}` | 见 §2.7 的结构 |
| P3-2 | `StateCache`：只绑变化的 PSO / 描述符 / 顶点缓冲 | 同上 | 计数器进 stat |
| P3-3 | 光照与几何 Pass 改读命令表（不再逐对象写 descriptor） | `src/Render/GeometryPass.*`、`LightingPass.*` | 两处试点，其余 Pass 不动 |
| P3-4 | `PSOCacheKey` + 启动后延迟预热 + 缺失计数 | 新增 `src/RHI/PsoCache.{h,cpp}` | 见 §2.8 |
| P3-5 | 着色器参数块自动化（把 TM-12 从 SSAO 扩到这两个 Pass） | `src/Render/*Pass.cpp` + `build/shaders/*.reflect.h` | 纯收益项 |

**验收**

- 状态切换次数下降 ≥ 40%（P0-3 基线对照，cube-field 场景）。
- 首帧 hitch：连续 60 帧内，最长帧不超过中位帧的 3 倍（现在没有这个指标，P0 建立）。
- 立方体场景 FPS 不低于改造前 -2%（命令表本身有开销，靠状态缓存赚回来）。

**回退开关**：`--no-draw-command-list`、`--no-pso-precache`。

---

### P4 虚拟阴影图（20 人日）

**目标**：阴影分辨率与「相机到遮挡物距离」解耦；静态物体不重画阴影。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P4-1 | 页分配器（与 P7 距离场共用）：空闲链表 + 分配/释放 compute + 间接参数 | 新增 `src/Render/SparsePageAllocator.{h,cpp}` | 一次写、两处用 |
| P4-2 | 物理图集 + 页表 + 每页 HZB | 新增 `src/Render/VirtualShadowMap.{h,cpp}` | 8192² / 128² 页 / 4096 页 |
| P4-3 | clipmap：3 级、沿视线滚动、页对齐；每级包围体计算 | 同上 | 参考 `VirtualShadowMapClipmap.cpp:490` 的思路 |
| P4-4 | 页标记：本帧投影到该页的实例 → 脏页集合；未标记页复用上帧内容 | 同上 | 静态缓存的核心 |
| P4-5 | 阴影几何复用 P2 的 `visibleIdList`，不再 CPU 逐实体提交 | `src/Render/ShadowPass.*` | 与 P2-4 对接 |
| P4-6 | 采样：`LightingPass` 从 CSM 数组采样改为页表 → 图集采样 | `shaders/lighting.frag` | 保留 CSM 路径做回退 |

**验收**

- 同等显存（约 64M texel）下，近处阴影 texel 密度 ≥ CSM 的 8 倍（用「1 米在阴影图上的像素数」计算）。
- 静态场景（相机移动、光源与物体不动）：阴影 Pass 的 GPU 时间下降 ≥ 60%（页复用生效）。
- 动态场景：阴影 Pass GPU 时间 ≤ CSM 的 1.5 倍。
- 阴影边缘无可见接缝（页边界处逐像素对比 CSM 与 VSM 的深度，差值 < 1e-4）。

**回退开关**：`--no-vsm`（整体退回 CSM，代码路径保留到 P4 验收后 3 个版本）。

---

### P5 成像链：速度缓冲与 TSR 启发式（12 人日）

**目标**：把 TAA 从「相机重投影」升级到「逐物体运动矢量 + 时域拒斥」，
并让运动模糊第一次拥有正确的输入。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P5-1 | G-Buffer 增 `RG16F velocity`：`project(prevVP * prevWorld) - project(VP * world)` | `src/RHI/GBuffer.*`、`shaders/mrt.frag` | 需要 P2-2 的上一帧世界矩阵 |
| P5-2 | TAA 改用 velocity 重投影；颜色空间换 YCoCg 做方差裁剪 | `src/Render/TAAPass.*`、`shaders/taa.frag` | 保留原路径做回退 |
| P5-3 | 薄几何检测（邻域深度差）+ 去遮挡检测（velocity 与邻域中位数偏差） | `shaders/taa.frag` | 两个启发式，先各加一个阈值 |
| P5-4 | 运动模糊改读 velocity，替代「全屏相机速度」 | `shaders/present.frag` | 现在 `FrameInfo.h:288` 的注释写的就是这个妥协 |
| P5-5 | 预留 `ITemporalUpscaler` 接口（只定义，不实现 CNN） | 新增 `src/Render/TemporalUpscaler.h` | 给后续留缝，不增加当前复杂度 |

**验收**

- velocity 与 CPU 端 NDC 差分误差 < 0.5 像素（用 3 个已知变换的实例做定点测试）。
- 相机以 1 像素/帧平移时，静态区域无可见鬼影（对比截图 + 「静态区域时域方差」指标下降 ≥ 30%）。
- 快速旋转相机（90°/s）时，薄物体（树叶、栏杆）不产生拖影（人工判据 + 截图存档）。

**回退开关**：`--no-velocity`（TAA 退回世界位置反投影，G-Buffer 不写 velocity）。

---

### P6 meshlet 几何（22 人日）

**目标**：几何细节不再受「draw 数 + 顶点着色器调用量」限制，
把「画什么」从 CPU 生成的间接参数交给 GPU 的 cluster 层级决定。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P6-1 | 离线切分：按索引贪心聚成「≤64 顶点 / ≤124 三角形」的 cluster，再按误差聚成层级 | `tools/meshlet/`（自己写，不引第三方） | 不追求最优切分，追求可复现 |
| P6-2 | 打包：顶点数 14 位 + 位置偏移 18 位、索引位宽 4 位、包围球、LOD 误差 —— 参考 `NaniteResources.h:106 FPackedCluster` 的位域划分 | `src/Render/Meshlet.h` | 位宽不够时降级到 22/10，别硬塞 |
| P6-3 | GPU cluster 剔除：frustum + HZB + LOD 误差选择 | `shaders/cluster_cull.comp` | 复用 P2 的实例表 |
| P6-4 | 光栅化第一版：每 cluster 一条 indirect draw（硬件光栅） | `src/Render/GeometryPass.*` | 先跑通，再谈性能 |
| P6-5 | 可见性缓冲 + 材质解析：`R32_UINT` 存 clusterId + 三角形 id，第二遍解析材质 | 新增 `src/Render/VisibilityBuffer.*` | 只在 meshlet 路径启用 |

**验收**

- 200 万三角形场景：几何阶段 GPU 时间下降 ≥ 40%。
- 顶点着色器调用量下降 ≥ 60%（LOD + 剔除生效）。
- 无可见裂缝（相邻 cluster 的边界顶点必须共享，切分工具做 assert）。
- 与 P3 的命令表共存：meshlet 路径与经典路径可在同一帧切换（用于对照）。

**回退开关**：`--no-meshlet`（整体退回经典几何路径，命令表保留）。

**为什么放在 P6 而不是更早**：cluster 剔除的输入是「逐实例的可见性与 LOD 误差」（P2），
输出是「批量 indirect 命令」（P3）。没有这两层，meshlet 只能退化成「更多的小 draw」，
比现在还慢——这是 UE 之外很多引擎踩过的坑。

---

### P7 光照扩展（18 人日）

**目标**：把「屏幕空间光照」补上「世界空间近似」：远距离遮蔽、接触硬化阴影、
可解析的天空，以及解除灯数上限后的第二级优化。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P7-1 | 距离场：3 级 clipmap（64³/级）、页 8³、复用 P4-1 的页分配器 | `src/Render/DistanceField.*` | 先只做「物体 → 距离」的合成 |
| P7-2 | 天空光遮蔽：距离场沿法线步进，与 SSAO 取 max | `shaders/lighting.frag` | 解决 SSAO 的屏幕外失效 |
| P7-3 | 接触硬化软阴影：按距离场梯度估遮挡锥角 | `shaders/shadow_sample.glsl`（新增） | 与 P4 的页表采样并存 |
| P7-4 | 天光大气：transmittance LUT + multiscatter LUT + raymarch | `shaders/sky_geometry.frag` 改造 | 现在是渐变 + 太阳盘 |
| P7-5 | 灯数上限解除后的第二级优化：逐像素随机采样 N 盏灯 + 时域重用 | `shaders/lighting.frag` | 仅在灯数 > 256 时启用 |

**验收**

- 关闭 SSAO、只开距离场遮蔽时，远处墙面不出现「无遮蔽的悬浮感」（对比截图；客观指标：远平面遮蔽项的方差 ≥ SSAO 的 60%）。
- 天空在太阳高度 0~60° 区间，与参考 LUT 的色差 < 2%（逐像素）。
- 云 + 距离场 + VSM 同时开启，1080p 下总 GPU 时间增量 ≤ 3ms。

**回退开关**：`--no-distance-field`、`--no-sky-atmosphere`、`--no-stochastic-lights`。

---

### P8 工具与工程化（持续，按窗口插）

**目标**：让「加一个特性」从「改 5 个文件」变成「注册 1 个扩展」。

**任务**

| # | 内容 | 落点 | 备注 |
|---|---|---|---|
| P8-1 | `CVar<T>` 声明式注册，命令行与控制台共用一份 | 新增 `src/Core/CVar.{h,cpp}` | 逐步替换散落的 `--no-xxx` 解析 |
| P8-2 | stat 注册表：Pass 名自动成计时项，计数器进 HUD | `src/RHI/PipelineStats.*` 扩展 | 与现有 `GpuProfiler` 对接 |
| P8-3 | `IViewExtension` + 6 钩子 + `subscribePostProcess` | 新增 `src/Render/ViewExtension.{h,cpp}` | 见 §2.14 |
| P8-4 | 截图金标自动化：每阶段一条命令生成 `build/golden/<phase>/` 并出 diff | `tools/` 扩展 | 把「逐像素一致」变成可执行判据 |

**验收**：实现一个新的后处理特性（例如「胶片颗粒」），改动文件数 ≤ 2（现在需要改 4~6 个）。

---

## 4. 为什么是这个顺序

顺序不是偏好，是被依赖卡死的。下面每条都给反例——如果顺序反过来会发生什么。

1. **P1 必须在 P2 之前**。P2 的逐实例剔除输出 `visibleIdList`，要被「主视图 / 阴影 / 反射」三个消费者读，
   每个消费者的队列不同。没有外部资源注册与多提交点，这份列表只能放在图形流里串行消费，
   异步 compute 永远只能跑探针。**反例**：先做 P2，剔除结果无法跨队列，等于白做一遍剔除。
2. **P2 必须在 P4 之前**。VSM 的页标记输入是「本帧投影到该页的实例集合」，
   而实例集合来自逐实例剔除。没有 P2，页标记只能按「组」做，粒度粗到会把整组静态物体标脏，
   页复用的收益直接归零。**反例**：先做 P4，页复用率接近 0，VSM 退化成「更贵的 CSM」。
3. **P2 必须在 P6 之前**。cluster 剔除的输入是实例的 LOD 误差与可见性；
   meshlet 的第一层剔除本质上是「实例级剔除」的细化。**反例**：先做 P6，cluster 数据结构要跟着实例表重写两次。
4. **P3 与 P4 可并行**。P3 改「怎么发命令」，P4 改「往哪画」。两者只在 `visibleIdList` 的消费端相遇，
   接口一旦定好（P2 结束时），两边可以各自推进。
5. **P5 必须在 P2 之后**。velocity 需要「上一帧世界矩阵」，那是 P2-2 的 `instanceBuffer` 里的字段。
   现在 `FrameInfo` 只有 `previousProjectionView`（相机级），拿不到逐物体速度。**反例**：先做 P5，
   只能继续用相机重投影，等于把 TAA 的旧妥协原样保留。
6. **P7 最后**。距离场与天光大气都是「在已有画面上加一层近似」，收益是画质不是架构。
   而灯数上限的解除（P2-6）必须先发生，否则 P7-5 的随机采样没有对象。
7. **P8 可以随时插**。它不改渲染路径，只改「怎么观测、怎么扩展」。唯一需要注意的是
   P8-3 的 `IViewExtension` 一旦落地，P5/P6/P7 的新特性应该走它，而不是继续往 `Application.cpp` 里加分支。

**关键路径**：`P0 -> P1 -> P2 -> P3 -> P6`，共 58 人日。
P4 -> P5 -> P7 是次关键路径，共 50 人日，可与主路径部分重叠。
按每周 4 个有效人日算，主路径约 14.5 周，全量约 27 周。

---

## 5. 验收协议

每个阶段都用同一套闸门，避免「改完自己觉得挺好」。

### 5.1 三道硬闸门（沿用 [borrowed-techniques](borrowed-techniques.md)）

| 闸门 | 判据 | 怎么验 |
|---|---|---|
| 不破坏基线 | 退出码 0、stderr 0 字节、`[Validation] errors=0 warnings=0` | `tools/Validate.ps1` |
| 有可量化验收 | 每个阶段至少一个数字指标（FPS / GPU 时间 / 计数） | `tools/bench.ps1` 对照 P0 基线 |
| 可整体回退 | 每个阶段有独立开关，关掉后行为与改造前一致 | 开关矩阵逐项跑一遍 |

### 5.2 阶段验收表

| 阶段 | 主指标 | 门槛 | 附加指标 |
|---|---|---|---|
| P1 | 512 灯场景 FPS | ≥ 65.37 | barrier 条数 -25%；`--no-bloom` 时图内 Pass 数 -7 |
| P2 | `visible/submitted` | ≤ 0.6 | 摊平 CPU 时间 -70%；draw calls 不增 |
| P3 | 状态切换次数 | -40% | 最长帧 ≤ 中位帧 3 倍 |
| P4 | 近处阴影 texel 密度 | ≥ CSM 的 8 倍 | 静态场景阴影 GPU 时间 -60% |
| P5 | velocity 误差 | < 0.5 像素 | 静态区域时域方差 -30% |
| P6 | 几何阶段 GPU 时间 | -40% | 顶点着色器调用量 -60% |
| P7 | 远平面遮蔽方差 | ≥ SSAO 的 60% | 天空色差 < 2%；总增量 ≤ 3ms |
| P8 | 新特性改动文件数 | ≤ 2 | — |

### 5.3 命令

```powershell
# 全量回归（含文档检查）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1

# 基准（P0 建立，之后每个阶段跑一次）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bench.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bench.ps1 -Scenario lights-512 -Runs 5 -Strict
```

### 5.4 回退开关约定

- 命名统一：`--no-<feature>`，与现有 `--no-hiz`、`--no-ssr`、`--no-merged-buffers` 一致。
- 开关必须能在同一进程内通过 ImGui 切换（不只是命令行），否则调试时无法对比。
- 开关打开时，渲染结果必须与改造前**逐像素一致**（P5 的 TAA 路径除外，那里允许「旧路径」作为回退）。
- 开关默认值：新特性第一版默认**关闭**，验收通过后再改成默认开启。

---

## 6. 风险台账

| 风险 | 触发信号 | 缓解 | 回退 |
|---|---|---|---|
| 多提交点破坏 present 顺序 | 偶发花屏 / 设备丢失 | 每帧把 timeline 值写进 breadcrumbs；在 `Renderer::endFrame` 加 assert：提交序号单调 | `--no-async-split` |
| 外部资源生命周期错配 | validation layer 报「resource in use」 | registry 持引用，帧末断言计数归零 | 移除注册，退回手工屏障 |
| `RenderGraph.cpp`（63KB）继续膨胀 | 单文件超 80KB、改动频率最高 | P1 开工前先拆：`RenderGraphBarrier.cpp` / `RenderGraphExternal.cpp` / `RenderGraphCull.cpp` / `RenderGraphPool.cpp` | — |
| 逐实例剔除与合并网格冲突 | 合并网格的实例被单独剔除后缺块 | 合并网格保留「区间」语义，按区间剔除，不按实例 | `--no-instance-cull` |
| VSM 页分配难调 | 页泄漏 / 画面出现黑块 | 复用 `DebugLightGrid` 的做法，加「页可视化」调试通道 | `--no-vsm` |
| 距离场合成太慢 | 首帧卡顿 | 先只做 1 级 64³，按需增加 | `--no-distance-field` |
| 截图金标受驱动差异影响 | 换机器就 diff 失败 | 只在同一台机器上做逐像素 diff，跨机器只比结构指标 | — |
| 工期估算崩 | 某阶段超 1.5 倍 | 每阶段结束都是可停状态；宁可砍范围（例如 VSM 先只做方向光）不延工期 | — |

---

## 7. 第一批工单（可立即开始）

按顺序做，每单独立可验收、独立可回退。T-10 先做，因为没有基线就没有验收。

| # | 任务 | 落点 | 验收 | 人日 |
|---|---|---|---|---|
| T-10（已落地） | 基准脚本 + barrier/state 两个计数器 | `tools/bench.ps1`、`src/RHI/FrameStats.*`、`src/RHI/RenderGraph.cpp` | 三场景跑出 `baseline.md`；计数器逐位可复现 | 1 |
| T-01（已落地） | `ExternalResourceRegistry`：帧首 acquire / 帧尾 release | `src/RHI/RenderGraph.{h,cpp}` | 图外 buffer 的状态漂移被检出 + compute 写入的内容经图形流搬运后逐字正确 | 3 |
| T-02（已落地） | `ClusterBuildPass` 改回 `addComputePass` | `src/Render/ClusterBuildPass.*`、`src/RHI/ScratchArena.cpp` | 512 灯 FPS ≥ 65.37；聚簇 dispatch 在 compute 队列（breadcrumbs 验证） | 1 |
| T-03（已落地） | 图像屏障在 Pass 内按六元组分桶合并（新增 `src/RHI/RenderGraphBarrier.cpp`） | `src/RHI/RenderGraph.{h,cpp}`、`src/RHI/RenderGraphBarrier.cpp` | 调用 45.2→38.0 / 61.2→47.0；`entries` 不变；A/B FPS +0.1% | 2 |
| T-04 | 跨帧 RT 池 | 新增 `src/RHI/RenderTargetPool.{h,cpp}`，`--rt-pool` 才开 | **已落地（默认关）**：稳态本来就是 0 创建；一次 resize 约 44 张，池只省 13~15% 且要 256 MiB 空闲图 → 判据改成「一次 resize 创建量 44 → ≤25」 | 2 |
| T-05 | Pass 剔除 + dry-run 校验 | `src/RHI/RenderGraph.cpp` | **已落地（默认关）**：机制正确但本引擎没有「输出无人读」的 Pass，实测 `culled/frame=0.0`；`--no-bloom` 的 21 → 14 是 Pass 自己不进图 | 2 |
| T-06（已落地） | Blackboard（`type_index` + 类型擦除，生命周期一帧） | `src/RHI/RenderGraphBlackboard.h`、`src/Render/FramePayloads.h` | 八个交接字段搬进图级黑板，`FrameInfo` 字段减少 8 个（≥ 4 达标） | 1 |
| T-07 | `FramePlan` + 三提交点 `G0 -> C0 -> G1`（**已落地**：队列窗口时间戳 + 拆分提交，默认开，`--no-async-split` 回退） | `src/RHI/Renderer.{h,cpp}:214`、`src/RHI/RenderGraph.{h,cpp}` | compute 与 graphics 有重叠：`[Queue] overlap` 0.00 -> 0.51 ms（100% of compute）；`--no-async-split` 截图在噪声内 | 3 |
| T-08 | `primitiveBuffer` + `instanceBuffer` 骨架（先只读） | `src/Render/GpuScene.{h,cpp}` | 编号跨帧稳定；一万实例的 `primitives=104 / instances=10103`，稳态上传 0.2 KB/帧 | 3 | **已落地（只读骨架）** |

**T-08 只读骨架（2026-11-24）**：`GpuScene` 用内容键给图元与实例分配跨帧稳定编号（图元键 `(模型指针, 子网格)`，实例键再混组内序号），脏区间攒成一帧一次 `vkCmdCopyBuffer`；`SceneGpuBatch::buildInstances` 末尾镜像一份进持久缓冲，`prepare` 里 `beginFrame/flush/endFrame`。实测 cube-field `primitives=104 / instances=10103`、`dirtyFrames=1/40`、`upload=35.6 KB/frame`；Sponza `103/103`、`dirtyFrames=1/120`、`upload=0.2 KB/frame`。骨架阶段没有任何着色器绑定这两条缓冲，剔除仍走 `GroupRecord`，所以**这一步不产生帧时间收益**——它的价值是把「编号不漂移」变成可验收的不变量，供 P2 的实例级剔除直接记账。开关 `--no-gpu-scene-persist`（默认开）。踩坑：`SceneGpuBatch` 在 `GeometryPass::init()` 里才创建，`setEnabled` 必须放在 `renderPipeline.initAll()` 之后，否则启动期空指针 `0xC0000005`（stderr 0 字节、日志停在 `[Tiny] 材质就绪`）。
| T-09（已落地） | `MeshDrawCommand` + `StateCache`（先只接 GeometryPass） | 新增 `src/Render/MeshDrawCommandList.{h,cpp}` | 状态切换次数 -40%；FPS 不低于改造前 -2% | 3 |

**P2 第一步已落地（2026-11-24，决策 53）**：世界 AABB 改中心/半长（对仿射精确），`subMeshBoundsFor` 挪到加载期预热（`SceneGpuBatch::prewarmModelBounds`，启动打 `[SceneGpu] prewarm | models=N`）。`flatten` 从 33.55 / 56.52 / 116.99 ms（default / 10k / 30k）降到 10.94 / 34.31 / 72.05 ms；帧率不变（88.75 → 88.20 FPS，噪声内），省的是首帧 hitch（`max` 90 → 76.5 ms）。
**P2 的摊平分解（2026-11-24）**：`[SceneGpu] flatten breakdown` 把一次结构重建拆成 collect / merged / entities / external / sort / assemble / mirror 七段。`--cube-field 10000` 实测 total 56.5 ms：`external` 23.3 ms（每实例 ≈ 2.3 µs）、`entities` 17.6 ms（其中 `subMeshBoundsFor` 首次索引扫描 17.3 ms，一次性）、`merged` 8.6 ms、`mirror` 5.6 ms；`30000` 时 total 126.3 ms、`external` 74.6 ms、`mirror` 17.0 ms —— 逐实例部分是线性大头，`sort` + `assemble` 合计 < 2%。**结论：P2 的 −70% 必须打在逐实例构造上（把法线矩阵与 8 角点 AABB 挪到 GPU 或按实例缓存），搬家/排序省不出来；另外 `subMeshBoundsFor` 应该在加载期预热，否则首帧摊平被一次性扫描虚高。**

**P2 第二步（2026-11-24）**：先关掉没消费者的镜像 —— `--gpu-scene-persist` 默认关（`diag::Options::gpuScenePersist = false`），镜像整段包在 `if (gpuScene.isEnabled())` 里，`mirror` 三档归零。再认清剩下 80% 在哪：外部循环新增 `extRecord` / `extPush` 两段计时后，30k 实例下 `external 43.9 ms` 里只认领 6 ms（`extPush 4.9`、`extRecord 0.6`、`extAabb 0.2`、`extNormal 0.3`），其余约 38 ms（每实例 1.27 µs）是循环脚手架 + `std::map` 组查表 + `std::vector<std::vector>` 的 `push_back` 扩容拷贝 + Debug 下 4 次 `steady_clock` 采样。**结论：AABB 上 GPU 不再是 P2 的第一步**（`extAabb` 已 0.2 ms）；−70% 要靠「持久缓冲变权威、逐实例直写映射内存、去掉组查表」，实例级剔除则要先改 `shaders/scene_cull.comp`（现为组级，`GroupRecord` 5 word/组 + `stats[3]`）成 remap + 逐实例两段式。

**P2 第三步（2026-11-24）**：把尺子换成 Release 再判。实测 Release 摊平：30k `14.67 ms`（`external 9.0`、`merged 4.5`）、10k `7.83`、default `4.55`（`merged` 占 98.7%），FPS `100.45 / 119.26 / 198.53`；Debug 的逐实例成本被放大 4 倍（1.27 µs → 0.30 µs）。按本轮结论，**「持久缓冲变权威 + 逐实例直写映射内存」这一刀不做了**：试做「预分配 `instanceScratch` + 逐条写入」在 Debug 30k 上是负收益（`assemble 1.2 → 3.2 ms`、`total 57.60 → 58.78`），因为 `vector::insert` 对 POD 走 `memmove`、且稳态下 `buckets` 容量保留本来就不扩容，已回退。**P2 的下一个大头是 `merged`（合并网格，三档恒为 4.5 ms）**，要动它得改合并策略（按材质合并 / 持久化合并网格），判据统一改成「Release 构建 + 稳态重建帧」。


**P2 第四步（2026-11-24）**：先补「稳态」这把尺子再继续。新增 `--rebuild-structure <n>`（每 n 帧强制重建一次）与 `mergedBuild=` 计数后，Release 稳态（40 帧、每 10 帧重建）为：default `0.11 ms/次重建`、10k `3.80`、30k `10.08`，`merged=0.0`、`mergedBuild=0`。上一次「`merged` 占 98.7%」是首重建样本的假象 —— 合并网格按签名复用，只建一次（192496 顶点 / 786801 索引）。**结论：摊平只在结构签名变化时发生；P2 的 −70% 必须按「每次重建」计，且「合并网格持久化」不需要做。真要再压只有按需增量重建（只重建变化的组），那是 P2 之后的独立一单。**
T-01~T-07 总计 14 人日，完成后 P1 结束；T-08 开始 P2；T-09 开始 P3。
### 已落地的工单（2026-11-23）

**0a 拆文件（先决条件）**：`src/RHI/RenderGraph.cpp` 由 1534 行降到 1399 行，屏障发射搬到新的
`src/RHI/RenderGraphBarrier.cpp`（118 行，只放 `emitBarrierIfNeeded` / `emitBufferBarrierIfNeeded`），
两个翻译单元共用的 `kWriteBits` / `isDepthFormat` / `aspectOf` 收进 `src/RHI/RenderGraphInternal.h`
的 `namespace rgdetail`。判据是纯搬移：`--stats-frame` 三个数逐位不变（21.0 / 45.2 / 22.5 与 27.0 / 61.2 / 39.5）。
P1 的 T-03 / T-05 / T-06 都要动这两个文件，先切出屏障是唯一的无风险窗口。

**0b `tools/bench.ps1`**：三场景 × 3 次 × 600 帧，解析 `[SelfTest] performance/telemetry`、`[FrameStats]`、
`[Validation]` 三类行，输出场景汇总表 + 逐次运行明细。实测基线与波动判定见 [baseline](../70-status/baseline.md)：
default 160.82 FPS / voxel-world 96.15 / lights-512 88.93，波动 2.78% / 1.18% / 6.42%。

**T-10**：新增 `src/RHI/FrameStats.{h,cpp}`（`namespace tiny::framestats`，带 `selfTest`），
在 `RenderGraph::beginFrame/endFrame`、`emitBarrierIfNeeded`、`emitBufferBarrierIfNeeded`、
`Pipeline::bind` / `ComputePipeline::bind` 上挂钩，`--stats-frame` 退出时打三行。
实测（Debug，RTX 2070，60 帧）：
- 基线场景 `passes/frame=21.0`、`barriers/frame=45.2 calls 45.2 entries`（每子资源一条，`entries` 恒等于 `calls`）、`skipped: sameState=18.8 crossQueue=0.0`、`pipelineBinds/frame=22.5 redundant=10.5 (46.7%)`、87.59 FPS。
- 512 灯压力场景 `passes/frame=27.0`、`barriers/frame=61.2`、`sameState=35.8`、`crossQueue=2.0`、`pipelineBinds/frame=39.5 redundant=19.5 (49.4%)`。

**P0 的状态**：P0-1 / P0-3 / P0-4 已落地，P0-2 只补了本轮需要的部分（屏障条数、管线绑定与冗余、draw calls、
desc binds、三角形数；显存占用与 submitted/visible 组数仍靠 `--stats-clusters` 与编辑器面板人工看）。

**T-06 图级黑板**：新增 `src/RHI/RenderGraphBlackboard.h`（`std::type_index` → `Entry{void*, destroy}`，
`getOrCreate` / `find` / `set` / `has` / `clear`）与 `src/Render/FramePayloads.h`（曝光覆盖、选中项、
模拟插值、UI 回调四个载荷）；`beginFrame()` 第一件事 `blackboard.clear()`，载荷严格一帧。
`FrameInfo` 的八个交接字段全部删除，只留 `tiny::RenderGraphBlackboard* blackboard` 句柄供 `ImGuiSystem` 使用。
必须同改的一处时序：`imguiSystem->prepareDrawData(frameInfo)` 从 `beginFrame` 之前挪到载荷之后，否则面板读到空黑板。

- 默认 40 帧 + `sync`：`EXIT=0`、`errors=0`、`stderr 0` 字节、`[FrameTime] median=13.12ms p95=41.62ms max=95.70ms`、`[Resources] total=145`。
- `--cube-field 10000` 120 帧：`85.45 FPS`、`median=8.62ms p95=29.88ms max=132.36ms`、`instances/frame=10063`、`visibleRatio=0.62`、`errors=0`。
- `--cross-queue-test`：`[TD02]` 两向 + `[T01]` 两条断言全绿。
- 一次 `0xC0000005` 的代价：第一版一次性改完九个文件，编译过但启动即崩；回退后分四步重做，
  根因是 `callbacks` 的声明被插进 `if (!ImGui::CollapsingHeader(...))` 与它的 `return;` 之间，
  后面的 Save/Reload 读到未初始化指针。教训：往无花括号的单语句 `if` 中间插代码会静默切断控制流。
因此 P0 算**完成但有一处口径偏差**：计划里写的 `cube-field` 场景在引擎里没有生成器，改用 `--voxel-world 1`；
`editor-default` 改用 `--selftest` 的 `default`。偏差以 [baseline](../70-status/baseline.md) 的场景表为准。

**T-02**：`ClusterBuildPass` 的 dispatch 挂回 `addComputePass`，前面补两块砖：
`ScratchArena::createSlotBuffer` 在 `hasDedicatedComputeQueue()` 时用 CONCURRENT +
`{graphics,compute}` 两个队列族；簇表用 `importBuffer` 以 `crossQueue=true` 登记成图内句柄
（`desc.size` 取四段 `offset+size` 的最大值），读侧由一个只声明不执行的 `Clusters.ReadSync` 表达。
手写的两条 `VkBufferMemoryBarrier2` 删除。详见[决策 43](../10-architecture/decisions.md)。

**验收偏差（重要）**：
- 「512 灯 FPS ≥ 65.37」这条门槛来自文档记载的 60 帧、不带截图、不带 `--validation-features sync` 的测量；
  本次为了同时验证跨队列正确性开着了 sync 验证层，绝对 FPS 掉到 40 上下，与该门槛不可比。
  因此改用**同构建内的 A/B 比例**当判据：120 帧、`--voxel-world 1 --light-stress 512 --clustered-lighting --screenshot-frame 90` 下，
  旧代码 GPU 58.61 / CPU 45.80 FPS，新代码 GPU 56.23 / CPU 43.63 FPS —— 两侧同降（运行间噪声），
  比例仍是 GPU 建簇快约 29%，说明 T-02 没有吃掉决策 40 的收益。
- 「聚簇 dispatch 在 compute 队列（breadcrumbs 验证）」改用 `--stats-frame` 的 `skipped: crossQueue=2.0`
  作为判据：那是 ranges / indices 两条缓冲屏障被 timeline 信号接管后跳过的计数，
  通路不通就不会有这两个数。`--validation-features sync` 下 `errors=0 warnings=0`、stderr 0 字节。

**顺带修掉的既有缺陷**：`src/RHI/ScreenshotCapture.cpp` 转入 `TRANSFER_SRC_OPTIMAL` 的 `dstAccessMask`
误写成 `TRANSFER_WRITE`，导致任何 `--screenshot` + `sync` 运行报一条 READ-AFTER-WRITE。
已改为 `TRANSFER_READ`，见 [pitfalls 第 39 条](../60-debug-profiling/pitfalls.md)。


**T-01 外部资源的状态声明（`declareExternalTexture` / `declareExternalBuffer`）**

落点：`src/RHI/RenderGraph.{h,cpp}`（`struct ExternalState{layout, stage, access}`、两个声明函数、
条目上的 `externalDeclared` / `externalUsed` / `externalEnd` / `lastExternalEnd` / `hasLastExternalEnd`、
`endFrame` 尾部的交回段）、`src/Render/CrossQueueProbePass.{h,cpp}`（图外 buffer + 回读缓冲 + 双向验收）。

三条不变量：每资源每帧最多声明一次、声明早于第一次使用、本帧入口等于上一帧出口。第三条违反时抛
「状态漂移」。交回屏障在 `endFrame` 尾部发在 graphics 流上（外部持有者就在这一侧消费），不参与
T-03 的攒批。

验收（`--selftest 40 --cross-queue-test --validation-features sync`）：`errors=0 warnings=0`、
stderr 0 字节、EXIT 0；负向 `[T01] external buffer drift detection verified`、正向
`[T01] external buffer verified: entry (COPY, TRANSFER_READ) -> fill (CLEAR, TRANSFER_WRITE) ->
copy (COPY, TRANSFER_READ), 64 words`。

**验收偏差**：计划里写的是「探针 Pass 写入的 scratch buffer 能被 compute 队列正确读取」。实际改成
探针**自建的图外 buffer**（VMA 创建、`hasDedicatedComputeQueue()` 时 CONCURRENT）。理由是暂存区是
`ScratchArena` 管理的整块 buffer，探针往里写会污染同一个 arena 的其它使用者（阴影矩阵、聚簇表都在
里面），而自建 buffer 能同时验三件事：CONCURRENT 创建、跨队列 timeline 依赖、host 可见回读。
代价是「暂存区整块 buffer 能否作为图外资源被声明」这条路径没有直接覆盖 —— 聚簇表那条（T-02 的
`Scratch.ClusterTables`）走的是 `importBuffer` + `writeBuffer` 声明，不经过 T-01 的 `declareExternalBuffer`。

**顺带修掉的两个问题**（细节见 [pitfalls](../60-debug-profiling/pitfalls.md) 第 40、41 条）：
动态渲染期间录 `vkCmdCopyBuffer` 会被验证层判 `VUID-vkCmdCopyBuffer-renderpass`（20 条错误 → 0，
搬进零附件 Pass）；用 `frameNumber - kFramesInFlight` 反推写入者会指到不存在的第 0 帧（回读全 0，
改成每槽位记录排队帧号）。
**度量批（T-10 的后续，2026-11-23）**：`--stats-frame` 从三行扩到七行，补的正是 T-04 / P2 / P3 各自缺的判据。

| 新增读数 | 决定的判据 | 实现 |
|---|---|---|
| `[FrameTime] median/p95/max` | P3「最长帧 ≤ 中位帧 3 倍」 | `FrameStats::recordFrameTime` + 0.25 ms 桶直方图 + `percentile()` |
| `[Resources] images/frame total peakFrame` | T-04「稳定态每帧 `vkCreateImage` 为 0」 | `ResourceStats`，挂在 `createOwnedTexture` / `createOwnedBuffer` / `createSlotBuffer` 之后 |
| `[Memory] heapN used/allocs first/last/peak` | T-04「显存不增长」 | `vmaGetMemoryProperties` + `vmaGetHeapBudgets`，10 Hz 采样，退出前补一次 |
| `[SceneGpu] cull summary visibleRatio` | P2「visible/submitted ≤ 0.6」 | `SceneGpuBatch::readStats` 累计 + `logCullSummary()` 归一化 |

实测（600 帧 × 3 次）：default `115.91 FPS / 8.63 ms / 中位 7.88 / p95 9.12 / 最长 88.02 ms / images 0.27 / heap0 1220.7 MiB / visible 0.61`，
voxel-world `98.73 / 10.13 / 9.62 / 10.88 / 89.83 / 0.27 / 1220.7 / 1.00`，lights-512 `89.57 / 11.16 / 10.62 / 11.88 / 100.43 / 0.27 / 1220.7 / 1.00`。
`images/frame` 是全程平均：160 张图在启动期创建，摊到 600 帧就是 0.27，稳态每帧是 0 —— T-04 要压的是启动峰值，不是这个均值。

**顺带发现的基线缺陷**：`default` 场景此前的基线是「1 draw call / 8192 三角形」，因为相机来自
`config/engine.json`（本机存的是 `(-66.06, 15.92, -76.19)`，站在 Sponza 外 100 米看向天空），
103 个几何组全部落在视锥外。修法是给引擎加 `--camera <px,py,pz,pitch,yaw>`，`bench.ps1` 三个场景都钉住
`Scene::init` 的 Sponza 默认机位。钉住之后 `default` 变成 104 draw calls / 270459 三角形 / `visibleRatio 0.61`，
115.91 FPS 不能和旧版的 164.80 直接比。这条属于「先有尺子」的一部分：没有固定机位，截图金标与剔除读数都不可复现。
---

## 8. 明确不抄清单

写下来是为了以后不用重新论证一遍。每条都给「不抄的理由」与「替代方案」。

| 不抄的东西 | UE 证据 | 为什么不抄 | 替代 |
|---|---|---|---|
| UObject / 反射 / GC | `CoreUObject/` | 无热更、无蓝图、无序列化兼容需求；C++ 直接注册更快 | 静态注册表 + `type_index` |
| RHI 线程 | `RHI/Public/RHICommandList.h` 187KB | 21 个 Pass 的单线程录制 < 1ms，收益为负 | 保持单线程录制 |
| Slate / UMG | `Slate*`、`UMG` | UI 不是渲染核心，已有 ImGui | ImGui |
| Substrate | `Renderer/Private/Substrate/` | 材质组合爆炸，需要材质图配合才有意义 | 特性位集（§2.12-2） |
| Lumen 全量 | `Renderer/Private/Lumen/` | surface cache + mesh card + radiosity 三件套，成本远大于收益 | 距离场 + SSAO/SSR（P7） |
| Nanite 全量流式 | `Renderer/Private/Nanite/`（`NaniteCullRaster.cpp` 327KB） | 需要虚拟纹理 + 流式 IO + 专用压缩格式 | meshlet + 简化 LOD（P6） |
| MegaLights | `Renderer/Private/MegaLights/` | 依赖 GPUScene + VSM + 随机采样三件套，缺一不可 | P7-5 的简化版 |
| TSR 神经网络 | `FTSRConvolutionNetworkShader` 645 | 需要独立推理路径与训练流程 | 启发式 TSR（P5） |
| VSM 的 Nanite 页标记 | VSM 与 Nanite 耦合路径 | 没有 cluster 数据 | 先硬件光栅（P6-4） |
| 并行 Pass 执行 | `RDG_ENABLE_PARALLEL_TASKS` | TM-10 实测并行准备比串行慢 4.9~5.8 倍 | 串行 |
| `FRDGBlackboard` 内存池 | `RenderGraphBlackboard.h` | 21 个 Pass 用 `std::any` 足够 | `std::any` |
| 19 个 ViewExtension 钩子 | `SceneViewExtension.h:145-261` | 你只有单相机与一条后处理链 | 6 钩子（§2.14） |

---

## 9. UE 参考索引

按主题存放，全部相对 `D:\work\Engine\UE\Engine\Source\Runtime\`。

| 主题 | 路径 | 关键符号 |
|---|---|---|
| 访问态与屏障 | `RHI/Public/RHIAccess.h` | `ERHIAccess`(10)、`IsReadOnlyExclusiveAccess`、`IsWritableAccess` |
| Pass 类型标记 | `RenderCore/Public/RenderGraphDefinitions.h` | `ERDGPassFlags`(129)、`ERDGBufferFlags` |
| 外部资源 | `RenderCore/Public/RenderGraphUtils.h` | `FRDGExternalAccessQueue`(1261) |
| 图级黑板 | `RenderCore/Public/RenderGraphBlackboard.h` | `FRDGBlackboard`(56) |
| 剔除与并行开关 | `RenderCore/Private/RenderGraphPrivate.h` | `GRDGCullPasses`(85)、`STAT_RDG_PassCullCount`(166)、`GRDGParallelExecute*`(94-101) |
| 资源池与提取 | `RenderCore/Public/RenderGraphBuilder.h` | `QueueTextureExtraction`(345)、`AsyncComputeBudget`(1162) |
| 瞬态分配 | `RHI/Public/RHITransientResourceAllocator.h` | `FRHITransientResourceAllocator` |
| 场景 GPU 数据 | `Renderer/Private/GPUScene.h` | `Update`(267)、`UploadGeneral`(485)、`AllocateInstanceSceneDataSlots`(244)、`InstanceDataTileSize`(377) |
| 绘制命令 | `Renderer/Public/MeshPassProcessor.h` | `FMeshDrawCommand`(1281)、`MatchesForDynamicInstancing`(1319)、`FMeshDrawCommandSortKey`(1536)、`FMeshDrawCommandStateCache`(905) |
| PSO 预热 | `Engine/Public/PSOPrecache.h` | `FPSOPrecacheParams`、`PrecachePSOs` |
| Nanite cluster 打包 | `Engine/Public/Rendering/NaniteResources.h` | `FPackedCluster`(106) |
| Nanite 着色分组 | `Renderer/Private/Nanite/NaniteShared.h` | `FNaniteShadingPipeline`(891)、`FNaniteShadingCommand`(1103)、`FNaniteRasterBin`(659) |
| 虚拟阴影图 | `Renderer/Private/VirtualShadowMaps/VirtualShadowMapArray.h` | `FVirtualShadowMap`(67)、`NumHZBLevels`(86)、`BeginMarkPages`(378)、`MaxPhysicalPages`(160) |
| VSM clipmap | `Renderer/Private/VirtualShadowMaps/VirtualShadowMapClipmap.cpp` | 构造(200)、`ComputeBoundingVolumes`(490)、`UpdateCachedFrameData`(640) |
| 光照网格 | `Renderer/Private/LightGridInjection.cpp` | `FLightGrid`(822)、`LightGridInjection`(828)、`FLightGridInjectionCS`(435) |
| 距离场 | `Renderer/Private/GlobalDistanceField.cpp` | `FInitPageFreeListCS`(1727)、`FAllocatePagesCS`(1757)、`FCullObjectsToGridCS`(1581) |
| TSR 多 Pass | `Renderer/Private/PostProcess/TemporalSuperResolution.cpp` | `FTSRRejectShadingCS`(958)、`FTSRDetectThinGeometryCS`(1080)、`FTSRUpdateHistoryCS`(1270) |
| 视图扩展 | `Engine/Public/SceneViewExtension.h` | `ISceneViewExtension`(145-261) |
| CVar | `Core/Public/HAL/IConsoleManager.h` | `IConsoleVariable`(577)、`TAutoConsoleVariable`(1969/2134) |
| Stats | `Core/Public/Stats/Stats.h` | `DECLARE_CYCLE_STAT`(130)、`DECLARE_DWORD_COUNTER_STAT`(139) |
| 材质 | `Engine/Public/MaterialShared.h` | `FMaterialShaderMap`(1630)、`FMaterial`(2169) |

---

## 10. 这份文档怎么维护

- 本文只保留「抄什么、按什么顺序、怎么验收」。单项技术的实测数字与判定，写进
  [borrowed-techniques](borrowed-techniques.md)（TM 编号）；已经落地的架构决策，写进
  [decisions](../10-architecture/decisions.md)（决策编号）。
- 某个阶段完成后，**不要删本文的对应小节**，而是在 §3 的表格里把「工期」改成实际值，
  并在本节追加一行偏差记录（计划 vs 实际、砍了什么范围、为什么）。
- 若某项被判定不做，把它移进 §8，并补「不抄的理由」与「替代方案」——这是防止
  半年后重新论证同一件事的唯一办法。
**T-03**：`emitBarrierIfNeeded` 改成先追加进 `pendingImageBarriers`、由 `flushImageBarriers` 在 Pass 命令
之前按 `(srcStage,dstStage,srcAccess,dstAccess,oldLayout,newLayout)` 分桶发射；缓冲屏障仍就地一条
（每次访问只有一段）。实测：`calls` 45.2 → 38.0（default）、61.2 → 47.0（voxel-world / lights-512），
`entries` 完全不变，`--validation-features sync` 两场景 `errors=0`。

**验收偏差（重要）**：计划写的「barrier 条数 -25%」没达到（-16% / -23%）。第一版按子资源合并是零收益——
探针显示每个 Pass 的每次访问都只有 1 mip / 1 layer，桶里永远一个成员；只有把合并跨到"同一 Pass 里的多张图"
才拿到两位数降幅。而同批次 A/B（`-Scenario lights-512 -Runs 3 -Frames 600`）显示 89.62 → 89.71 FPS，
**省下的调用没有换成帧时间**。因此这条工单的定位改成"结构就绪、性能待 T-07 兑现"：
瓶颈在 P3 的状态切换（冗余 49.4%）与 P2 的每帧摊平，不在屏障调用数。跨 Pass 的帧级有序屏障
（UE RDG 的完整形态）需要提交点先分段，排在 T-07 之后。
**T-04（2026-11-23，默认关）**：`src/RHI/RenderTargetPool.{h,cpp}`，键为

**P2 的入场尺子：`--cube-field`（2026-11-24）**

P2 的三条验收（`visible/submitted ≤ 0.6`、摊平 CPU 时间 −70%、draw calls 不增）此前一条都测不了：
引擎里没有一万实例的场景，`--voxel-world 1` 只有 106 draw calls、`instances=63`。补上之后：

- `--cube-field 10000` 走外部实例注入（材质槽上限 256，一万个实体不可行），一万个立方体共享一个模板与一个槽位。
- 读数：`106.57 FPS`、`flattenUs 18871`（每帧 18.9 ms）、`instances 10063`、`visible 0.62`、`draw calls 2`。
  摊平时间就是 P2 要砍 70% 的那一段，现在有了对照物。
- `tools/bench.ps1` 增加 `cube-field` 场景与 `instances` / `flattenUs` 两列。
- 口径提醒：`visible` 仍是组级；一万个立方体合成一个组，组级剔除只能全剔或全留，实例级剔除是 P2 自己的活。
**T-04b（2026-11-23，默认开）**：把「只在本帧内读写」的目标合成一张（SSAO.Raw / SSAO / HDR.Scene / SSR / TAA.Output / Bloom 四级 / 平面反射 6 张 / 编辑器视口），并让 `submitGraphics` 多等一次上一帧的图形时间线。两件事必须成对：单独合并会被同步验证层抓到 `errors=120`（两帧真的重叠）。实测 lights-512 87.77 → 92.50 FPS（+5.4%）、图 160 → 145、`heap0` 1220.7 → 964.7 MiB、`--resize-test` 424 → 319；`--no-single-frame-targets` 整体回退，`--no-serialize-frames` 只用于复现竞争。官方基线重测为 121.57 / 103.66 / 94.56 FPS。
`格式 + 精确尺寸 + usage + mip/layer + flags`，`kRetireFrames=2`、`kMaxIdleBytes=256 MiB`；
`RenderGraph::createOwnedTexture` / `resizeOwnedTexture` 接上，`--rt-pool` 开、缺省关。

- 实测（60 帧 + `--resize-test` 六次尺寸变化）：池关 `images total=424`；池开 256 MiB
  `367`（命中率 24.8%）、512 MiB `360`（27.8%）；`heap0` 末值 1220.7 → 1476.7 MiB。
- **验收偏差**：原判据「稳定态每帧 `vkCreateImage` 为 0」在开工前就成立（默认场景
  `images/frame=0.27`、稳态 0），所以改成「一次尺寸变化的创建量」。池对这一条只给
  13~15%，代价 256 MiB，帧时间无可辨收益 → 默认关，判据改成 **44 → ≤25 张/次 resize**。
- 下一步的落点已经量出来：`[RTPool] top creators` 全是 `Frame0/Frame1` 成对的名字，
  说明 44 张里约一半是 `kFramesInFlight=2` 的复制；写在本帧读在本帧的目标不该双份。

**T-09（2026-11-23，已落地一半）**：计划里的 T-09 是「`MeshDrawCommand` + `StateCache`」两件事，这次只做了 `StateCache`（另一半是 T-08 的绘制命令列表，未开工）。落点是 `src/RHI/StateCache.{h,cpp}`：按 `VkCommandBuffer` 记当前管线，`Pipeline::bind` / `ComputePipeline::bind` 是唯一两个入口。实测 default 23.5 → 13.0 条/帧（省 44.7%）、lights-512 39.5 → 20.0（省 49.4%），`[StateCache]` 与 `[FrameStats]` 两表逐位相同。**验收偏差**：计划写「状态切换次数下降 ≥ 40%」已达（44.7% / 49.4%）；帧时间同批次交替 A/B（lights-512，600 帧 × 3，开关交替）中位数 93.92 对 91.76 FPS（+2.4%），幅度小但方向一致 —— 省的是 10~20 条驱动调用，本来不是瓶颈。第一次跑 `tools/Validate.ps1` 时 35 个地形/流式用例崩在 `0xC0000005`，原因是缓存对「每帧 reset、句柄不变」的一次性命令缓冲也记了账；改成只对 `trackCommandBuffer` 登记过的缓冲生效后全绿，`unmanaged` 归 0。回退开关是 `--no-pipeline-cache`。`MeshDrawCommand` 那半仍然有效，它要解决的是"每帧重建命令"而不是"重复绑定"。

**T-05（2026-11-23，默认关）**：`cullDeadPasses()` 六条根 + 反向闭包（带读位的访问才算依赖），跑在 `validateWriteCoverage()` 之后。实测默认 / `--pass-cull` / `--pass-cull --no-bloom` / 加截图与 sync 四种配置 `culled/frame` 全为 0，`errors=0`。

**验收偏差**：判据原文「`--no-bloom` 时图内 Pass 数 -7」在开工前已成立（21 → 14），归因是 Pass 按开关不进图，不是剔除。因此把 T-05 的定位改成「兜底工具」并默认关：省不下东西，却会因「读声明缺失」把上游误判成死 Pass（实现中撞到两次：LOAD 附件、导入资源），静默变黑且验证层不报错。
**T-07 第一步（2026-11-23，`--stats-queue`）**：先补了尺子再动结构。每条命令缓冲头尾各写一枚 timestamp（`src/RHI/QueueStats.{h,cpp}`，两条队列各管自己的一对 query，重置录进命令缓冲 —— 本机没开 `hostQueryReset`），退出时打印 `[Queue]`：

```
[Queue] frames=58 | makespan=8.65 ms | graphics busy=8.36 ms (96.7%) | compute busy=0.25 ms (2.8%) | overlap=0.00 ms (0.0% of compute, max 0.00 ms)
```

这条读数（512 灯 + `--clustered-gpu-build`，sync 验证层）：计算队列每帧只忙 0.25 ms，且与图形**零重叠**。原因不是调度器不给机会，而是同帧存在 c2g 依赖（建簇写簇表、光照读簇表），而每帧图形命令只有一次提交 —— 整条图形流都在等 compute。这一条把 T-07 的验收从「compute 与 graphics 有重叠」落到可测的具体数字上：现在的基线是 `overlap=0.00 ms`、`compute busy=0.25 ms`，要抬的就是 `overlap / compute busy`。

默认关且零开销：没有 `--stats-queue` 时 `Application` 直接 `queuestats::shutdown()`，一枚 timestamp 都不写（默认场景 `[FrameStats]` 三行与关闭前后逐位相同）。

**T-07 第二步（2026-11-24，已落地）**：图形拆成 `G0 -> C0 -> G1` 两段命令缓冲、三次提交。拆分点由图自己算（第一个「带读位且与 compute 写范围重叠」的 graphics Pass），三条保守规则：拆点为第 0 个 Pass 时不拆、`--no-async-split` 整体退回单次提交、没有 compute 写时提交路径与改造前完全一致。实测 512 灯 + sync：`makespan 10.06 -> 9.03 ms`、`overlap 0.00 -> 0.51 ms`（等于 compute 全部忙碌时间，100% 被遮住）；无验证层 600 帧 × 3 轮交替 `102.00` 对 `100.94` FPS（+1.05%）；默认场景 `passes/frame=16.0` 不变、截图差异在噪声内。**验收偏差**：本单收益上限本来就只有 compute 占比（2.5%），+1.05% 已是这个上限的一半。
