---
id: roadmap/borrowed-techniques
title: 可借鉴技术清单（TM 系列）
type: roadmap
scope: 空（跨代码库；候选改造点覆盖 src/RHI、src/Render、src/Core）
read-when: 决定下一项引擎基础改造做什么之前；或评估某个外部实现值不值得引入
size: ~262 行
updated: 2026-11-20
---

# 可借鉴技术清单（TM 系列）

## 1. 这篇解决什么

基础设施层"下一步抄什么"只在这里回答。每条都写明：现状证据（本仓库的路径与行号，或实测数字）、参考实现位置、落地步骤、可量化的验收判据、预估与风险。

边界：

- 功能需求（做什么玩法或画面能力）走 [roadmap](roadmap.md) 的 F1~F7；
- 缺陷与已欠下的账走 [tech-debt](../70-status/tech-debt.md) 的 TD 编号；
- 调试与"崩了怎么定位"的现状见 [diagnostics](../60-debug-profiling/diagnostics.md)。

"参考实现"一栏里的 `vulkan/...`、`renderer/...`、`util/...`、`threading/...`、`filesystem/...`、`event/...` 都是外部参考工程 Granite 的仓库内相对路径，不在本仓库里，点不开——给行号是为了照着读它的写法。

编号 TM-nn 分配后不再改号。做完的移到 §3，并在 architecture/decisions 落一条决策记录；本文件只留一句摘要加链接（同一事实只有一个出处）。

## 2. 排序判据

三道闸门，任一不满足就不排期：

1. **不破坏基线**：`stderr 0 字节`、`[Validation] errors=0 warnings=0`、截图类回归逐像素一致（`max diff = 0`）。
2. **有可量化的验收**：显存（`Device::reportVmaStats`）、帧率（`--selftest 60`）、每帧调用数（加计数器打印），三者至少一项要变，且事先写清变成多少。
3. **不新增第三方依赖**：3rdParty 已 vendored 且版本固定（见 [dependencies](../40-build/dependencies.md)），新依赖要另起一次决策。

风险中以上的条目必须带可整体回退的开关，照既有做法：`--no-hiz`、`--no-merged-buffers`、`--no-gpu-scene`。

| 层 | 含义 | 典型特征 |
|---|---|---|
| T1 | 立即可做 | 纯新增工具或纯重构，不改渲染行为，随时能插空做 |
| T2 | 要改热路径 | 触及命令缓冲、屏障、描述符、每帧分配 |
| T3 | 功能缺口 | 要新 Pass 或新子系统，得排在某个 F 阶段里 |

建议顺序是 T1 全做完，再按编号做 T2、T3。T2 里 TM-06 与 TM-08 抢同一块时间：当时是"PSO 太多"就先 TM-06，是"显存不够"就先 TM-08。

## 3. 已完成

| ID | 内容 | 关键结论 | 决策记录 |
|---|---|---|---|
| TM-01 | 管线缓存持久化 | 磁盘头过三道校验（驱动 `pipelineCacheUUID` → 载荷长度 → FNV-1a），原子落盘（`.tmp` + remove + rename），目录不可写退回工作目录；桌面 N 卡实测冷 102.8 / 热 98.9 FPS，**无收益**，价值在移动端与将来 `VK_KHR_pipeline_binary` | 决策 24（[decisions](../10-architecture/decisions.md)） |
| TM-02 | GPU breadcrumbs | 三档降级 NV checkpoint → AMD marker → `vkCmdUpdateBuffer`；4 槽（2 帧在飞 × graphics/compute）；崩溃后读 HOST_VISIBLE 映射得"第几帧 / 哪个 Pass"；每条 marker 前有只覆盖 4 字节的 TRANSFER_WRITE 屏障 | 第 9 节（[diagnostics](../60-debug-profiling/diagnostics.md)） |
| TM-03 | 环形临时表（TemporaryHashmap） | `src/Core/TemporaryHashmap.h` + `src/Core/IntrusiveList.h`（RingSize 个环、固定桶侵入式哈希、对象池；命中老环时把条目提升到当前环）。**关键坑**：不做提升时常用 key 会每 `RingSize` 帧被销毁重建 —— `GlassPass` 接入前实测 30 帧重建 15 次（缓存每两帧失效一次，比不用缓存更糟），修好后 30 帧只重建 1 次。已接入 `GlassPass::refreshChunkStatesAndRebuild`（`src/Render/GlassPass.cpp`） | 决策 25（[decisions](../10-architecture/decisions.md)） |
| TM-04 | 对象池（ObjectPool） | `src/Core/ObjectPool.h`：64 个对象起步、每开新块翻倍的 aligned 块 + vacants 栈 + placement new；契约是"必须显式 `destroy()`，池不管生命周期"，Debug 下析构时仍有存活对象直接断言。TM-03 的环回收就靠它归零 | 决策 25 |
| TM-05 | 设备能力与厂商怪癖表（DeviceQuirks） | `src/RHI/DeviceQuirks.h/.cpp`：一次采集厂商 / 设备类型 / subgroup 支持阶段与操作 / 独立 compute 队列，推出 `asyncComputePost`（核显关）、`subgroupOpsUsable` 等策略位；启动打印 `[Device] quirks: NVIDIA | 独立显卡 | subgroup={size=32 stages=0x3FFF shuffle=1} | 异步compute=1 | 独立compute队列=1`。参考实现里与 subpass/transient 相关的字段未搬（本项目用 dynamic rendering，没有 subpass 可合并） | 决策 25 |

