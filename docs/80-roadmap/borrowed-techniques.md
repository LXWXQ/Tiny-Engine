---
id: roadmap/borrowed-techniques
title: 可借鉴技术清单（TM 系列）
type: roadmap
scope: 空（跨代码库；候选改造点覆盖 src/RHI、src/Render、src/Core）
read-when: 决定下一项引擎基础改造做什么之前；或评估某个外部实现值不值得引入
size: ~221 行
updated: 2026-11-19
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

TM-01 与 TM-02 各留了一条尾巴，都不阻塞：**统计口径**（`--stats-pipelines` 打印 PSO 命中/新建）与**粒度**（breadcrumbs 目前到 pass 级，到 draw 级要几百条 marker/帧）。前者并入 TM-06 的验收，后者并入 TM-02 的后续。

## 4. T1：立即可做

### TM-03 环形临时表（TemporaryHashmap）

- **现状**：需要"按帧存活、按 key 查、下一帧自动回收"的表时，只能在各自模块里手写 map 加清理：`RenderGraph` 的 `std::vector<std::unique_ptr<Pass>> passes`（`src/RHI/RenderGraph.h:443`）每帧 `reset` + `clear`（`src/RHI/RenderGraph.cpp:397-399`），`SubMeshCulling`、`TextureBudget` 各有一套。没有通用容器。
- **参考**：`util/temporary_hashmap.hpp:62` 的 `template <typename T, unsigned RingSize = 4, bool ReuseObjects = false>`；`:91 begin_frame()` 轮转 ring 并回收；`:106 T *request(Hash hash)`。内部是 IntrusiveList 的 ring 数组 + ObjectPool + IntrusiveHashMap。用法示例：`vulkan/render_pass.hpp:236` 与 `:266`（`FramebufferAllocator`、`TransientAttachmentAllocator` 都用 `TemporaryHashmap<Node, VULKAN_FRAMEBUFFER_RING_SIZE = 16, false>`）。
- **落地**：① 连同依赖一起搬：`util/intrusive_list.hpp`、`intrusive_hash_map.hpp`、TM-04 的 `ObjectPool`；② 至少接一处真实调用（建议 `SubMeshCulling` 的每帧缓存）；③ 补纯逻辑 selfTest（ring 轮转、request 命中与未命中、跨环不串）。
- **验收**：`[TempHashMap] selfTest PASS`；`--selftest 60` 帧率不回归；被接入模块每帧的 map 重建次数降到 0（加计数器）。
- **预估**：0.5 天，风险低。

### TM-04 对象池与每帧对象复用

- **现状**：`RenderGraph` 每个 pass 每帧新建对象（`std::make_unique<Pass>`）再销毁，名字是 `std::string`；21 个 pass 就是每帧 21 次堆分配加 21 次释放。
- **参考**：`util/object_pool.hpp` —— `num_objects = 64u << memory.size()` 分块 `memalign` 分配 + vacants 栈 + placement new，另有 `ThreadSafeObjectPool` 版本；同一思路在 breadcrumbs 侧是固定 64 字节的 checkpoint 对象（`vulkan/breadcrumbs.hpp:157`，`static_assert(sizeof(T) <= CheckpointObjectSize)`）。
- **落地**：① `Pass` 改成池化对象并用 IntrusiveList 串起来；② `std::string name` 换成定长 64 字节加截断（同 breadcrumbs 的 `kMaxLabelLength = 40`）；③ 加自检确认池不跨帧泄漏。
- **验收**：`--selftest 60` 期间 `Pass` 构造次数为 0（加计数器）；`[RenderGraph] pass bookkeeping selfTest PASS | executed=21 skipped=0` 不变；截图逐像素一致。
- **预估**：1 天，风险中（触及每帧路径）。

### TM-05 设备能力与厂商怪癖表（DeviceQuirks）