TM-01 与 TM-02 各留了一条尾巴，都不阻塞：**统计口径**（`--stats-pipelines` 打印 PSO 命中/新建）与**粒度**（breadcrumbs 目前到 pass 级，到 draw 级要几百条 marker/帧）。前者已随 TM-06 落地（`--stats-pipelines`，见 [diagnostics 第 10 节](../60-debug-profiling/diagnostics.md)），后者并入 TM-02 的后续。

## 4. T1：已清空

T1 的三项（TM-03 环形临时表、TM-04 对象池、TM-05 设备怪癖表）已于 2026-11-19 完成并移入 §3，这一档不再有待办。

**清单已收口**：TM-06（统计部分）、TM-07、TM-08（第一步）、TM-10、TM-12、TM-13（两步）、TM-16、TM-17、TM-18、TM-19 均已落地；
TM-09（跨队列 EXCLUSIVE 所有权转移）、TM-11（描述符三档）、TM-06 的按需建管线、TM-08 的第二步、TM-14（SPD 降采样链）、TM-18 的实际启用，
均经数据或结构判定不做，理由见[决策 28 / 29 / 27 / 35 / 39](../10-architecture/decisions.md)。
唯一还没开工的是 **TM-15（场景排序与批处理）**，它的触发条件是"前向 Pass 超过 5 个或透明物体超过 2000"，当前都没到。


## 5. T2：要改热路径

### TM-06 图形状态打包 + 按需建管线

- **状态**：第一步（状态字 + `--stats-pipelines` 统计）已完成，见 [diagnostics 第 10 节](../60-debug-profiling/diagnostics.md)；**第二步（按需建管线）经数据判定不做**，理由见下面的结论。
- **现状**：22 个 `*Pass.cpp` 各自在 init 里手写 `PipelineConfigInfo`（`src/RHI/Pipeline.h:7-32`，含 viewport / scissor / rasterization / multisample / `vector<VkPipelineColorBlendAttachmentState>` / depthStencil / pipelineLayout / `vector<VkFormat> colorAttachmentFormats` / binding / attribute / dynamicStates），`Pipeline` 与 `ComputePipeline`（`src/RHI/Pipeline.h:34-55`、`:59-77`）一对一包一个 `VkPipeline`。既没有"状态 → 管线"的查表，也没有绘制时按需编译（grep `unordered_map<...VkPipeline`、`stateKey` 在 `src/RHI/` 下 0 命中）。
- **参考**：`vulkan/command_buffer.hpp:127-144` 的 `CommandBufferDirtyBits`；`:154-207` 的 `union PipelineState { struct { 位域 } state; uint32_t words[4]; }`（整套图形状态压成 16 字节，可直接 memcmp / hash）；`:275` 的 `DeferredPipelineCompile`；`:868 extract_pipeline_state()`、`:876-878 build_graphics_pipeline / build_compute_pipeline / flush_pipeline_state_without_blocking()`。
- **落地**：① 定义 `GraphicsStateKey`（定长、可 memcmp、`static_assert(sizeof(...) == N)`）；② 新增 `PipelineLibrary::getOrCreate(key)`，内部 `unordered_map` + `device.getPipelineCache()`（TM-01 已就位）；③ 每个 Pass 从"持有 Pipeline"改成"提供 makeKey()"，绘制时才取；④ 加命中 / 新建计数，接 `--stats-pipelines`。
- **已完成**：`src/RHI/PipelineStats.h/.cpp`（`GraphicsStateKey` 定长、逐字段 FNV-1a、8 个语义字段各进哈希、`pipelineLayout` 也参与）；`Pipeline` / `ComputePipeline` 建管线时计时并登记；`--stats-pipelines` 退出时打印数量 / 耗时 / 状态去重。本机实测（RTX 2070，`--selftest 30`）：图形 30 条 + compute 8 条、68 个模块、0.41 MB SPIR-V，冷启动 total=50.5ms、热缓存 total=24.7ms（0.65ms/条），30 条图形管线只有 26 种状态、完全相同的管线 0 条。
- **结论**：桌面有驱动磁盘缓存时，整个启动期建管线只有二十几毫秒，且没有可合并的重复管线，按需建管线的收益小于它带来的风险（要把每个 Pass 从持有 Pipeline 改成提供 makeKey，碰所有初始化顺序）。这条留着，等出现管线成百上千条或移动端冷启动时再启用。
- **验收（已满足的部分）**：`--stats-pipelines` 打印数量 / 耗时 / 状态种类 / 完全相同的管线；不带该开关时零开销（输出与基线逐字节一致、stderr 0 字节）；验证层 0 errors。
- **预估**：2~3 天，风险中高（碰所有 Pass 的初始化顺序，必须一次只迁一个 Pass）。