- **现状**：能力探测散在两处 —— `src/RHI/Device.cpp:446-455` 逐个 `deviceHasExtension` 后 `push_back`，`src/RHI/Device.h:94-98` 各留一个 bool（multiDrawIndirect / drawIndirectCount / deviceFault / nvCheckpoint / amdMarker）。没有"厂商怪癖"这一层。
- **参考**：`vulkan/quirks.hpp` 的 `struct ImplementationQuirks`（instance_deferred_lights、merge_subpasses、use_transient_color、use_transient_depth_stencil、queue_wait_on_submission、use_async_compute_post、render_graph_force_single_queue、force_no_subgroups、force_no_subgroup_shuffle、force_no_subgroup_size_control）与 `struct ImplementationWorkarounds`（emulate_event_as_pipeline_barrier、broken_pipeline_cache_control、force_host_cached、broken_present_fence），都是静态单例。
- **落地**：新增 `src/RHI/DeviceQuirks.h`（纯数据 + 一个 `probe(VkPhysicalDevice, const std::vector<const char*>&)` 纯函数），Device 持有 `DeviceQuirks quirks`，把现有 bool 搬进去但访问器名不变 —— 先改名不改行为。
- **验收**：纯重构：`--selftest 60` 输出与改前逐行一致（时间与帧率除外）；验证层 0 errors。
- **预估**：0.5 天，风险低。
## 5. T2：要改热路径

### TM-06 图形状态打包 + 按需建管线

- **现状**：22 个 `*Pass.cpp` 各自在 init 里手写 `PipelineConfigInfo`（`src/RHI/Pipeline.h:7-32`，含 viewport / scissor / rasterization / multisample / `vector<VkPipelineColorBlendAttachmentState>` / depthStencil / pipelineLayout / `vector<VkFormat> colorAttachmentFormats` / binding / attribute / dynamicStates），`Pipeline` 与 `ComputePipeline`（`src/RHI/Pipeline.h:34-55`、`:59-77`）一对一包一个 `VkPipeline`。既没有"状态 → 管线"的查表，也没有绘制时按需编译（grep `unordered_map<...VkPipeline`、`stateKey` 在 `src/RHI/` 下 0 命中）。
- **参考**：`vulkan/command_buffer.hpp:127-144` 的 `CommandBufferDirtyBits`；`:154-207` 的 `union PipelineState { struct { 位域 } state; uint32_t words[4]; }`（整套图形状态压成 16 字节，可直接 memcmp / hash）；`:275` 的 `DeferredPipelineCompile`；`:868 extract_pipeline_state()`、`:876-878 build_graphics_pipeline / build_compute_pipeline / flush_pipeline_state_without_blocking()`。
- **落地**：① 定义 `GraphicsStateKey`（定长、可 memcmp、`static_assert(sizeof(...) == N)`）；② 新增 `PipelineLibrary::getOrCreate(key)`，内部 `unordered_map` + `device.getPipelineCache()`（TM-01 已就位）；③ 每个 Pass 从"持有 Pipeline"改成"提供 makeKey()"，绘制时才取；④ 加命中 / 新建计数，接 `--stats-pipelines`。
- **验收**：`--stats-pipelines` 输出新建与命中数；首次运行新建 ≤ 22、第二次运行命中数等于首次新建数；截图逐像素一致；验证层 0 errors。
- **预估**：2~3 天，风险中高（碰所有 Pass 的初始化顺序，必须一次只迁一个 Pass）。

### TM-07 每帧暂存（scratch）线性分配器

- **现状**：没有统一的每帧常量 / 顶点 / 索引暂存（grep `scratch|RingBuffer|request_uniform_block|LinearAllocator` 在 `src/RHI/` 下 0 命中），各 Pass 自己管 staging 与 `vkCmdUpdateBuffer`。
- **参考**：`vulkan/command_buffer.hpp:522/530/532/534`（`allocate_constant_data` / `allocate_vertex_data` / `allocate_index_data` / `update_buffer`）；实现细节 `vulkan/command_buffer.cpp:2416-2423`（`ubo_block.allocate(size)` 失败就 `device->request_uniform_block(ubo_block, size)` 扩容后重试）、`:2519-2526`（顶点同型）、`:4551-4556`（退出时归还）。
- **落地**：① 每帧一个大块 `ScratchBuffer`（HOST_VISIBLE | HOST_COHERENT，bump offset）；② 帧首重置 offset；③ 先只把一个 Pass（建议 `ShadowPass`，它按灯逐次更新常量）从 `vkCmdUpdateBuffer` 换成 scratch 加带 offset 的描述符。
- **验收**：该 Pass 每帧 `vkCmdUpdateBuffer` 调用数降到 0（加计数器）；`--selftest 60` 帧率不回归；截图一致。
- **预估**：2 天，风险中。

### TM-08 渲染图资源别名化