### TM-07 每帧暂存（scratch）线性分配器

- **状态**：已完成（暂存区 + 第一个消费者 SSAO），见 [resource-lifetime 第 8 节](../10-architecture/resource-lifetime.md)。
- **现状（当初的判断是错的，以这段为准）**：`vkCmdUpdateBuffer` 全仓库只有 breadcrumbs 在用（`src/RHI/Breadcrumbs.cpp:190`），`ShadowPass` 走的是 push constant —— 所以原验收口径「ShadowPass 每帧 vkCmdUpdateBuffer 降到 0」是个空指标。真实问题是：每个 Pass 各自建 `HOST_VISIBLE | MAPPED` 的每帧 buffer（`Application` 全局 UBO、`FogVolumePass`、`ReflectionPass`、`SSAOPass`、`ParticlePass`、`VegetationPass` 的实例与骨骼、`EditorOverlayPass`），各自 memcpy + flush，而且尺寸写死 —— `src/Render/EditorOverlayPass.cpp:103` 的 `bufferSize = sizeof(glm::vec3) * 48` 只够两个盒子，多一个就越界。
- **参考**：`vulkan/command_buffer.hpp:522/530/532/534`（`allocate_constant_data` / `allocate_vertex_data` / `allocate_index_data` / `update_buffer`）；实现细节 `vulkan/command_buffer.cpp:2416-2423`（`ubo_block.allocate(size)` 失败就 `device->request_uniform_block(ubo_block, size)` 扩容后重试）、`:2519-2526`（顶点同型）、`:4551-4556`（退出时归还）。
- **落地**：① 每帧每个槽位一个大块，bump offset，帧首归零；② uniform 分配必须按 256 对齐（`minUniformBufferOffsetAlignment` 的上限），顶点 / 索引用 16；③ 扩容时旧块不能立刻销毁（本帧早先的 `Block` 还指着它），挪进 retired、等下次轮到这个槽位再回收；④ 描述符必须带 `Block.offset` 每帧重写。
- **已完成**：`src/RHI/ScratchArena.h/.cpp`（`Device` 持有，`beginFrame` / `allocate` / `flushIfNeeded`，由 `RenderGraph::beginFrame` 与 `endFrame` 调）；第一个消费者是 `SSAOPass`（每帧 256 对齐分配一段 `SSAOUbo`，描述符每帧写偏移，原来的 `uboBuffers[2]` 三件套删除）；`--stats-scratch` 打印帧数 / 槽位容量 / 分配次数与字节 / 峰值偏移 / 扩容与刷新次数。实测（`--selftest 30`）：2 × 1.00 MiB 槽位、1.0 次分配/帧、1.1 KB/帧、峰值 1.2 KB、扩容 0 次、刷新 0 次（内存是 COHERENT，30 帧全部跳过）。
- **结论**：收益不在每帧几微秒，而在三件结构性的事 —— 帧内分配可见、写死尺寸的自建 buffer 不再散落各 Pass、新消费者不必再造一份 VMA 分配。剩余消费者（`FogVolumePass` / `ReflectionPass` / `ParticlePass` / `VegetationPass` / `EditorOverlayPass` / 全局 UBO）按「下次改到它时顺手迁」推进，不做一次性大改：每个都牵描述符写入时机，一次全迁会让一次像素回归极难归因。
- **验收（已满足的部分）**：`--stats-scratch` 给出分配次数 / 字节 / 扩容 / 刷新；`--selftest 30` exit=0、stderr 0 字节、验证层 0 errors；与上一版截图逐像素比对，差异只有 `Performance` 面板里的 FPS 读数（333 像素，全在 x 95..157 / y 119..129 那一小块），3D 画面完全一致；`--selftest 60` 三次 86.10 / 98.00 / 94.65 FPS，与改动前的 91.99 / 99.52 / 112.62 落在同一噪声区间。
- **预估**：2 天，风险中（实际只用了半天，因为第一个消费者只有一段 UBO）。

### TM-08 渲染图资源别名化

- **现状**：`src/RHI/RenderGraph.h:14-35` 自述"线性渲染图、Pass 严格按声明顺序、**不做重排 / 别名化 / 自动剔除**"，注释里写着"将来要加自动剔除 / 内存别名化时，这个结构也留了位置"；[known-limits](../70-status/known-limits.md) 第 36 行记为"决策 2"的边界。
- **参考**：`renderer/render_graph.cpp:628` 的 `// Find resources which can alias safely`；`set_physical_index` 分配物理索引，冲突即 `throw std::logic_error("Cannot alias resources. Index already claimed.")`（`:699/:726/:753/:791/:897`）；`physical_dimensions[]` 记 levels / flags / queues（`:785-786`）；输入附件最后分配，好与同一 subpass 的 color / depth 别名（`:917-918`）；`add_fake_resource_write_alias`（`:345/:933/:1171`）。
- **状态**：第一步（生命周期区间 + `--alias-report` 只读报告）已完成；第二步（真打开复用）经数据判定不做。
- **已完成**：`src/RHI/RenderGraphAlias.h/.cpp`（`tiny::aliasreport`）——`Resource`（名字 / 字节 / `firstWrite` / `lastUse` / 格式 / 尺寸 / `owned` / `transient`）、`pack()`（按 `firstWrite` 升序线性扫描、best-fit，只有格式 / 尺寸 / mip / 层数全同才允许落在一起）、`report()`、`estimateBytes()`、`selfTest()`；`RenderGraph` 在 Pass 循环里用 `lastFrameExecutedPasses.size() - 1` 当 Pass 序号（跳过的 Pass 不占号），`endFrame` 末尾把本帧清单交给报告模块，开关关掉时整段不执行。
- **结论（本机 `--selftest 30 --alias-report`，1920x1080）**：
  - 渲染图自己持有的每帧目标只有 8 个、合计 40.8 MiB，生命周期全部重叠，**可省 0**。
  - 真正占显存的是 13 个每帧重建但**导入**的目标（合计 181.9 MiB，GBuffer 的 5 张附件与深度都在这里）：显存不归渲染图管，别名化在它们身上无从谈起。
  - 另有 4 个常驻导入目标（229.4 MiB，IBL cubemap 一类），本来就不该参与。
  - 明细举例：`HDR.Scene.Frame1 [Pass 8..11]`、`TAA.Output.Frame1 [Pass 11..19]`、`Bloom.Frame1.Level0..3 [Pass 12..19]` —— TAA 输出与整条 bloom 链全程重叠，而且 Pass 11 同时读 HDR、写 TAA，连首尾相接都不成立。
- **要真省，得先满足三条前提**（这是第二步不做的理由）：① 让渲染图持有 GBuffer 与深度的显存（现在由 `src/Render/GeometryPass.cpp:33` 建好后 `importTexture`）；② 让 bloom 链与 TAA 的生命周期错开（现在是嵌套的 up / down sample 链）；③ 放宽并验证格式兼容规则（现在要求同格式同尺寸）。
- **验收（已满足的部分）**：`--alias-report` 给出候选对数与预估节省字节（0）；`[Alias] selfTest PASS | 生命周期装箱 + 形状兼容 + 字节估算`；关掉开关时零开销（`--selftest 30` 输出与基线一致、`[Diag] zero-overhead guard PASS`）。
- **预估**：报告部分 1 天（已完成）；真打开复用 5~7 天，且要先做上面三条结构改造，风险高。
- **注意**：本项目是 dynamic rendering、没有 `VkRenderPass`，别名判据只用 pass 顺序加读写集合，比参考实现（还含 subpass 合并）简单一档 —— **先做别名，不碰 subpass 合并**。

### TM-09 跨队列 EXCLUSIVE 所有权转移

- **状态**：已评估，本结构下不做。release / acquire 的全套通路实现过并跑通了探针，但清不掉验证层错误，根因是单提交结构表达不了双向转移 —— 证据与报文见[决策 28](../10-architecture/decisions.md)。
- **现状**：[weak-points](../10-architecture/weak-points.md) 第 1 条与 [known-limits](../70-status/known-limits.md) 第 33 行：跨队列资源走 `CONCURRENT` sharing，未实现 ownership transfer；RenderGraph 用 `VK_SHARING_MODE_CONCURRENT` 加 timeline semaphore 串依赖。
- **参考**：`vulkan/command_buffer.hpp:428-439`（`release_/acquire_buffer_barrier`、`release_/acquire_image_barrier`）；`renderer/render_graph.hpp` 的 `RenderPassExternalLockInterface`（`external_acquire_semaphore` / `mark_access_in_queue` / `acquire_internal` / `release_internal` / `inline_queue_invalidate` / `inline_queue_flush`），`uses_semaphore()`（`:223-233`）判断这次访问是否真跨物理队列。
- **做法（已回退，代码不保留）**：`crossQueue` 资源按 EXCLUSIVE 建（`queueFamilyIndexCount = 1`），逐 subresource 记 `ownerQueue` / `releasedFrom`，跨队列访问时发 release 加 acquire，帧末把仍持有的资源 release 回另一条队列。
- **结论**：不实现。一次所有权转移只能沿提交方向发生（release 必须落在源队列已提交的命令流里，而提交顺序由数据依赖方向决定，见 `src/RHI/Renderer.cpp:317-328`）；`--cross-queue-test` 的探针同一帧里两个方向都要，必然有一半逆序。单向跨队列在原理上可行，但当前没有单向的跨队列消费者，按 §2 的闸门（必须有可量化验收）不满足条件。
- **验收（已满足的部分）**：CONCURRENT 路径 `--selftest 30 --cross-queue-test --validation-features sync` 得到 errors=0 warnings=0、stderr 0 字节、`[TD02] graphics->compute verified: frame 1 checksum 4096` 与 `[TD02] compute->graphics verified: frame 2 checksum 2218407808` 两向都通过、61.4 FPS。
- **预估**：3~4 天，风险高；现已确认不只是难，而是与单提交结构冲突 —— 真要做得先把渲染图改成每队列多提交，那属于架构级改动。

### TM-10 Pass 准备阶段并行