- **现状**：`src/RHI/RenderGraph.h:14-35` 自述"线性渲染图、Pass 严格按声明顺序、**不做重排 / 别名化 / 自动剔除**"，注释里写着"将来要加自动剔除 / 内存别名化时，这个结构也留了位置"；[known-limits](../70-status/known-limits.md) 第 36 行记为"决策 2"的边界。
- **参考**：`renderer/render_graph.cpp:628` 的 `// Find resources which can alias safely`；`set_physical_index` 分配物理索引，冲突即 `throw std::logic_error("Cannot alias resources. Index already claimed.")`（`:699/:726/:753/:791/:897`）；`physical_dimensions[]` 记 levels / flags / queues（`:785-786`）；输入附件最后分配，好与同一 subpass 的 color / depth 别名（`:917-918`）；`add_fake_resource_write_alias`（`:345/:933/:1171`）。
- **落地**：① 先只算生命周期区间（first-write / last-read 的 pass 索引）并打印可别名候选对，**不改分配行为**；② 加 `--alias-report` 逐对校验；③ 确认无误后再打开复用，且一次只让一个中间目标别名。
- **验收**：`--alias-report` 给出候选对数与预估节省字节；打开后 `reportVmaStats` 的显存峰值下降、截图逐像素一致、`--validation-features sync` 0 errors。
- **预估**：5~7 天，风险高（必须有开关）。
- **注意**：本项目是 dynamic rendering、没有 `VkRenderPass`，别名判据只用 pass 顺序加读写集合，比参考实现（还含 subpass 合并）简单一档 —— **先做别名，不碰 subpass 合并**。

### TM-09 跨队列 EXCLUSIVE 所有权转移

- **现状**：[weak-points](../10-architecture/weak-points.md) 第 1 条与 [known-limits](../70-status/known-limits.md) 第 33 行：跨队列资源走 `CONCURRENT` sharing，未实现 ownership transfer；RenderGraph 用 `VK_SHARING_MODE_CONCURRENT` 加 timeline semaphore 串依赖。
- **参考**：`vulkan/command_buffer.hpp:428-439`（`release_/acquire_buffer_barrier`、`release_/acquire_image_barrier`）；`renderer/render_graph.hpp` 的 `RenderPassExternalLockInterface`（`external_acquire_semaphore` / `mark_access_in_queue` / `acquire_internal` / `release_internal` / `inline_queue_invalidate` / `inline_queue_flush`），`uses_semaphore()`（`:223-233`）判断这次访问是否真跨物理队列。
- **落地**：① 记录每次异步 compute 的 src/dst 队列族；② 在跨队列边界插 release / acquire（EXCLUSIVE），叠在现有 timeline 信号前后；③ 保留 `--cross-queue-test` 作正反例。
- **验收**：`--selftest 60 --validation-features sync` 0 errors；`--cross-queue-test` 通过；CONCURRENT 资源数下降（加计数器打印）。
- **预估**：3~4 天，风险高。

### TM-10 Pass 准备阶段并行

- **现状**：JobSystem 已有 `submitAfter(std::vector<JobHandle>, ...)`、`parallelFor`、`selfTest`（`src/Core/JobSystem.h:93/99/108`），但 RenderGraph 的 pass 准备（去重资源访问、推导屏障）仍是单线程顺序循环（`src/RHI/RenderGraph.cpp:171` 起）。
- **参考**：`threading/task_composer.hpp:36 begin_pipeline_stage()`、`:41 get_pipeline_stage_dependency()`、`:49 get_deferred_enqueue_handle()`（子任务自行派生任务时，下一阶段不提前开始）。
- **落地**：把 pass 循环拆成"收集 → 并行准备（纯数据，不碰 `VkCommandBuffer`）→ 串行录制"三段，前两段进 JobSystem。
- **验收**：21 个 pass 的准备耗时下降（用现有 `--selftest` 的计时口径对比）；`[RenderGraph] pass bookkeeping selfTest PASS | executed=21 skipped=0` 不变；截图一致。
- **预估**：2~3 天，风险中（并行边界先只覆盖纯数据段）。

### TM-11 描述符的混合模型