- **状态**：已完成（拆分已落地，并行经数据判定不做）。
- **现状（当初的判断逐条修正）**：JobSystem 确实已有 `parallelFor` / `submitAfter`（`src/Core/JobSystem.h:93/99/108`），但"准备阶段耗时值得并行"这个前提是错的 —— 21 个 Pass 的准备（去重资源访问 + 校验声明 + 组装附件描述）合计只有 36 us/帧，约占一帧的 0.3%。
- **已完成**：`RenderGraph::endFrame` 拆成"串行挑 Pass → `preparePass()`（纯数据，错误只记进 `PreparedPass::error`）→ 串行按 Pass 顺序抛错 → 串行录制"四段；准备阶段可选择走 `JobSystem::parallelFor`（`--parallel-prepare`，默认关闭）；`--stats-rendergraph` 打印两段各自的每帧耗时。详见[决策 29](../10-architecture/decisions.md)与[诊断 12](../60-debug-profiling/diagnostics.md)。
- **结论（本机 RTX 2070，1920x1080，各 3 次）**：准备阶段串行 35.5 / 35.9 / 36.9 us，并行 176.9 / 203.2 / 207.7 us —— 并行慢 4.9~5.8 倍；录制时间（1435~1532 us）不变。原因是把 21 个任务丢进工作窃取池再等它们完成的开销（160 us 量级）远大于被并行的工作量。**因此准备阶段保持串行**，开关留着只为可复现对照。
- **验收（已满足的部分）**：`[RenderGraph] pass bookkeeping selfTest PASS | executed=21 skipped=0` 不变；两种模式下 `[Validation] errors=0 warnings=0`、stderr 0 字节；截图与改动前逐像素一致（差异只落在 HUD 的 FPS 读数区域）。
- **什么时候重新评估**：准备阶段重到几十微秒/Pass（例如真的在准备阶段做 SPIR-V 反射或管线状态推导），或者 Pass 数量涨到上百个。

### TM-11 描述符的混合模型

- **状态**：已评估，保持现状；触发条件已写进 [descriptors](../10-architecture/descriptors.md) 第 10 节（[决策 31](../10-architecture/decisions.md)）。
- **现状（含实测）**：bindless 材质已是 1 个 set 4 个纹理数组加 push constant（desc binds 104 → 1，见 [descriptors](../10-architecture/descriptors.md)）。全仓库 `vkUpdateDescriptorSets` 31 处调用点，但每帧只有 12~15 次、每次 1~2 个 binding；`UPDATE_AFTER_BIND` / `vkCreateDescriptorUpdateTemplate` / `vkCmdPushDescriptorSet` / `VK_EXT_descriptor_buffer` 全部 0 命中。CPU 侧每帧十几微秒。
- **参考**：`vulkan/descriptor_set.hpp`（per-type 位掩码 + `ArraySizeAccessMeta meta[VULKAN_NUM_BINDINGS]` + `UNSIZED_ARRAY = 0x7f`；`VULKAN_NUM_SETS_PER_POOL = 64`、`VULKAN_DESCRIPTOR_RING_SIZE = 16`）、`vulkan/descriptor_set.cpp:39`（`bindless = layout.meta[0].array_size == UNSIZED_ARRAY`，同一个 allocator 自动切 bindless）、`:388-395`（`descriptorPoolOverallocation` 可用则加 `ALLOW_OVERALLOCATION_POOLS/SETS_BIT_NV` 复用 pool）、`:215-230`（`vkGetDescriptorSetLayoutSizeEXT` / `vkGetDescriptorSetLayoutBindingOffsetEXT` 记 desc buffer 偏移）。
- **落地**：保持现状；出现压力时按"池化 set → push descriptor → descriptor buffer"逐档上。三档各自的触发条件、成本与参考实现行号见 [descriptors](../10-architecture/descriptors.md) 第 10 节。
- **验收（已满足的部分）**：触发条件写成可判定的数字；描述符相关文档与实际调用点数一致。
- **预估**：3 天，风险中（当前不排期）。

### TM-12 把反射结果变成构建产物

- **状态**：已完成（[决策 30](../10-architecture/decisions.md)，用法见 [shaders](../20-pipeline/shaders.md) 第 6 节）。
- **现状（落地前）**：shader 由 `glslangValidator` 预编译加 `spirv-val` 静态校验（`CMakeLists.txt`），运行期不做反射；binding 号靠手写常量与约定对齐，改一个 binding 漏改 C++ 常量时要等验证层报错。
- **参考**：`vulkan/shader.cpp:781-817`（反射结果的缓存与复用）。
- **已完成**：`tools/SpirvReflect.cpp`（自己解析 SPIR-V，不依赖外部工具的块大小口径）生成 `build/shaders/<name>.reflect.h`（set / binding / count、块大小与成员偏移、stage、entry point）；CMake 每个 shader 两条 custom command（`.spv` 一条、`.reflect.h` 依赖 `.spv` 一条 —— VS 生成器对多 OUTPUT 的增量判断不可靠）；`build/shaders` 进 `tiny_core` 的 include，并 `add_dependencies(tiny_core TinyShaders)`；SSAOPass 已接入（`static_assert` 对齐 `sizeof` / `offsetof` / 数组长度 / set 号，binding 全走生成常量）。
- **结论**：52 个 shader 全部生成成功，与 `spirv-reflect` 的 15 处差异复核后全是口径差异；漂移实验（在 `ssao.frag` 插一个 `vec4`）被 6 条 `static_assert` 当场拦住。
- **验收（已满足的部分）**：改 GLSL 结构体不改 C++ → 编译期报错（`error C2338: static_assert failed`）；`--selftest 30` exit=0、stderr 0 字节、`[Validation] errors=0 warnings=0`；`--selftest 60 --gpu-scene` 91.17 FPS。
- **已知边界**：std140 尾部填充、变长数组块只给偏移、全局 set 0 仍由 `DescriptorManager` 单点定义。
- **预估**：2 天，风险低（实际约一天半）。
## 6. T3：功能缺口

### TM-13 Clustered lighting

- **状态**：已完成（第一步 + 第二步落地；默认关闭，理由见结论）
- **现状（当初的判断需要补一句）**：`LightingSystem.cpp` 确实没有 cluster 结构，但更要紧的是**引擎此前没有点光源** —— `ubo.numLights` 恒为 0，`lighting.frag` 的逐灯循环是死代码。所以这一步先补灯，再谈聚簇。
- **已完成**：`LightSources`（灯数组 + `--light-stress` 压力场）、`ClusteredLighting`（16x9x24、每簇 256 上限、按灯剪枝 + 计数排序、`selfTest` 验反投影 / 球盒相交 / 切片区间 / 确定性）、Set 0 新增 binding 15~18、`lighting.frag` 的双路径与 `ComputeClusterIndex`、`--clustered-lighting` / `--stats-clusters` 开关。
- **结论（本机 RTX 2070，Debug）**：8 灯 —— 逐灯 65.8 / 71.0 FPS，聚簇 61.4 / 58.6 FPS；512 灯 —— 逐灯 69.4 / 64.5 FPS、聚簇 28.3 / 28.8 FPS（最挤的簇 202 盏）。建簇本身不贵（Release 512 灯 1.0 ms），贵的是「灯太密时每片元要遍历 200 盏」。默认关闭，等「总数上千、每簇几盏」的场景再翻。
- **验收到什么程度**：簇表用暴力全扫描交叉验证（3456 个簇里 9 个因切片边界少算 1 盏）、两种模式 `--validation-features sync` 均 0 errors、8 灯与 512 灯均不崩。**没有做到**「与旧路径逐像素一致」—— 逐灯路径本来就一盏都不亮（衰减 1/(d²+1) 在这些位置上视觉不可见），没有可比对的基准。这条留在决策 32 里说明。
- **何时重新评估**：场景里出现「几百上千盏灯、每盏只影响小块区域」时；或要把阴影 / 雾 / 贴花也挂到同一份簇表上时。
- **预估**：8~10 天，风险高。

### TM-14 单趟降采样（SPD）

- **现状**：Bloom 是 bright + 3 级 down + 3 级 up（`src/Render/BloomPass.cpp`），每级一次全屏 pass。
- **参考**：`renderer/post/spd.cpp`（9.4 KB，single-pass downsampler）。
- **落地**：down 链换成单次 dispatch 的多级写法（compute + 每级 subgroup 归约），up 链暂不动。
- **已完成**：基线量测（`--bloom --gpu-timing`，40 帧平均，1920x1080）：`Bloom.Bright 0.1325 ms`、
  `Down0 0.0332`、`Down1 0.0102`、`Down2 0.0056`（**down 链合计 0.0490 ms**）、`Up0 0.0362`、`Up1 0.0113`、`Up2 0.0064`
  （up 链 0.0539），Bloom 总计约 0.235 ms/帧，而同一帧 `Cloud 2.3945`、`Geometry 0.9806`、`Lighting 0.9752`。
- **结论（本机，Debug 构建）**：down 链三级里 `Down2` 只有 16x9 像素却仍花 5.6 µs，说明单级固定开销约 5 µs、
  真正的 13 抽样工作量集中在 `Down0`（33 µs）。合并成单次 dispatch 最多省掉两个 pass 的固定开销（约 10 µs/帧 = 一帧的 0.09%），
  而 13 抽样与双线性必须原样搬进 compute 才能保住 `max diff <= 1/255`，改动量与收益不成比例。
  另外实测 `--stats-rendergraph` 的录制时间是 1412~1591 µs/帧，但 21 个 pass 与 23 个 pass 的录制时间没有可见差异 ——
  "少两个 pass 能省 CPU 录制"这条前提在本引擎上也不成立。
- **何时重新评估**：Bloom 链本身涨到 1 ms/帧以上（例如 4K + 更多级、或加 DOF/SSR 共用同一条金字塔），
  或者真的要为 F6 做 Compute Bloom（那时 SPD 是顺带的中间产物，而不是为了省 10 µs 单独做）。
- **预估**：2~3 天，风险中。**关联**：F6 的 Compute Bloom 项。

### TM-15 场景排序与批处理三段式