- **现状**：bindless 材质已是 1 个 set 4 个纹理数组加 push constant（desc binds 104 → 1，见 [descriptors](../10-architecture/descriptors.md)）。**当前没有压力**，只在出现"每帧新建 set 组合"时才有价值。
- **参考**：`vulkan/descriptor_set.hpp`（per-type 位掩码 + `ArraySizeAccessMeta meta[VULKAN_NUM_BINDINGS]` + `UNSIZED_ARRAY = 0x7f`；`VULKAN_NUM_SETS_PER_POOL = 64`、`VULKAN_DESCRIPTOR_RING_SIZE = 16`）、`vulkan/descriptor_set.cpp:39`（`bindless = layout.meta[0].array_size == UNSIZED_ARRAY`，同一个 allocator 自动切 bindless）、`:388-395`（`descriptorPoolOverallocation` 可用则加 `ALLOW_OVERALLOCATION_POOLS/SETS_BIT_NV` 复用 pool）、`:215-230`（`vkGetDescriptorSetLayoutSizeEXT` / `vkGetDescriptorSetLayoutBindingOffsetEXT` 记 desc buffer 偏移）。
- **落地**：保持现状；等出现压力时按"池化 set → push descriptor → descriptor buffer"逐档上。
- **验收**：暂不排期；触发条件写进 [descriptors](../10-architecture/descriptors.md)，出现时改那篇。
- **预估**：3 天，风险中。

### TM-12 把反射结果变成构建产物

- **现状**：shader 由 `glslangValidator` 预编译加 `spirv-val` 静态校验（`CMakeLists.txt`），运行期不做反射；binding 号靠手写常量与约定对齐，改一个 binding 漏改 C++ 常量时要等验证层报错。
- **参考**：`vulkan/shader.cpp:781-817`（反射结果的缓存与复用）。
- **落地**：在 **构建期** 生成反射结果（`spirv-cross --reflect` 或 `spirv-reflect`），输出成 `shaders/<name>.reflect.h`，运行期只读。这不是运行期反射，而是把反射结果变成可 diff 的产物。
- **验收**：故意把某个 binding 号改错，编译期就报错（而不是运行期验证层报"descriptor 类型不匹配"）。
- **预估**：2 天，风险低。
## 6. T3：功能缺口

### TM-13 Clustered lighting

- **现状**：`src/Render/LightingSystem.cpp` 只有 2.5 KB（`LightingSystem.h` 0.8 KB），没有 cluster 结构（grep `cluster` 全仓库只命中 `VegetationGenerator` 一次）。
- **参考**：`renderer/lights/clusterer.cpp`（60.5 KB）：`LightClusterer : RenderPassCreator`、`set_resolution(x, y, z)`、bindless 的 `get_cluster_parameters_bindless` / `global_transforms` / `transform_buffer` / `bitmask_buffer` / `range_buffer`、`MaxTasks = 4` 的并行 CPU cluster 构建、`build_cluster_bindless_gpu`、阴影（ShadowType）、体积雾与贴花复用同一份 cluster、`owning_queue_type()` 可指定 compute 队列。
- **落地**：① 先 CPU 建 cluster（16x9x24 起步）写一张 bitmask；② forward 与前向透明通道按片元所在 cluster 取灯表；③ 最后再把 cluster 构建挪到 compute。
- **验收**：单场景 512 个点光下 forward ≥ 60 FPS（Debug 与 Release 各一次）；8 灯场景与旧路径截图逐像素一致；`--validation-features sync` 0 errors。
- **预估**：8~10 天，风险高。
- **关联**：F6 渲染特性；也是 `LightingSystem` 唯一现实的长大路径。

### TM-14 单趟降采样（SPD）

- **现状**：Bloom 是 bright + 3 级 down + 3 级 up（`src/Render/BloomPass.cpp`），每级一次全屏 pass。
- **参考**：`renderer/post/spd.cpp`（9.4 KB，single-pass downsampler）。
- **落地**：down 链换成单次 dispatch 的多级写法（compute + 每级 subgroup 归约），up 链暂不动。
- **验收**：Bloom 金字塔输出与旧路径 `max diff <= 1/255`；down 链 GPU 时间（`GpuTimer` 的每 pass timestamp）下降。
- **预估**：2~3 天，风险中。
- **关联**：F6 的 Compute Bloom 项。

### TM-15 场景排序与批处理三段式

- **现状**：F1 已完成 GPU 驱动提交（draw call 104 → 2、合并缓冲、Hi-Z，见 [gpu-driven-submission](../10-architecture/gpu-driven-submission.md)），CPU 侧排序批处理已不是瓶颈；剩余价值只在透明与前向通道（Water / Particle / Glass 各自已在内部排序）。
- **参考**：`renderer/renderer.cpp`（`AbstractRenderable::get_render_info` → `RenderQueue` → flush 的排序 / 批处理 / 提交三段分工）。
- **落地**：暂不排期。触发条件：前向 Pass 超过 5 个，或单帧透明物体超过 2000。
- **预估**：2 天，风险低。