- **现状**：F1 已完成 GPU 驱动提交（draw call 104 → 2、合并缓冲、Hi-Z，见 [gpu-driven-submission](../10-architecture/gpu-driven-submission.md)），CPU 侧排序批处理已不是瓶颈；剩余价值只在透明与前向通道（Water / Particle / Glass 各自已在内部排序）。
- **参考**：`renderer/renderer.cpp`（`AbstractRenderable::get_render_info` → `RenderQueue` → flush 的排序 / 批处理 / 提交三段分工）。
- **落地**：暂不排期。触发条件：前向 Pass 超过 5 个，或单帧透明物体超过 2000。
- **预估**：2 天，风险低。

### TM-16 资产路径协议与热重载

- **状态**：已完成（决策 37）。
- **已完成**：`src/Core/AssetPath.{h,cpp}` 的 `resolve()`（构建期路径优先、失效才按锚点回溯）+ `asset://`/`shader://`/`config://` 协议；接入 `Pipeline::readFile`、`Texture` 的 `stbi_load`/`stbi_loadf`、`GLTFLoader` 的 tinygltf 入口、`EngineConfig` 配置路径；`--print-assets` 打印三个根与构建期宏状态。
- **已完成（热重载）**：`src/RHI/ShaderReload.{h,cpp}` 轮询 `.spv` 的 mtime，`Pipeline`/`ComputePipeline` 自己登记与注销，`--hot-reload` 打开；实测改 `cloud.frag.spv` 后日志出现 `[HotReload] 重建管线：…/cloud.frag.spv` 并继续跑。
- **结论**：只监控 `.spv`、不运行期编译 GLSL；轮询而不是目录通知（几十次 stat 的代价换掉线程与句柄时序）。
- **已知边界**：只重建管线与模块，改 binding 数量/附件格式要重启；"原树被完全移走"的实验里 shader/纹理/glTF/配置都能找到，但退出时仍撞既有的 VMA teardown 断言（与路径解析无关，见 known-limits）。
- **验收（已满足的部分）**：`--print-assets` 三个根正确；`--selftest 30` 与 `TinyEditor --selftest 5` 的 `[Assets]`/`[HotReload]` 自检 PASS、验证层 0 errors；`--hot-reload` 下改 `.spv` 会重建。
- **关联**：F4 资产管线。

### TM-17 latched 事件

- **状态**：已完成（只做 latched 一类）。
- **现状（当初的判断）**：没有事件层，初始化顺序靠 `Application` 构造函数里的手工次序。
- **已完成**：`src/Core/EventBus.h/.cpp`（latched 总线 + RAII 订阅）+ `src/Core/AppEvents.h`
  两个事件（`SwapchainRecreated`、`RenderTargetsResized`）；删掉三处手工调用
  （`screenshotCapture.onResize`、`renderPipeline.resizeAll` 两条路径）；
  `--print-events` 打值与订阅数；自检四边界（补发 / 退订 / 顺序 / 回调里退订）。
- **结论**：latched 语义正好解「后订阅者也要知道已经发生的事情」，而不必引入
  输入事件那套分发/消费；`Renderer::onSwapchainRecreated()` 仍显式调用，因为
  信号量必须先于广播就位（见[决策 38](../10-architecture/decisions.md)）。
- **验收（已满足）**：`--selftest 60 --resize-test --validation-features sync`
  （4 次交换链重建 + 2 次动态分辨率）`errors=0`、stderr 0 字节；
  `--resize-test --screenshot` 在 5 次重建后仍写出 1920x1080 PNG。
- **不引入**：immediate 类（键盘/鼠标）、事件优先级、跨线程发布。

### TM-18 transient attachment

- **状态**：已完成（只做能力探测，不改分配）。见[决策 39](../10-architecture/decisions.md)。
- **现状（当初的判断）**：`src/RHI/` 下 `LAZILY_ALLOCATED`、`TransientAttachment`、`ImageDomain` 均 0 命中；所有中间目标都是普通 device-local image。
- **参考**：`vulkan/render_pass.hpp:240 TransientAttachmentAllocator`、`:248 request_attachment(width, height, format, index, samples, layers)`；`vulkan/image.hpp:252-255` 的 `enum class ImageDomain { ..., Transient }`、`:358 transient_render_target()`；`RenderPassOpFlags` 里的 `ENABLE_TRANSIENT_STORE/LOAD_BIT`。
- **已完成**：`src/RHI/TransientProbe.h/.cpp` + `--probe-transient`。判据只有两条 ——
  ① 驱动接受用法组合（`vkGetPhysicalDeviceImageFormatProperties` 用 `TRANSIENT_ATTACHMENT | COLOR_ATTACHMENT | INPUT_ATTACHMENT`；
  深度目标换 `DEPTH_STENCIL_ATTACHMENT`）；② 设备上存在 `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` 的内存类型。
  **本页原先写的 `VK_FORMAT_FEATURE_LAZILY_ALLOCATED` 并不存在**（第一次实现就撞 `error C2065`），已按代码改正。
- **结论（本机 RTX 2070 + AMD 核显）**：6 个候选格式第 ① 条全部"接受"，第 ② 条 **0 个**惰性内存类型 →
  桌面端没有任何收益：没有 tile memory 可省带宽，驱动还会把 `TRANSIENT_ATTACHMENT` 退化成普通分配。
- **验收（已满足）**：`--probe-transient` 打印 6 行表（目标 / 格式 / 用法 / 结论）+ "本设备 LAZILY_ALLOCATED 内存类型：0 个"；
  `--selftest 30` 里 `[Transient] selfTest PASS`、`errors=0`、stderr 0 字节。
- **何时重新评估**：出现移动端 / 带 tile 的集显目标，或 WSI 换成 Android surface；届时才改分配器，
  并按 TM-08 的生命周期区间挑目标。

### TM-19 格式与能力表

- **现状**：`Device::findSupportedFormat` / `findDepthFormat` / `findMemoryType` 按需查询，没有集中的格式能力表；G-Buffer 的 5 个格式定义在 `GBufferFormat`（`src/RHI/GBuffer.h`）。
- **参考**：`vulkan/format.hpp`（格式特性与兼容表）、`vulkan/limits.hpp`、`vulkan/type_to_string.hpp`（枚举转字符串，省掉到处手写 switch）。
- **落地**：把 G-Buffer 5 个格式加深度格式做成一张表，附 `VkFormatProperties` 与可打印名字；加 `--print-formats` 输出。
- **已完成**：新增 `src/RHI/FormatTable.h/.cpp`（`tiny::formats`：`formatName` / `bytesPerPixel` / `describe` /
  `query` / `print` / `selfTest`）；`--print-formats` 在 `run()` 最前面打印 10 个格式后直接退出（不渲染任何一帧）；
  G-Buffer 的四个格式仍由 `GBufferFormat`（`src/RHI/GBuffer.h`）单点定义，格式表只是读它。
- **验收（已满足）**：`TinyEngine.exe --print-formats` 输出「字节 / 颜色 / 深度 / 采样 / 存储 / blit源 / blit目标 / 线性」八列，
  可直接看出 `VK_FORMAT_R32_UINT` 不支持线性过滤、深度格式不是颜色附件等；`[Formats] selfTest PASS | 名字映射 + 字节数 + 能力位 + 未收录退化`。
- **已知边界**：只收录引擎实际用到的十几个格式，其余打印成 `VK_FORMAT_<数字>`（字节数 0）；只读 `optimalTilingFeatures`。
- **预估**：0.5 天，风险低。

## 7. 明确不借鉴

| 项 | 为什么不抄 |
|---|---|
| `math/` 重写 GLM | 参考工程重写它是为了编译时间（自述快 40~50%）；本项目 GLM 已全量接入，换数学层的收益抵不过一次性改动量 |
| 自研 `memory_allocator` | 本项目已用 VMA。只借一个概念：`vulkan/memory_allocator.hpp:302` 的 `memory_heap_is_budget_critical[]`，将来可接进 `TextureBudget` 作"紧预算先降分辨率"的判据 |
| 单文件巨型化（`device.cpp` 198.9 KB、`command_buffer.cpp` 171.6 KB） | 本项目单文件 400 行是硬规则；学它的机制，不学它的组织 |
| `VkRenderPass` + subpass | 本项目是 dynamic rendering，引入 `VkRenderPass` 会推翻 RenderGraph。只借它的"禁止合并清单"（`renderer/render_graph.cpp:1246-1350` 的 `should_merge`）作为将来真要合并渲染通道时的判据表 |
| 自家 ECS | 本项目已用 EnTT，30 个组件描述符已就位 |
| ocean FFT 实时化 | 属功能需求（画面），归 F 阶段，不进基础设施清单 |

## 8. 与 F 阶段的关系

| F 阶段 | 相关 TM | 关系 |
|---|---|---|
| F1 GPU 驱动渲染 | TM-09、TM-07 | TM-09 是 F1 的前置（roadmap 已写"EXCLUSIVE 未实现"）；TM-07 降 CPU 侧每帧提交成本 |
| F4 资产与场景管线 | TM-16、TM-12 | 热重载与 binding 校验都服务于资产迭代速度 |
| F6 渲染特性 | TM-13、TM-14、TM-06 | clustered lighting 是最大一块；SPD 让 Bloom 迁 compute；TM-06 是新 Pass 增多后的前置 |
| 任意窗口期 | TM-03、TM-04、TM-05、TM-17、TM-19 | 不改行为，随时可做 |

## 9. 维护方式

1. 状态就在本文件里：开工前把条目标成"进行中"，做完移到 §3 并写清关键结论。
   本页是基础改造的台账；[backlog](../70-status/backlog.md) 只管地形与规模化任务，两者不混。
2. 落地后在 [decisions](../10-architecture/decisions.md) 落一条决策（取舍、触发条件、反面结论），本文件只留摘要加链接。
3. 新增条目必须四件齐全：现状证据（路径:行或数字）、参考实现（路径:行）、可量化的验收判据、预估与风险。缺验收判据的条目不进本文件。
4. 前提被推翻的条目要删（例如本项目结构变了、或外部实现已改），不要留着凑数。