### TM-16 资产路径协议与热重载

- **现状**：资源路径靠 `configure_file` 生成 `AssetPaths.h` 注入**绝对路径**（`CMakeLists.txt`），换目录要重新 configure；没有热重载，改 shader 必须重跑。
- **参考**：`filesystem/` 的 protocol 路径（`builtin://`、`assets://`、`cache://`、`file://`）加变更通知驱动的自动热重载纹理与 shader；`asset_manager`（11 KB）、`volatile_source.hpp`。
- **落地**：① 先做 `asset://` 解析（相对仓库根或 `Resources/`），替换 `AssetPaths.h` 的绝对路径；② 再做 shader 热重载（Windows 侧 `ReadDirectoryChangesW`）。
- **验收**：仓库整体换目录后不重新 configure 也能跑 `--selftest 30`；改 `shaders/*.frag` 后 1 秒内画面更新。
- **预估**：3~5 天，风险中。
- **关联**：F4 资产管线。

### TM-17 latched 事件

- **现状**：没有事件层，初始化顺序靠 `Application` 构造函数里的手工次序（[weak-points](../10-architecture/weak-points.md) 第 6 条）。
- **参考**：`event/`（全局事件管理器，分 latched 与 immediate 两类；新注册者会立刻收到已发生的 latched 事件；`EVENT_MANAGER_REGISTER_LATCH` / `EVENT_MANAGER_REGISTER`）。
- **落地**：只引入 latched 一类（device created / swapchain created / config loaded），Pass 与编辑器面板按需订阅。
- **验收**：删掉至少一处"谁先初始化"的手工调用点；`--resize-test` 通过；验证层 0 errors。
- **预估**：2 天，风险低。

### TM-18 transient attachment

- **现状**：`src/RHI/` 下 `LAZILY_ALLOCATED`、`TransientAttachment`、`ImageDomain` 均 0 命中；所有中间目标都是普通 device-local image。
- **参考**：`vulkan/render_pass.hpp:240 TransientAttachmentAllocator`、`:248 request_attachment(width, height, format, index, samples, layers)`；`vulkan/image.hpp:252-255` 的 `enum class ImageDomain { ..., Transient }`、`:358 transient_render_target()`；`RenderPassOpFlags` 里的 `ENABLE_TRANSIENT_STORE/LOAD_BIT`。
- **落地**：与 TM-08 同一底层（生命周期区间），**先做别名**；transient 只在移动端有真实收益（桌面没有 tiler）。
- **验收**：桌面端只做能力探测（`VK_FORMAT_FEATURE_LAZILY_ALLOCATED`），不改分配策略；真实启用等移动端目标出现。
- **预估**：探测 0.5 天。

### TM-19 格式与能力表

- **现状**：`Device::findSupportedFormat` / `findDepthFormat` / `findMemoryType` 按需查询，没有集中的格式能力表；G-Buffer 的 5 个格式定义在 `GBufferFormat`（`src/RHI/GBuffer.h`）。
- **参考**：`vulkan/format.hpp`（格式特性与兼容表）、`vulkan/limits.hpp`、`vulkan/type_to_string.hpp`（枚举转字符串，省掉到处手写 switch）。
- **落地**：把 G-Buffer 5 个格式加深度格式做成一张表，附 `VkFormatProperties` 与可打印名字；加 `--print-formats` 输出。
- **验收**：改 G-Buffer 格式时只改一处；`--print-formats` 能看出每个格式是否支持 `STORAGE` / `BLIT_DST`。
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

1. 开工前在 [backlog](../70-status/backlog.md) 队列加一行并把状态写成"进行中"；做完改"已完成"，同时把本条从这里移到 §3。
2. 落地后在 [decisions](../10-architecture/decisions.md) 落一条决策（取舍、触发条件、反面结论），本文件只留摘要加链接。
3. 新增条目必须四件齐全：现状证据（路径:行或数字）、参考实现（路径:行）、可量化的验收判据、预估与风险。缺验收判据的条目不进本文件。
4. 前提被推翻的条目要删（例如本项目结构变了、或外部实现已改），不要留着凑数。