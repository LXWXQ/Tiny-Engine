---
id: status/milestones
title: 里程碑与实测基线
type: status
scope: 空
read-when: 对照历史进度、或需要"已知良好基线数字"
size: ~692 行
updated: 2026-11-27
---

# 里程碑与实测基线
## 1. 已完成阶段
| 阶段 | 内容 | 关键成果 |
|---|---|---|
| baseline | 修改前的原始状态 | — |
| A | Vulkan 正确性修复 | 动态状态、视图空间 SSAO、法线接缝 |
| B-prep | 材质槽位可回收 + Model 子网格 | `kInvalidMaterialSlot` 哨兵 |
| B | 接入 tinygltf，加载 Sponza | 几何烘焙到世界空间 + 逐子网格材质 |
| C | IBL 预过滤（split-sum） | 过曝像素 95.9% -> 7.1% |
| D-1(a) | 引入 EnTT/spdlog、升 Vulkan 1.3、落地线性渲染图 | dynamicRendering + sync2 |
| D-1(b) | 全部 Pass 迁移到动态渲染 + 渲染图 | 删除 `PostProcessPass` |
| D-2 | 线性 HDR + 独立 PresentPass + BloomPass | 9 个新 Pass 没写一行屏障 |
| D-2 补充 | Pass 开关纳入命令行自检 | 四种组合回归 |
| D-2 修复 | 修掉"关闭 SSAO 让环境光乘未定义内容" | 开关下沉到着色器 |
| W1 | 运行时配置持久化（`EngineConfig`） | 顺带修掉析构顺序导致的 VMA 崩溃 |
| W2 | 渲染图语义校验（`validateWriteCoverage`） | 把"没人写"变成启动即报错 |
| D-3 | Bindless 材质描述符 | `desc binds` 104 -> 1，FPS 452 -> 498 |
| D-4 | 多帧并行 + timeline semaphore | 2 帧在飞 |
| D-5 | Compute + 异步计算队列 | `ComputeProbePass` GPU 写 + CPU 读回 |
| D-6 | Job System | worker pool + `parallelFor` |
| D-7 | EnTT ECS + Transform 层级 + 空间索引 | 视锥剔除接入渲染 |
| TD-01~10 | 技术债清理 | 见 [tech-debt](tech-debt.md) |
| P0~P3 | 调试/剖析基础设施 | 验证层结构化、GPU 计时、截图、深度校验、基线 diff、证据包 |
## 2. 当前实测基线
环境：Windows / RTX 2070 / Vulkan 1.4 驱动 / Debug 构建 / Sponza 场景。
**只适合同机相对比较，不可跨机对比。**

| 项 | 值 | 证据来源 |
|---|---|---|
| 回归 | **96 项全 PASS**（`-EnableProfiling` 时 97 项），stderr 全 0 字节 | `tools/Validate.ps1` |
| 验证层 | `errors=0 warnings=0` | 每次运行退出打印 |
| syncval | 0 errors / 0 warnings | `--validation-features sync` |
| 场景 | 103 子网格 + 天空盒；CPU 路径 `draw calls 104`，F1-b 合并模式 `draw calls 2`（1 次 multidraw + 球体仍走旧路径）（相机朝向 Sponza 时；无头自检的默认视角由 `config/engine.json` 决定） | `[Perf]` 行 |
| 三角形 / 顶点 | 270459 / 811377（`vertices` 实际统计的是索引数） | 同上 |
| 描述符绑定 | `desc binds 2`（Set0/1/2 一次 + Set3 实例 SSBO 一次；后者由所有图形管线静态使用，GPU 批关闭时也要绑） | `[SelfTest] telemetry` / `[Perf]` 行 |
| 帧率 | `--selftest 300` 约 240~260 FPS（Debug，随相机/场景内容变化） | 同上 |
| 材质槽位 | `26/256` 占用 | 启动日志 |
| GPU 时间分布 | Geometry ~1.0ms、Present ~1.1ms、SSAO ~0.58ms、其余 < 0.3ms | `--gpu-timing` |
| 流送世界 | 半径 2 -> `chunks=25`、`maxUpdateMs≈0.7`；`--streaming-fly 60` 飞 150 帧 -> `loaded=54 unloaded=19`；Release 半径 24（2401 块）= 208 FPS、`Scene::update()` 约 62 µs/帧（Debug 17 FPS） | `--streaming-world` 自检输出 |
| 地形合批 | arena 半径 12 约 46+22 MB、半径 24 约 171+82 MB；`--streaming-world 2` 的 draw calls 13 -> 2 | F2-b/F2-d/F2-g 记录 |
| GPU 批 | 实例数 ≥ 256 才走 GPU 提交（103 实例默认 CPU 路径）；稳定帧 439µs -> 9µs；当前场景 `hizCulled=0` | F1-a~e / P2 记录 |
## 3. 关键性能对照（同机 Debug）
| 对照 | 数字 | 结论 |
|---|---|---|
| Bloom 开 vs 关 | 455 FPS vs 531 FPS（+17%） | TD-06 的关闭路径真的省 GPU |
| compute probe 默认关 vs 开 | 486 FPS vs 455 FPS（-6.5%） | 探针是固定成本，必须默认关 |
| bindless 前 vs 后 | 452 FPS vs 498 FPS（+10%），绑定 104 -> 1 | D-3 有效 |
| IBL 老实现 vs split-sum | 过曝像素 95.9% -> 7.1% | C 阶段质变 |
## 4. 更新基线的规矩
改完影响性能/画面的代码后：

1. 跑 `tools/Baseline.ps1 -Action Diff` 确认无回归；
2. **成对更新**本文件 §2 与根 [`README.md`](../../README.md) §6；
3. 记录测法（帧数、是否剔除预热、构建配置），否则数字无法复现。

## 5. 近期里程碑（2026-11-23 起）

> **更早的阶段记录已归档**：D-25～D-47（地形/流送/GPU 驱动提交一轮轮迭代）、F1～F6、B1、V-01、
> 2026-11-04～11-13 的专题修复，全部移到
> [`milestones-archive.md`](milestones-archive.md)。要查「某个数字当初怎么来的、哪一轮引入的」，去归档页；
> 本页只保留**当前基线**与**仍在影响决策的近期工作**。

## T-10 / T-02：帧级计数器与建簇回到异步 compute（2026-11-23）

按 [UE 借鉴改造总纲](../80-roadmap/ue-borrowing-plan.md) §7 落地前两单。

**T-10 帧级计数器**：新增 `src/RHI/FrameStats.{h,cpp}`（`namespace tiny::framestats`，含 `selfTest`），
在 `RenderGraph::beginFrame/endFrame`、`emitBarrierIfNeeded`、`emitBufferBarrierIfNeeded`、
`Pipeline::bind` / `ComputePipeline::bind` 上挂钩，`--stats-frame` 退出时打三行。
- 基线场景（60 帧，`--clustered-lighting --clustered-gpu-build --validation-features sync`）：
  `passes/frame=21.0`、`barriers/frame=45.2 calls 45.2 entries | skipped: sameState=18.8 crossQueue=0.0`、
  `pipelineBinds/frame=22.5 redundant=10.5 (46.7%)`、87.59 FPS、`errors=0 warnings=0`、stderr 0 字节。
- 512 灯压力场景（追加 `--voxel-world 1 --light-stress 512`）：`passes/frame=27.0`、
  `barriers/frame=61.2`、`sameState=35.8`、`crossQueue=2.0`、`pipelineBinds/frame=39.5`、
  `redundant=19.5 (49.4%)`、38.67 FPS（开 sync 验证层，不可与不验比横比）。
- 两个数字的用途：`barriers/frame` 是 T-03（屏障批处理）要压 25% 的起点；
  `redundant=46.7%~49.4%` 是 T-09（状态缓存）能吃掉的上限。注意 `entries == calls`，
  因为图像屏障按 **mip × layer 逐子资源**各发一条命令，批处理的空间就在这里。

**T-02 建簇回到异步 compute**：`ScratchArena::createSlotBuffer` 在 `hasDedicatedComputeQueue()` 时
改 `VK_SHARING_MODE_CONCURRENT` + `{graphics,compute}`；`ClusterBuildPass` 用 `importBuffer`
把簇表登记成图内句柄（`crossQueue=true`、`desc.size` 取四段最大 `offset+size`），
dispatch 改 `addComputePass("Clusters.Build")`，后跟只声明不执行的 `Clusters.ReadSync`
表达 `FRAGMENT_SHADER` 读；手写两条 `VkBufferMemoryBarrier2` 删除。理由与边界见
[决策 43](../10-architecture/decisions.md)。
- 通路证据：压力场景 `skipped: crossQueue=2.0` —— ranges / indices 两条缓冲屏障被 timeline 信号接管后跳过；
  `--validation-features sync` 下 `errors=0 warnings=0`、stderr 0 字节。
- 收益未被吃掉：120 帧 A/B（`--voxel-world 1 --light-stress 512 --clustered-lighting --screenshot-frame 90`）
  旧代码 GPU 建簇 58.61 / CPU 45.80 FPS，新代码 56.23 / 43.63 FPS，比例仍是 GPU 快约 29%。
  **绝对 FPS 不可跨构建横比**（两次构建之间同一台机的 CPU 侧也一起降了约 5%）。

**顺带修掉的既有缺陷**：`src/RHI/ScreenshotCapture.cpp` 转入 `TRANSFER_SRC_OPTIMAL` 的
`dstAccessMask` 误写成 `TRANSFER_WRITE`，任何 `--screenshot` + `--validation-features sync` 的运行
（与聚簇无关，默认场景同样命中）都会报一条 `SYNC-HAZARD-READ-AFTER-WRITE`。改为 `TRANSFER_READ` 后
150 帧 + 截图 + sync 得 `errors=0 warnings=0`。细节见 [pitfalls 第 39 条](../60-debug-profiling/pitfalls.md)。

**图像一致性说明**：这个场景逐次运行本身就不同（动画相位随帧耗时变化），同二进制两次运行的平均像素差
（每 2 像素采样）已达 0.0087，而 T-02 前后是 0.0090 —— 落在噪声内。`tools/Baseline.ps1` 的
`-MaxMeanDiff 2.0` 远未触及。也就是说：**在本场景里"截图逐像素一致"不能当回归判据**，
`--validation-features sync` 的 0 errors 才是。
## T-03：图像屏障只在 Pass 内按桶合并（2026-11-23）

`emitBarrierIfNeeded` 从"逐子资源各发一次"改成"追加进 `pendingImageBarriers`，由
`flushImageBarriers` 在 Pass 命令之前按 `(srcStage,dstStage,srcAccess,dstAccess,oldLayout,newLayout)`
分桶发射"。第一版按子资源合并，实测零收益（`calls 45.2 → 45.2`）：探针显示 21 个 Pass 的每次访问都只有
1 个 mip / 1 个 layer，桶里永远一个成员。

| 场景 | 屏障调用/帧（前 → 后） | entries | sameState | FPS（同批次 A/B） |
|---|---|---|---|---|
| default | 45.2 → 38.0（-16%） | 45.2 不变 | 18.9 | not measured |
| voxel-world | 61.2 → 47.0（-23%） | 61.2 不变 | 34 | not measured |
| lights-512 | 61.2 → 47.0（-23%） | 61.2 不变 | 36 | 89.62 → 89.71（+0.1%） |

**结论：省下的屏障调用没有换成帧时间。** 47 次 / 帧不是这个场景的瓶颈，瓶颈在 P3 的状态切换
（`redundant=19.5 / 49.4%`）与 P2 的每帧摊平。这一单的结构性价值是屏障发射独立成
`src/RHI/RenderGraphBarrier.cpp`、以及"屏障可以先攒后发"的通路存在（T-07 的 3 提交点需要它）。

`--validation-features sync` 两场景 `errors=0`、stderr 0 字节；150 帧 + `--screenshot build\bench\t03-check.png`
（1920x1080）`errors=0 warnings=0`。帧级有序屏障（跨 Pass 搬运）留到 T-07 之后，理由见
[决策 44](../10-architecture/decisions.md)。
## T-01：外部资源的状态声明与漂移校验（2026-11-23）

| 项 | 值 |
|---|---|
| 新增 API | `RenderGraph::declareExternalTexture` / `declareExternalBuffer`、`struct ExternalState{layout, stage, access}` |
| 状态字段 | `TextureEntry` / `BufferEntry` 各加 `externalDeclared`、`externalUsed`、`externalEnd`、`lastExternalEnd`、`hasLastExternalEnd` |
| 三条不变量 | 每资源每帧一次 / 声明早于第一次使用 / 本帧 `entry == 上一帧 exit` |
| 交回时机 | `endFrame` 尾部（所有 Pass 之后），走 graphics 命令流 |
| 负向验收 | 故意用 `(VERTEX_SHADER, SHADER_READ)` 当入口 → 抛「状态漂移」，打印 `[T01] external buffer drift detection verified` |
| 正向验收 | compute `vkCmdFillBuffer` 填 64 字 → 图形流 `vkCmdCopyBuffer` 拷进 host-visible 回读缓冲 → CPU 逐字比对，打印 `[T01] external buffer verified: ... 64 words` |
| 40 帧 cross-queue + sync | `errors=0 warnings=0`、stderr 0 字节、EXIT 0；`barriers/frame=43.0 calls 50.4 entries`、`crossQueue=1.0` |
| 默认场景 150 帧 + 截图 + sync | `passes/frame=21.0`、`38.0 calls / 45.1 entries`、`sameState=18.9`、`22.5 binds / 46.7% redundant` —— 与 T-03 之后逐位一致 |
| lights-512 60 帧 + sync | `passes/frame=27.0`、`47.0 calls / 61.2 entries`、`sameState=35.8`、`crossQueue=2.0`、`[Clusters] 建簇=GPU`、`errors=0` |
| lights-512 600 帧 × 3 次 | 91.34 FPS / 波动 1.61% / 10.95 ms / 27 passes / 47 barriers |

结论：图外资源从「只有句柄」变成「有入口状态、有交回状态、有漂移检查」。三个既有场景的计数
一格没动，因为声明只在有人调用时才生效；`--cross-queue-test` 里它同时充当 CONCURRENT 创建、
跨队列 timeline 依赖、host 可见回读三条链路的活体证明。

顺带修掉两个问题（都已写进 [pitfalls](../60-debug-profiling/pitfalls.md)）：第 40 条「动态渲染
期间录 `vkCmdCopyBuffer`」（`VUID-vkCmdCopyBuffer-renderpass`，20 条错误 → 0）、第 41 条「用
`frameNumber - kFramesInFlight` 反推写入者，指到不存在的第 0 帧」（回读全 0）。前者是这次改动
引入的，后者是探针的边界假设错了 —— 两条都不是同步错误，`errors` 与 stderr 都不会指向真正的位置。

校验：`tools/DocCheck.ps1` PASS；默认场景与 lights-512 均 `errors=0 warnings=0`、stderr 0 字节。

## 度量批：帧时间 / 创建计数 / 显存 / 剔除汇总（2026-11-23）

`--stats-frame` 从三行扩到七行。判定依据是：T-04 的验收（稳定态每帧 `vkCreateImage` 为 0、显存不增长）
此前根本无法测量；P3 的「最长帧 ≤ 中位帧 3 倍」指标不存在；P2 的 `visible/submitted ≤ 0.6` 只有
每 60 帧一次的行内打印、没有进基线。先补尺子再动工，与 T-03 之前补屏障条数同理。

| 读数 | 实现 | 决定哪条判据 |
|---|---|---|
| `[FrameTime] frames=N \| median=.. p95=.. max=..` | `FrameStats::recordFrameTime` + 0.25 ms 桶直方图（0~250 ms + 溢出桶）+ `percentile()` | P3 首帧 hitch |
| `[Resources] frames=N \| images/frame=.. total=.. peakFrame=.. \| buffers/frame=..` | `ResourceStats`，挂在 `RenderGraph::createOwnedTexture/createOwnedBuffer` 与 `ScratchArena::createSlotBuffer` 之后 | T-04 每帧创建数为 0 |
| `[Memory] heapN used first/last/peak MiB \| allocs ..` | `vmaGetMemoryProperties` + `vmaGetHeapBudgets`，10 Hz 采样 + 退出前补一次 | T-04 显存不增长 |
| `[SceneGpu] cull summary \| frames=.. visibleRatio=.. \| groups/frame=.. visible=.. culled=.. \| instances/frame=.. submitted/frame=.. \| hizCulled/frame=.. screenCulled/frame=..` | `SceneGpuBatch::CullTotals` + `logCullSummary()`，只在 `visible+culled > 0` 的帧算样本 | P2 `visible/submitted ≤ 0.6` |

实测（600 帧 × 3 次，Debug，RTX 2070）：

| 场景 | FPS 中位 | 中位 ms | p95 ms | 最长 ms | images/帧 | heap0 MiB | visibleRatio |
|---|---|---|---|---|---|---|---|
| default | 115.91 | 7.88 | 9.12 | 88.02 | 0.27 | 1220.7 | 0.61 |
| voxel-world | 98.73 | 9.62 | 10.88 | 89.83 | 0.27 | 1220.7 | 1.00 |
| lights-512 | 89.57 | 10.62 | 11.88 | 100.43 | 0.27 | 1220.7 | 1.00 |

顺带发现并修掉的基线缺陷：`default` 场景此前是「1 draw call / 8192 三角形」，原因是相机来自
`config/engine.json`（`(-66.06, 15.92, -76.19)`，站在 Sponza 外 100 米看向天空），103 个几何组全在视锥外。
新增 `--camera <px,py,pz,pitch,yaw>` 覆盖机位，`bench.ps1` 三个场景钉住 `Scene::init` 的 Sponza 默认机位；
钉住后 `default` 是 104 draw calls / 270459 三角形 / `visibleRatio 0.61`，115.91 FPS 与旧版 164.80 不可比。

实现上踩过的两个坑（都已修）：`ResourceStats::beginFrame` 一开始清零本帧计数，导致 `total=0`
——G-Buffer、图自持目标、暂存区都在第一帧 `beginFrame` 之前创建，清掉就再也算不进来；现在清零放在
`endFrame` 累加之后。`ResourceStats::selfTest` 里测「无分配器时空操作」后忘了还原分配器与堆采样，
而 selfTest 在启动链里跑，于是整轮显存采样全死、`[Memory]` 一行都不打。

校验：`tools/DocCheck.ps1` PASS；三场景 `errors=0 warnings=0`、stderr 0 字节；`[Memory]` 的
`first` 与 `last` 相等（1220.7 → 1220.7 MiB），稳态不增长。
## T-04：跨帧 RT 池（默认关）+ 一次尺寸变化到底建多少图（2026-11-23）

T-04 的验收原文是「稳定态每帧 `vkCreateImage` 为 0、显存不增长」。先量：默认场景 600 帧
`images/frame=0.27 total=160 peakFrame=160` —— 稳态每帧 0 张，判据的一半在开工前就成立。

`--resize-test`（60 帧、6 次尺寸变化）暴露真正的成本：`total=424`，即一次尺寸变化约
44 张图，全落在 `vkDeviceWaitIdle` 之后那一帧。

| 配置 | images 总计 | heap0 末值 | hits/misses | 命中率 |
|---|---|---|---|---|
| 池关（`--rt-pool` 缺省） | 424 | 1220.7 MiB | — | — |
| 池开（256 MiB 预算） | 367 | 1476.7 MiB | 57 / 173 | 24.8% |
| 池开（512 MiB 预算） | 360 | 1476.7 MiB | 64 / 166 | 27.8% |

结论三条：池确实能省 13~15% 的创建量；但它要常驻 250 MiB 以上的空闲图，帧时间没有可辨
收益（p95/max 本来就在 39~70 / 85~106 ms 之间漂）；命中只发生在「尺寸切回旧值」，
窗口拖动那串中间尺寸几乎全不命中 —— 所以默认关，留 `--rt-pool` 作开关。

`top creators` 指出下一步：`SSAO.Raw.Frame0/Frame1`、`SSAO.Frame0/Frame1`、
`TAA.History0/History1` 各 6 次，44 张里约一半是 `kFramesInFlight=2` 的复制。
写在本帧读在本帧的目标不必双份，去掉这一层复制省的是常驻显存，比池更划算（见决策 46）。

校验：默认场景与 `--resize-test` 均 `errors=0 warnings=0`、stderr 0 字节；池关时
`images total=424` 与改动前逐位一致（回退可信）。

## T-04b：同帧目标合成一张 + 让 GPU 侧两帧排开（2026-11-23）

T-04 的 `[RTPool] top creators` 全是 `Frame0` / `Frame1` 成对出现。顺着这条线量下去，得到的是两个结论，其中一个和计划里写的不一样。

**第一个结论：`--single-frame-targets` 单独用是错的。** 把 SSAO / HDR / SSR / Bloom / TAA 输出 / 平面反射的图合成一份、不动提交侧，`--validation-features sync` 立刻报 `errors=120` —— 两帧确实在重叠。这条错误本身是有价值的证据：D-4 的双份不是保守，是必需的。

**第二个结论：让两帧排开之后它更快。** `submitGraphics` 多等一次上一帧的 `graphicsTimelineSemaphore` 值，就足以让合并成立，而且帧率上升。

| 配置 | lights-512 FPS | default FPS | 图总数 | heap0 |
|---|---|---|---|---|
| 回退 `--no-single-frame-targets` | 87.77 | 116.93 | 160 | 1220.7 MiB |
| 只加帧间等待 `--serialize-frames` | 91.13 | — | 160 | 1220.7 MiB |
| 合成 + 等待（默认） | 92.50 | 118.90 | 145 | 964.7 MiB |

绝大多数收益来自"排开两帧"（+3.8%），合并只再加 1.5%；但合并顺手少 15 张全屏级目标、堆少 256 MiB。

官方基线重测（`tools/bench.ps1 -Runs 3 -Frames 600`，默认配置）：`default 121.57` / `voxel-world 103.66` / `lights-512 94.56` FPS，`images/帧 0.24`（145 张），`heap0 964.7 MiB`。`--resize-test` 六次尺寸变化：424 → 319 张（每次 resize 44 → 约 17.5）。

校验：`--validation-features sync` 下默认、`--resize-test`、`--cross-queue-test` 三种跑法均 `errors=0 warnings=0`、stderr 0 字节；`--no-single-frame-targets` 时 160 张、与改动前逐位一致。

## T-09：管线绑定缓存，冗余绑定 44.7% → 0（2026-11-23）

`[FrameStats]` 的 `pipelineBinds/frame=23.5 redundant=10.5 (44.7%)` 从 T-10 起就一直挂着。这条线查下去发现冗余集中在几何 Pass 的一个函数里：

`src/Render/GeometrySystem.cpp:1314`（地形合批）→ `:1355`（GPU 场景合批）→ `:1379`（体素合批）三次都调 `geometryPipeline->bind(commandBuffer)`，中间只换了描述符集与顶点缓冲 —— 这些都不影响管线绑定，所以后两次是纯浪费。

做法：新增 `src/RHI/StateCache.h/.cpp`，按 `VkCommandBuffer` 记「当前绑着哪个管线」，`Pipeline::bind` / `ComputePipeline::bind` 是唯一两个入口（全仓只有这两处 `vkCmdBindPipeline`）。

| 场景 | 绑定/帧 | 真正发出 | 省掉 | 比例 |
|---|---|---|---|---|
| default（60 帧） | 23.5 | 13.0 | 10.5 | 44.7% |
| `--resize-test` | 23.3 | 13.0 | 10.3 | 44.3% |
| `--cross-queue-test` | 25.5 | 15.0 | 10.5 | 41.2% |
| lights-512 | 39.5 | 20.0 | 19.5 | 49.4% |

**帧时间没有可辨变化。** lights-512 600 帧 × 3 次两批：95.01（spread 1.09%）与 96.30（spread 2.39%），基线 94.56 —— 差值落在波动内，不宣称涨帧率。省掉的是 10~20 条驱动调用，本来就不是瓶颈。

**过程中修掉的一个记账错误。** 第一版把 `statecache::beginFrame` 放在 `Renderer::beginFrame`，`[StateCache] bindCalls` 比 `[FrameStats] pipelineBinds` 每帧多 1.5 —— 启动期的 IBL / 纹理上传发生在第一帧之前，只进了总数不进任何一帧。改成与 `framestats::beginFrame` 同一行开窗、并在第一帧清零总数之后，两表逐位相同。这条等式现在是缓存的唯一自检手段。

校验：default / `--resize-test` / `--cross-queue-test` / lights-512 / 150 帧 + 截图 五种跑法均 `errors=0 warnings=0`、stderr 0 字节。

### 第一次回归为什么崩（已修）

第一版缓存对**所有** `VkCommandBuffer` 记账。地形构建（`src/Render/TerrainComputeGenerator.cpp`、`src/Render/TerrainGpuMeshBuilder.cpp`）、纹理上传（`src/RHI/Texture.cpp`、`src/RHI/Texture3D.cpp`）用的是自己的一次性命令缓冲：每帧 `vkResetCommandBuffer` 重录，句柄不变。缓存因此以为"管线还绑着"，整段派发一条管线都没绑，`tools/Validate.ps1` 里 35 个地形/流式用例直接 `exit code -1073741819 (0xC0000005)`（`terrain-render`、`f2-streaming-world`、`f2-streaming-fly`、`f2b-terrain-batch`、`f2b-terrain-batch-off`、`f2c-origin-rebase`、`f2c-origin-off`、`f2d-streaming-radius`）。

修法是"只对登记过的缓冲记账"：`statecache::trackCommandBuffer(cb)` 在 `vkBeginCommandBuffer` 之后调用，引擎两条主缓冲由 `beginFrame` 自动登记，上面四处一次性缓冲各补一行；没登记的缓冲一律照发，并计入 `unmanaged`。修完 `unmanaged=0`、五个场景两表逐位相同、地形与流式用例全部恢复。

### 回退与验证

`--no-pipeline-cache` 是整体回退开关（关掉后每次绑定都照发）。五个场景（default / voxel-world / lights-512 / `--resize-test` / `--cross-queue-test`）均 `errors=0 warnings=0`、stderr 0 字节；同批次交替 A/B（lights-512，600 帧 × 3）中位数 93.92 对 91.76 FPS（+2.4%）。
## T-05：Pass 剔除（默认关，实测没有可剔的对象）（2026-11-23）

`RenderGraph::cullDeadPasses()` 在图内做「输出无人读」的剔除：先定根（纯声明 Pass / 无访问声明 / 最后一个 / 写 persistent 纹理 / 写 host-visible 缓冲 / 图上没写东西 / 写导入资源），再从根反向闭包找最近的重叠写者。

| 配置 | `passes/frame` | `culled/frame` | `[Validation]` |
|---|---|---|---|
| 默认 | 21.0 | 0.0 | errors=0 warnings=0 |
| `--pass-cull` | 21.0 | 0.0 | errors=0 |
| `--pass-cull --no-bloom` | 14.0 | 0.0 | errors=0 |
| `--pass-cull --validation-features sync --screenshot --screenshot-frame 35` | 21.0 | 0.0 | errors=0 |

结论：**一条都没剔掉**。本引擎的 Pass 在特性关闭时根本不进图（`BloomPass::addToGraph` 先看 `bloomEnabled`），计划里那条「`--no-bloom` 时图内 Pass 数减 7」在开工前就成立（21 → 14），但不是剔除的功劳。因此默认关，只留 `--pass-cull` 当兜底。

实现过程中被实测撞到三次：

1. **LOAD 附件是读**。`writeColor(..., LOAD_OP_LOAD)` 的访问位含 `COLOR_ATTACHMENT_READ`，按「无写位才算读」判会把它的上游整条判死 —— 第一版把 `Glass` / `Geometry` / `Lighting` / `SSAO` 都剔了。
2. **导入资源与「图上没写东西」的 Pass 必须是根**。`Present` 写的是 `importTexture(..., VK_IMAGE_LAYOUT_UNDEFINED)` 导入的交换链图（`persistent` 为假，只能靠 `owned=false` 认出来），`HiZBuild` 在图上只声明了读、输出落在 `SceneGpuBatch` 的自持图像里。漏了这两条，Bloom 整链 + `Present` + `HiZBuild` 全被判死。
3. **开关必须在 `renderGraph` 创建之后设**。一开始写在 `renderer` 初始化旁边，四次运行全部 `0xC0000005`、stderr 0 字节、日志停在 `[Renderer] D-4/D-5 active`。

校验：`tools/DocCheck.ps1` PASS；`tools/Validate.ps1 -Incremental -SkipSetup` ALL PASS（96 项、FAIL 0、stderr 0 字节）。
## T-07：队列窗口时间戳 + 图形拆成两段提交（2026-11-24）

### 第一步：先给队列装尺子（`--stats-queue`，提交 `f9397bd`）

每条命令缓冲头尾各写一枚 timestamp，两条队列各管自己的一对 query。第一次量 512 灯场景（`--clustered-gpu-build`，sync 验证层）：

| 指标 | 值 |
|---|---|
| makespan | 8.65 ms |
| graphics busy | 8.36 ms (96.7%) |
| compute busy | 0.25 ms (2.8%) |
| overlap | 0.00 ms (0.0% of compute) |

计算队列每帧只忙 0.25 ms，而且与图形零重叠：同帧 c2g 依赖 + 每帧只提交一次图形命令，整条图形流都在等 compute。

### 第二步：图形拆成 `G0 -> C0 -> G1`（本次）

| 配置 | makespan | graphics busy | compute busy | overlap |
|---|---|---|---|---|
| `--no-async-split` | 10.06 ms | 9.73 ms (96.8%) | 0.27 ms (2.7%) | 0.00 ms (0%) |
| 默认（拆分） | 9.03 ms | 9.03 ms (100.0%) | 0.51 ms (5.7%) | 0.51 ms (100%) |

无验证层、600 帧 × 3 轮交替：开 101.79 / 100.95 / 103.25，关 100.55 / 101.43 / 100.83 → 均值 **102.00 对 100.94（+1.05%）**。

三条保守规则：拆点是第 0 个 graphics Pass 时不拆；`--no-async-split` 整体退回单次提交；没有 compute 写时提交路径与改造前逐位一致（默认场景 `passes/frame=16.0`、截图差异落在噪声内：`mean=0.0262` 对同二进制噪声地板 `mean=0.0173`）。

校验：默认场景 150 帧 + sync + 截图 `errors=0`；`--cross-queue-test` 的 `[TD02]` 两向与 `[T01]` 两条断言全绿、`overlap=0.10 ms (41.8%)`（g2c 帧不拆，符合设计）。
## cube-field：P2 的入场尺子（2026-11-24）

用户选 A（先量再动）之后的第一个改动：引擎里终于有了一万个实例。
做法不是建一万个实体——材质槽上限 256（`DescriptorManager::init(Device&, uint32_t maxMaterials = 256)`），
一万个实体各持一个槽位直接撞墙；走的是既有的外部实例注入路径（`SceneGpuBatch::ExternalInstance`），
一万个立方体共享一个材质槽、一个模板模型（`Resources/Models/debug_cube.obj`）。

- `--cube-field <n>`：`Scene::loadCubeFieldTemplate()` 加载模板，`SceneGpuBatch::setCubeField(...)` 只登记参数，
  真正铺开在第一次构建时做（`refreshCubeFieldInstances`），数量不变就不再重建。
- 阵列中心在构建时按本帧 `viewMatrix` 的逆算：启动期相机的视图矩阵还没被 `update()` 刷过，在 `Application` 里算会拿到单位矩阵
  （踩过两次：先算出 (0,0,-14) 把阵列铺在原点，再按错的方向铺到相机背后，被视锥整组剔掉）。
- 读数（`--cube-field 10000`，300 帧 × 3 次）：`106.57 FPS`、`flattenUs 18871`（**每帧 18.9 ms 花在摊平一万个实例上**）、
  `instances/frame 10063`、`visible 0.62`、`heap0 964.7 MiB`、`errors=0`。
- `tools/bench.ps1` 新增 `cube-field` 场景与 `instances` / `flattenUs` 两列；`[SceneGpu] cull summary` 也带上 `flattenUs/frame`。
- 一个必须记住的口径：`visible = 0.62` 还是**组级**的。一万个立方体合并成一个组，组级剔除只能全剔或全留，
  所以 P2 的「visible/submitted ≤ 0.6」在实例级剔除落地之前不会因为这个场景而改善。

## T-06：帧间交接改走图级黑板（2026-11-24）

`FrameInfo` 上八个"只有一个生产者、消费者三四个"的交接字段搬进 `RenderGraph` 的黑板，
`FrameInfo.h` 从 470 行降到 441 行。

| 项 | 之前 | 之后 |
|---|---|---|
| `FrameInfo` 交接字段 | 8 个（`renderAlpha` / `renderTick` / `exposureOverride` / `editorSelection` / `onSaveConfig` / `onLoadConfig` / `onBuildEditorUI` / `isEditorMode`） | 1 个句柄 `tiny::RenderGraphBlackboard* blackboard` |
| 交接点定义位置 | `FrameInfo.h`（聚合初始化，插中间会让后续字段错位） | `src/Render/FramePayloads.h`（四个载荷类型） |
| ImGui 读载荷的时机 | `prepareDrawData` 在 `beginFrame` 之前（读不到） | 挪到黑板填好之后 |
| 默认场景 `passes/frame` | 16.0（T-07 后） | 16.0（不变） |

读数（默认 40 帧 + `sync`）：`[FrameTime] median=13.12ms p95=41.62ms max=95.70ms`、
`[Resources] total=145 peakFrame=145`、`[SceneGpu] visibleRatio=0.61`、`errors=0`、`stderr 0` 字节。
`--cube-field 10000` 120 帧：`85.45 FPS`、`median=8.62ms p95=29.88ms max=132.36ms`、
`instances/frame=10063`、`visibleRatio=0.62`、`errors=0`。
`--cross-queue-test`：`[TD02]` 两向 + `[T01]` 两条断言全绿。

三处实现要点：

- 黑板键是 `std::type_index`，值是 `Entry{void*, void(*)(void*)}` —— 析构函数在创建时按 `T` 记下来。
- `beginFrame()` 第一件事是 `blackboard.clear()`，所以载荷**严格一帧**；跨帧状态留在各 Pass 自己的成员里。
- `ImGuiSystem` 不持有 `RenderGraph`，靠 `FrameInfo::blackboard` 句柄取载荷。

一次崩在 `0xC0000005` 的经历值得记：第一次一次性改完九个文件，编译过但启动即崩（`EXIT=-1073741819`、
`stderr 0` 字节、日志停在自检开始）。回退后分四步重做，根因是 `ImGuiSystem::renderConfigPanel` 里
`callbacks` 的四行声明被插到了 `if (!ImGui::CollapsingHeader(...))` 与它那句 `return;` **之间** ——
等于把 `return` 挪到了声明之后，Save/Reload 随后读到未初始化的指针。
## T-08：实例与图元的跨帧编号（只读骨架，2026-11-24）

P2 的三条验收要「visible/submitted ≤ 0.6」「摊平 CPU 时间 −70%」，但 GPU 侧的实例编号原来每帧随结构重建而漂移，任何"按实例记忆"的优化都没有立足点。这一步只补地基，不改渲染路径。

| 读数 | `--no-gpu-scene-persist` | 默认开 |
|---|---|---|
| `--cube-field 10000`：primitives / instances | 0 / 0 | 104 / 10103 |
| `--cube-field 10000`：dirtyFrames | 0.0% | 1 帧（首帧建结构） |
| `--cube-field 10000`：upload | 0.0 KB/frame，0 次 | 35.6 KB/frame（摊到 40 帧），1 次 |
| `--cube-field 10000`：resizes | 0 | 2（0.5 → 2.2 MiB） |
| 默认场景（Sponza）：primitives / instances | — | 103 / 103，`dirtyFrames=1/120`，`upload=0.2 KB/frame` |
| 渲染结果 | 一致 | 一致（`errors=0 warnings=0`，`--validation-features sync`） |

三条结论：

1. **静态场景的结构只上传一次**。Sponza 120 帧里 `dirtyFrames` 只占 1 帧、`upload` 摊薄到 0.2 KB/frame —— 持久缓冲的稳态成本接近零，这与 T-04 那次"池化只在尺寸换回旧值时命中"是同一种形态：真正的收益要看后续消费者。
2. **键必须来自内容和来源，不能来自本帧顺序**。图元键取 `(模型指针, 子网格)`，实例键再混组内序号；同一物体在下一帧拿回同一个编号，才谈得上"记住上次结果"。
3. **开关的位置比开关的值更容易出错**。`SceneGpuBatch` 在 `GeometryPass::init()` 里才被创建，把 `setEnabled` 写在 `initAll()` 之前就是空指针解引用（`0xC0000005`、stderr 0 字节、日志停在 `[Tiny] 材质就绪`）。判据：凡是往"Pass 内部成员"上落开关，先确认那个成员是哪一步构造出来的。

校验：四场景（default 120 帧 + sync、cube-field 40 帧 + sync、`--cross-queue-test`、`--no-gpu-scene-persist`）全部 `EXIT=0`、`errors=0 warnings=0`、stderr 0 字节；`--cross-queue-test` 的两条 `[TD02]` 与两条 `[T01]` 断言照旧通过。
## P2 第一步：实例 AABB 用中心/半长，子网格包围盒加载期预热（2026-11-24）

| 场景 | flatten 改前 | 改后 | 稳态帧率 |
|---|---|---|---|
| default（Sponza 103 子网格） | 33.55 ms | 9.75~10.94 ms | 不变 |
| `--cube-field 10000` | 56.52 ms | 34.31 ms | 88.75 → 88.20 FPS（噪声内） |
| `--cube-field 30000` | 116.99 ms | 72.05 ms | 不变 |

- 世界 AABB：8 个角点各一次 `mat4 * vec4` → 中心/半长（`worldHalf = |basis[i]| · half`），对仿射变换精确。
- `subMeshBoundsFor()` 首次索引扫描（默认场景 16.6 ms）从第一帧挪到加载期，启动打 `[SceneGpu] prewarm | models=2`。
- 两者都只影响**结构重建帧**，所以帧率不动；`[FrameTime] max` 从 90 ms 量级降到 76.5 ms。
- 验证：default / 10000 / 30000 三组 `--validation-features sync` 均 `errors=0 warnings=0`、EXIT=0、stderr 0 字节；A/B 用 `git stash` 反构建三次取中位。
## P2 第二步：镜像改 opt-in，摊平成本分摊到循环体（2026-11-24）

- `diag::Options::gpuScenePersist`（默认 `false`）+ `--gpu-scene-persist`；`--no-gpu-scene-persist` 保留为显式回退。
- 镜像整段包进 `if (gpuScene.isEnabled())`；`structureSamples` 挪到判断之外（否则关掉后 `flatten breakdown` 一行不打）。
- 外部循环新增 `extRecord` / `extPush` 两段计时。
- 读数：`mirror` 三档归零；30k `total 59.79 → 57.60 ms`、`external 40.9 → 43.9`（运行间 ±3 ms）；`extPush 4.9`、`extRecord 0.6`、`extAabb 0.2`，其余约 38 ms 在循环体。
- 结论：P2 的 −70% 不能靠 AABB 上 GPU，要靠「持久缓冲变权威 + 直写映射内存 + 去掉组查表」。
- 校验：三档 `--validation-features sync` 全部 `EXIT=0`、`errors=0 warnings=0`、stderr 0 字节。

## P2 第三步：把尺子换成 Release，回退一次负收益改动（2026-11-24）

- 实测 Release：30k `total 14.67 ms`（`external 9.0`、`merged 4.5`）、10k `7.83`、default `4.55`（`merged` 占 98.7%）；FPS `100.45 / 119.26 / 198.53`。
- Debug 与 Release 的逐实例成本差 4 倍（1.27 µs → 0.30 µs）。
- 试做「预分配 `instanceScratch` + 逐条写入」：`assemble 1.2 → 3.2 ms`、`total 57.60 → 58.78`（Debug 30k），**负收益**，已 `git checkout --` 回退。原因是 `vector::insert` 对 POD 走 `memmove`，比逐条赋值快，且稳态下 `buckets` 容量保留、本来就不扩容。
- 结论：P2 的大头是 `merged`（合并网格），逐实例那条线在 Release 下已不是瓶颈；后续 P2 判据一律看 Release + 稳态重建帧。
- 校验：三档 `--validation-features sync` 全部 `EXIT=0`、`errors=0 warnings=0`、stderr 0 字节（回退后复测同前）。

## P2 第四步：稳态样本——摊平几乎为零，合并网格是首帧假象（2026-11-24）

- 新增 `--rebuild-structure <n>`（默认 0 关）与 `CullTotals::mergedBuildSamples`，`flatten breakdown` 末尾多打 `mergedBuild=`。
- Release 稳态（40 帧，每 10 帧强制重建）：default `total 0.11 ms`、10k `3.80`、30k `10.08`；`merged=0.0`、`mergedBuild=0`。
- 一次性成本对照：default 首重建 4.55、10k 7.83、30k 14.67 —— 差额是合并网格构建（192496 顶点 / 786801 索引的上传）+ `buckets` 首次扩容 + 预热前的包围盒扫描。
- 结论：摊平只在结构签名变化时发生；P2 的 −70% 要按「每次重建」计，不能按帧数平均。「合并网格持久化」不需要做（已按签名复用），真要再压只能做按需增量重建。
- 校验：`--rebuild-structure` 三档 `EXIT=0`、`errors=0 warnings=0`（Debug/release 各自跑过）。

## P2 第五步：外部实例记录缓存（2026-11-24）

- `SceneGpuBatch::ExternalRecordCache` 按「模型 + 子网格 + 材质槽 + 变换 memcmp」复用逐实例记录，`flatten breakdown` 多打 `extReused=`。
- Release 30k 一次重建 10.08 → **8.91 ms**（`external` 9.4 → 7.0）；Debug 30k 57.60 → **35.32**（`external` 40.9 → 27.6）、Debug 10k 26.46 → 14.11。
- 第一版用逐浮点 FNV 哈希做键，30k 下哈希本身比 `memcmp` 贵一个数量级，换成 64 字节 `memcmp` 才拿到收益。
- 剩余成本：`extPush` 3.2 ms（Release 30k）。要再压只能让持久缓冲直接当剔除输入，不再每帧重建 `buckets`。

## P2 第六步：剔除统计缓冲扩到 4 个 uvec4（布局验证，2026-11-25）

- `shaders/scene_cull.comp` 的 `uvec4 stats[3]` → `stats[4]`（第 4 组是实例级剔除的保留槽）；`SceneGpuBatch::Stats` 13 → 16 个 `uint32_t`（`reserved[3]`），stats 缓冲改按 `sizeof(Stats)` 分配，尺寸只有一个来源。
- 布局锁进编译期：`static_assert(sizeof(Stats) == 16u * sizeof(uint32_t))`，外加 include 生成的 `scene_cull.comp.reflect.h` 后断言 `kStatsBufferBlockSize == sizeof(Stats)`、`kStatsBufferStatsCount == 4`（反射头实测 `kStatsBufferBlockSize = 64`）。
- 验收：三档 120 帧前后 `cull stats` / `cull summary` 的整数读数逐位相同（default `103/63/40`、10k `instances=10063`、30k `30063`；`instFrustum=6756/10000`、`11157/30000`；`visibleRatio 0.61/0.62/0.62`），只有计时列不同；`--validation-features sync` `errors=0 warnings=0`、stderr 0 字节。
- 全套回归 **96 项** ALL PASS（含 `f1d-sync-validation`、`f3v-water-off`），stderr 全 0，验证日志 `build/validation/20261125-103927`；DocCheck PASS（documents=81 lines=18000）。
- 边界：`Stats` 字段顺序与 GPU 字序不同（`groups`/`drawCalls` 占 words[6..7]，与 GPU 的 `occlusionTests`/`occlusionCulled` 重叠），`readStats` 只能按名字逐字读，不能整块 memcpy。

## P2 第七步：实例级视锥统计上 GPU（2026-11-25）

- `shaders/scene_cull.comp` 加 binding 5（`readonly buffer InstanceBuffer{InstanceRecord[]}`，与 C++ 侧 224 字节的 `InstanceRecord` 同构），push constant 16 → 32 字节（`uvec4 more`：`x = count`、`y = mode`）；同一命令缓冲里两遍 dispatch：`mode = 0` 跑原组剔除，`mode = 1` 跑 `countInstanceVisibility()`（一个 invocation 一个实例，`(count + 63) / 64` 个工作组）。
- 计数写 `stats[3]`：`x` = 测过实例数、`y` = 可见实例数；`Stats` 的 `reserved[3]` 换成 `instanceTests` / `instanceVisible` / `reserved`（仍 16 个 uint，`readStats` 读 words[12]/[13]），`CullTotals` 加 `gpuInstanceTested` / `gpuInstanceVisible`，日志多一行 `[SceneGpu] instance summary | frames=118 gpuInst=0.67 (804288/1192154) | tested/frame=10103.0`。
- 口径：测**整条**实例缓冲，不是只测可见组 —— CPU 的 `instFrustum` 度量本来就是全量，同口径才谈得上对照；组门控要的「实例 → 组」表留到第三步压缩索引时一起建。两遍之间不加屏障（写集合不重叠，第二遍之后原有的全屏障已覆盖）。
- 验收（120 帧固定机位）：10k `gpuInst=0.6747` 对 CPU `0.6756`（差 0.09 个百分点）、30k `0.3726` 对 `0.3719`（差 0.07），default `0.58 (7080/12154)`；三档 `errors=0 warnings=0`、stderr 0 字节，`--validation-features sync`（10k）同。
- 代价：cube-field 30k 600 帧 × 3 轮交替 A/B，开启 `4.696/4.715/4.725 s` 对关掉这条 dispatch 的 `4.687/4.694/4.707 s`，median 帧时间同为 7.62 ms —— ≤0.5%，落在噪声内。
- 全套回归 **96 项** ALL PASS（stderr 全 0），验证日志 `build/validation/20261125-111243`；DocCheck PASS（documents=81 lines=18077）。
- 踩坑：GLSL 的 `half` 是保留字（局部变量改名 `worldHalf`）；`Copy-Item` 覆盖源码带回旧 mtime，MSBuild 跳过重编、读数全 0（实际在跑旧二进制）。
- 本步不裁剪：indirect 的 `instanceCount` 与 `visibleRatio` 仍是组级，第三步才换成 remap 后的数量。

## P2 第八步：实例级剔除落地（紧凑索引 + remap）（2026-11-25）

- `shaders/scene_cull.comp` 加 binding 6（`uvec4 cullGroups[]`：每组可见实例数 + remap 起点）与 binding 7（`uint scratch[]`：前一半 remap、后一半名次），binding 1（命令缓冲）去掉 `writeonly`；push constant 的 `more` 扩成「count / mode / 实例容量」三用，阶段 `0` 组剔除、`1` 测视锥 + 领名次、`2` 组前缀和 + 改命令、`3` 搬 remap、`4` 恒等 remap。
- `shaders/mrt.vert` 加 set 3 binding 1（`InstanceRemapBuffer`）：`instanceIndex = push.gpuScene != 0 ? instanceRemap[gl_InstanceIndex] : uint(gl_InstanceIndex)`，取矩阵与 `fragInstanceIndex` 都用它；地形批处理不过 remap。
- `src/Render/SceneGpuBatch.h/.cpp`：`FrameResources` 加 `cullGroups` / `instanceRemap`，compute 描述符从 6 槽扩到 8 槽、池 7×帧槽，图形 set 3 从 1 槽扩到 2 槽、池 2×帧槽；`prepare()` 每帧清零 `cullGroups`，按 `instanceCullEnabled && instanceCount > 0` 决定跑四段还是只跑恒等段，段间挂 compute → compute 屏障。
- 实例 → 组用 shader 内二分：CPU 按 `stable_sort(order)` 顺序拼实例缓冲，组区间按 `firstInstance` 升序连续；组级被剔除的组由 `commands[base + 1] == 0` 跳过，不复活。
- 语义变化：`cull summary` 的 `instances/frame` 与 `stats[0].w` 从「可见组的实例数之和」变成「紧凑后真正提交的实例数」，`instance summary` 多打 `submit/frame` 与 `cull=on/off`；`gpuInst` 口径不变。
- 回退开关 `--no-instance-cull`（`src/Core/Diagnostics.h` → `main.cpp` → `src/Core/CliCatalog.cpp` → `src/Core/Application.cpp` 调 `setInstanceCullEnabled(false)`）：GPU 写恒等 remap，命令不再改写 —— 顶点着色器走的还是同一段取下标代码。
- 验收（120 帧固定机位）：10k 提交 `6815.0/帧`（第二步 10063.0）、30k `11216.0/帧`（30063.0）、default `59.0`（63.0），`gpuInst` 与第二步逐位相同；开关两版截图 832 个差异像素全部落在 HUD「Performance」面板内，面板外 0 差异。
- 全量回归 **96 项 ALL PASS**（`build/validation/20261125-121228`，stderr 全 0），DocCheck PASS。
- 验证（必须用 **Debug**，Release 的 `enableValidationLayers` 在 `NDEBUG` 下是 `false`，`errors=0` 是空话）：三档 + `--no-instance-cull` + `--voxel-world 1/12` + `--validation-features sync`（10k）+ `--cube-field-motion 500 --rebuild-structure 7` 全部 `errors=0 warnings=0`、stderr 0 字节。
- 收益：30k 600 帧 × 3 轮 A/B，开 `142.19/142.86/142.08 FPS`（median 6.88 ms、p95 7.62）、关 `125.86/126.09/125.66 FPS`（median 7.88 ms）—— +13.1% FPS、median −1.00 ms；default 场景开 202.55/203.82 对关 204.81/204.65（−0.6~1.1%，实例太少时四段 dispatch 是纯开销）。
- 边界：**两处**用这套实例布局的池都要 ×2 并写满两个 binding —— 地形 chunk 实例表（`ensureTerrainChunkInstances`）与体素 chunk 实例表（`ensureVoxelBatchResources`），binding 1 都指向同一块实例缓冲（这两条路径不过 remap，内容不被读）；漏掉体素那处会让 `f3-voxel*` / `f3v-taa` / `f3v-water` / `f3v-ssr` 共 9 项回归因 `VUID-vkCmdDrawIndexedIndirect-None-08114` 变红。
- 不做并行前缀和、不改 `shadow.vert`、不改 `Stats` 布局。

## P2 第九步：逐实例 Hi-Z 遮挡的收益测量（否决，2026-11-25）

- 目的：P2 验收还差 `visible/submitted ≤ 0.6`（10k 现为 0.68），组级 `hizCulled` 恒为 0，先量逐实例 Hi-Z 的潜在收益。
- 做法：模式 1 里对通过视锥的实例算世界 AABB 并调 `occludedByHiZ`，命中写 `stats[3].z`；**只统计不裁**，画面与提交量不变；`instance summary` 增 `occInst=`。
- 读数：参考机位 `--cube-field 10000` 120 帧执行 790656 次、判定被遮挡 **0**；平视三个机位（eye y=1.2、pitch 0/−0.05/−0.15）分别 376264 / 391384 / 409416 次，仍全 0；`--voxel-world 1` 5768 次全 0。探针（`occludedByHiZ` 临时 `return true`）能让 `occInst` 跳到 0.66，证明链路没断。
- 原因：① 金字塔来自上一帧、被测试实例自己的深度就在里面（自遮挡），静止场景里比较必然为假；② 背景写 `kFarDepth = 1e9`，按屏幕尺寸选的 mip 一旦擦到天空就必然失败；③ 俯视平地上的 100×100 阵列在屏幕上不重叠，`0.68` 是视锥覆盖的几何事实。
- 结论：收益为 0，整段代码回退（工作树与 `f5ad4f3` 一致）。A/B 显示探针成本也在噪声内（cf30k 600 帧 144.3~146.6 vs 回退后 147.0/146.5/146.8 FPS），所以否决纯粹因为没收益。
- 不做：不含实例的第二份静态遮挡金字塔（要额外深度 pass，当前场景无收益）；逐实例屏幕尺寸剔除（真会少画东西，留给 P6 LOD）。

## P2 第十步：灯数据搬出 UBO 进灯缓冲，上限 100 → 1024（P2-6，2026-11-25）

- `src/Core/FrameInfo.h`：删 `pointLights[MAX_POINT_LIGHTS]` 与 `struct PointLight`，新增 `constexpr uint32_t kMaxPointLights = 1024;`；`previousProjectionView` 显式 `alignas(16)`（以前靠 `pointLights` 的 alignas 顶到 144，数组搬走后必须自己写）；`sizeof(GlobalUbo)` 3680 → 480。
- 灯缓冲在 **set 0 binding 19**（`layout(set = 0, binding = 19) readonly buffer LightBuffer { PointLight lights[]; }`），`lighting.frag` 逐灯循环改读 `lights[i]`；`present.frag` / `present_no_bloom.frag` / `taa.frag` 删掉各自的 `PointLight pointLights[100]` 声明（不读，但占 std140 的 3200 字节）。
- `src/Core/DescriptorManager.h/.cpp`：每帧槽一块 32 KiB 常驻映射灯缓冲（`CPU_TO_GPU` + `MAPPED`），`uploadLights()` 做 `memcpy` + 整块 flush、描述符每帧槽只写一次；全局 set 布局 19 → 20 条 binding、池 SSBO 占位循环扩到 `i <= 19`；新增 `applyLightBinding(set)` 并让 `writeLightBinding()` 供两个调用点复用。
- `src/Core/Application.cpp`：「逐盏写 UBO」换成一次 `uploadLights()` + `[Lights] lightBuffer | applied=… requested=… capacity=1024`（数量变化才打）；`src/Render/ReflectionPass.cpp` 自建 set 0 末尾补 `applyLightBinding()`（否则每帧一条 `VUID-vkCmdDrawIndexedIndirect-None-08114`）。
- 容量解除读数：`--light-stress 100/512/1024/1500` → `applied=100/512/1024/1024`（1500 夹取到容量），默认场景 `0`。
- 画面证据（截图 1920×1080 第 30 帧、固定机位、半径 12）：0 vs 100 盏 118,653 像素不同（≥8 的 311）、**100 vs 512 盏 208,703（≥8 的 2,446）**；逐灯 512 vs 聚簇 512 只差 9,785（0.47%），而「只上 100 盏」对聚簇 512 差 182,807（9.5%）。
- 验证（**必须 Debug**）：`--light-stress 512`、`512 --clustered-lighting`、`512 --clustered-lighting --clustered-gpu-build`、`1024`、默认场景、`--voxel-world 1 --light-stress 200` 六组全部 `errors=0 warnings=0`、stderr 0 字节。
- 全量回归 **96 项 ALL PASS**（`build/validation/20261125-132707`，stderr 全 0）；DocCheck PASS（documents=81 lines=18290）。
- 不做：灯脏区增量上传、16 bit `lightId`、簇表布局改动、`--light-stress` 默认半径调整。

## P2 第十一步：退役组级剔除（P2-5，2026-11-25）

- `src/Render/SceneGpuBatch.h`：`GroupRecord` 64 → 32 字节，只剩 `uvec4 draw` 与 `uvec4 counts`；组 AABB 留在 CPU 侧给 `gpuScene.ensurePrimitive`，不再上传。
- `shaders/scene_cull.comp`（395 → 325 行）：模式 0 改名 `kModeCommandSetup`，只写基线命令；删 `frustumVisible` / 组级 `tooSmallOnScreen` / `occludedByHiZ` / `kOcclusionBias` / `sampler2D hizPyramid`；新增 `instanceWorldBounds` / `instanceInFrustum(center, worldHalf)` / `tooSmallOnScreen(center, worldHalf)`；模式 1 一次判定视锥 + 屏率，模式 2 成为唯一裁决策（`stats[0].y/z` 与可见三角形/顶点都在这里累加）。
- 屏率误差剔除从组粒度降到实例粒度，公式不变；`--gpu-scene-min-px` 与 Validate 的 `f1e-screen-space` 保留。
- 读数：default `visible 63→60 / culled 40→43`、`instances 59→60`、`submit 59→60`、`visibleRatio 0.61→0.58`；10k `6815→6816`、0.62→0.59；30k `11216→11217`、0.62→0.59；三档 `gpuInst` 逐位不变（0.58 / 0.6747 / 0.3726）。
- **提交量第一次等于逐实例可见数**（60 = 7080/118、6816 = 804288/118、11217 = 1323606/118）—— 旧组级判定每帧误杀 1 个可见实例（`radius = length(extent)` 少乘了平面模长）。
- 画面：default 面板外 0 像素差异；10k 面板外 9,425 像素变化全在左下角 `(0,1000)-(151,1079)`，即被误杀的那条实例现在画出来了。
- 坑：`projectionMatrix[1][1]` 为负（`src/Scene/Camera.cpp:15`），屏率公式必须 `abs(frame.screen.z)`，否则 `--gpu-scene-min-px > 0` 一律剔光（这个 bug 从组级版本就在）。
- 验证（必须 Debug）：三档 + `--no-instance-cull` + `--gpu-scene-min-px 8/60` + `--no-hiz` + `--voxel-world 1` + `--validation-features sync` 共 9 组 `errors=0 warnings=0`、stderr 0 字节；全量回归 **96 项 ALL PASS**（`build/validation/20261125-140206`）；DocCheck PASS（documents=81 lines=18387）。
- 遗留：Hi-Z 金字塔仍在建但无消费者（`stats.occlusionTests/Culled` 恒 0），待复活或整条拆除；`--no-instance-cull` 语义改为「完全不剔除」。

## P2 第十二步：实例记录补上上一帧世界矩阵（P2-2 收尾，2026-11-26）

- `InstanceRecord` 224 → 288 字节（尾部 `glm::mat4 previousModel`，offset 224 由 `static_assert` 锁住）；`src/Render/GpuScene.h` 的 `Instance` 144 → 208（`previousTransform`）；`shaders/scene_cull.comp` 与 `shaders/mrt.vert` 的镜像结构同步加字段（本步不读，只为 stride 一致）。
- 填值：实体用 `worldTransform.previousMatrix * nodeMatrix`，外部缓存命中用 `external.transform`、未命中但同绑定用 `cache.transform`、首次出现用当前帧；`historyInstances` / `historyMoved` 每次重建统计，`logCullSummary()` 打 `[SceneGpu] instance history | instances=… moved=… (last rebuild)`。
- 读数（Release，120 帧，固定机位）：default `103/0`、10k `10103/0`、30k `30103/0`、`--cube-field-motion 500` `10103/10000`；三档 `instances/frame` 60 / 6816 / 11217 与 `gpuInst` 0.58 / 0.6747 / 0.3726 与 P2-5 逐位相同。
- 验证（必须 Debug）：default / cf10k / motion / `--no-instance-cull` / `--voxel-world 1` 五组 `errors=0 warnings=0`、stderr 0 字节；全量回归 **96 项 ALL PASS**（`build/validation/20261126-092708`）；DocCheck PASS（documents=81 lines=18673）。
- 两套构建对照：MSBuild（`build`）与 Ninja（`build-ninja`）各跑一遍三档 + motion，读数一致。
- 坑：shader 少声明 `previousModel` 不报错、只把第 1 个之后的实例读成错位数据（靠读数逐位不变才证明 stride 对齐）；`tools/Validate.ps1` 的 configure 写死 `-G "Visual Studio 17 2022"`，不能指到 `build-ninja`（报 `generator ... Does not match the generator used previously`）；运行引擎会改 `config/engine.json` 的相机位姿，提交前要 `git checkout --`。
- 边界：本步没有消费者（P5-1 的 velocity 才读它）；`--rebuild-structure n` 时「上一帧」退化成「上次重建」，velocity 会算成 n 帧位移；30k 场景实例缓冲 20.2 → 26.0 MB。

> 收尾补丁（2026-11-26）：`shaders/mrt.frag` 的 `InstanceRecord` 补 `previousModel`；`kTerrainChunkInstanceStride` / `kVoxelBatchInstanceStride` 从 224 改 288（行内布局不变，多出的 64 字节保持 0）。漏改的后果是片元侧材质与地形/体素合批除第 0 行外全部读错位 —— 验证层抓不到，只能靠像素 A/B（`--streaming-world 2` 合批地形修前/修后 59.4% 像素不同）。补丁后 Debug 四组 `errors=0`、全量回归 **96 项 ALL PASS**（`build/validation/20261126-094557`）。

## P2 第十三步：反射视图自己的剔除与紧凑命令（A2 上半，2026-11-26）

- `SceneGpuBatch` 新增视图层：`kMaxViews = 2`、`ViewResources`（命令 / 帧数据 / 统计 / 组计数 / remap + 两个描述符集），`FrameResources` 只留两视图共享的实例与组表。
- `prepare` / `recordDraw` 变 view 0 薄包装，新增 `prepareView` / `recordDrawView`；结构上传只由 view 0 做（`uploadStructure = viewIndex == 0u && …`），反射只读同一份。
- 描述符池与写入按 `kFramesInFlight * kMaxViews` 扩；graphics set 的 binding 0 共享实例缓冲，binding 1 各指自己的 remap。
- 反射的 compute 剔除放进独立的无附件 pass `PlanarReflection.Cull`：塞进 G-Buffer pass 会报 `VUID-vkCmdDispatch-renderpass` 与 `VUID-vkCmdPipelineBarrier-shaderTileImageColorReadAccess-08718`（12 帧 96 条）。
- 门槛与主视图同一判据 `useGpuPath && hasInstances`；小场景反射继续走 CPU 逐实体。
- 读水：`[SceneGpu] reflection view | groups=103 instances=503 cull=on`；主视图 `gpuInst=0.91 (26680/29174)`、`submit/frame=460`。
- 逐像素：与 HEAD（反射走 CPU）比 1920×1080 差异 347 像素，HUD 外 8 个像素、差值 1/255。
- Debug 六组 `errors=0 warnings=0`、stderr 0；全量回归 **96 项** ALL PASS（`build/validation/20261126-102435`）。
- 坑：`config/engine.json` 被写回相机，`terrain-render` 会因相机沉到水下而缺 `GPU selection selfTest PASS` marker；跑回归前先还原该文件。
- 遗留：阴影视图未接（A2 下半）、反射视图的 stats 不回读。
## P2 第十四步：阴影视图自己的剔除与紧凑命令（A2 下半，2026-11-26）

- `SceneGpuBatch::kMaxViews` 2 → 5：0 主视图、1 平面反射、2/3/4 三个级联；`prepareView` 增加 `screenSpaceTest` 形参（级联关掉屏幕误差剔除），合并网格的间接绘制抽成公开的 `drawMergedIndirect`。
- 新 `shaders/shadow_gpu.vert`：从 set 3 的实例表 + remap 取模型矩阵，push constant 只留 `mat4 lightViewProjection`（64 字节）。
- `ShadowPass` 并存两条管线（逐模型 0 个描述符集 / GPU 批 set 3 + 64 字节 push），`Application::initAll()` 之后 `setSceneGpuBatch()` 注入。
- 三个级联的 `prepareView` 在进入渲染通道之前派发；渲染里每个级联一条 indirect，随后切回逐模型管线画地形/体素/动态物体。
- 验收：Debug 四组 `errors=0 warnings=0`、stderr 0；阴影 A/B 第 40 帧差 77,254 像素、δ≥8 仅 2,681（0.13%）且全在阴影边缘（纹素级舍入）；全量回归 **96 项** ALL PASS（`build/validation/20261126-111338`）。
- 坑：一条管线不能同时服务两条路径（`VUID-VkGraphicsPipelineCreateInfo-layout-07988`）；合并网格重建要按帧槽退役旧缓冲（否则 `InvalidCommandBuffer-VkBuffer`）。
- 边界：阴影视图用上一帧的组/实例表（视图 0 的 `prepare` 更晚），每帧重建的场景滞后一帧；地形/体素/动态物体仍走逐模型路径。
## P5-1：运动矢量进 G-Buffer（2026-11-26）

- `GBufferFormat::Velocity = VK_FORMAT_R16G16_SFLOAT`，G-Buffer 第 6 张附件（带 `TRANSFER_SRC`），单位是 UV 增量（NDC 的一半）。
- `GeometrySystem` / `VegetationPass` 的管线扩到 5 个颜色附件；`mrt.vert` 输出当前/上一帧裁剪位置，`mrt.frag` 写 `location = 4`；天空盒/网格/几何天空/植被显式写 0。
- `mrt.vert` 的 `GlobalUbo` 尾部补 `numLights` / `exposure` / `previousProjectionView`：std140 下后者必须落在偏移 144。
- 上一帧模型矩阵取自 `InstanceRecord::previousModel`（决策 64 铺的数据），其余路径恒等于当前矩阵。
- 验收：Debug 三组 `errors=0 warnings=0`；补齐反射 G-Buffer 的第 5 张后，`VUID-vkCmdDrawIndexed-Indirect-colorAttachmentCount-06179` 归零。
- 遗留：本步没有消费者，逐像素与「差分 < 0.5 像素」的判据留到 P5-2。
- 全量回归 96 项 ALL PASS（`build/validation/20261126-114143`），stderr 全 0。
- 反射视图的帧级统计：`readStats(frame, 1)` 累加到独立的 `reflectionTotals`，`logCullSummary()` 多打一行 `[SceneGpu] reflection summary | …`（实测 31/103 组、430 实例/帧、gpuInst 0.85）。
## P5-2：TAA 改读速度图 + YCoCg 方差夹制（2026-11-26）

- 重投影改为优先读 `velocityTex`（`previousUv = uv - velocity + previousJitterUv`），速度为 0 的像素（天空盒/网格/几何天空/植被）回退到矩阵重投影。
- 3×3 邻域在 YCoCg 里同时算 AABB 与 `mean ± 1.25σ`，两者取交集后夹历史；邻域同色时兜 1/255 最小宽度。
- TAA 的 set 1 扩到 4 个采样器（`binding = 3` = 速度图），`pass.read(gbVelocity)` 进图。
- 验收：Debug 两组 `errors=0 warnings=0`；运动场景离 GT 更近（mean 4.831 → 4.775，>16 像素 6.77% → 6.61%）；静态场景两版互差 mean 0.572。
- 待补：`--velocity-probe`（velocity vs CPU NDC < 0.5 像素）与时域方差统计两条量化判据。
- 全量回归 96 项 ALL PASS（`build/validation/20261126-122409`）。
## P5-2 验收补实：速度探针与两个开关（2026-11-26）

- `--velocity-probe`：TAA 片元阶段按 8×8 抽一格，把「速度图 vs 上一帧矩阵」的差以 1/32 像素累积进 16 字节 SSBO，CPU 每 30 帧读回并打印 `[Velocity] probe | pixels= meanDiff= maxDiff= | sampled=1/64 >0.5px=`。
- `--taa-matrix-reprojection`：强制回到矩阵重投影，兼作 A/B 与回退开关；`Push` 的两个 pad 槽因此变成 `reprojectionMode` / `probeEnabled`（结构体仍 32 字节）。
- 静态场景 maxDiff 0.487 px、`>0.5 px` 0.00% —— 判据达标，且这 0.48 px 是 `R16G16_SFLOAT` 的量化下限（半精度 × 1920 宽）。
- 运动场景 meanDiff 1.2 px、76%~80% 采样点 > 0.5 px；但离 GT 只改善 1.2% / 2.4%，因为矩阵路径已经在用 `previousModel`。
- 「静态区域时域方差 −30%」**未达成**：第 40/44 帧整帧时域差 2.395（速度）vs 2.370（矩阵），被运动立方体主导；机理记在决策 68 的补测段。
- Debug 三组（静态 / 回退 / 运动 + 探针）`errors=0 warnings=0`、stderr 0 字节。

- 全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/20261126-125624`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19016）。

## P5-3：薄几何/去遮挡检测（邻域按世界半径过滤，2026-11-26）

- 邻域从 3×3 全收换成 5×5 十字 13 抽头；每抽头读 G-Buffer 世界位置，半径超出容差的抽头直接丢弃。
- 只用有效抽头算 `AABB ∩ (mean ± 1.0σ)`（σ 系数从 1.25 收到 1.0）；覆盖率 `validTaps/taps` 线性地把历史权重压到 0.45~1.0 倍。
- `--taa-classic-kernel` 是回退开关；`Push` 的两个 float 槽合并成 `uint taaFlags`（1 矩阵 / 2 探针 / 4 经典），结构体仍 32 字节。
- 读数：植被薄几何 mean 1.339 → 1.300（−2.9%）、`>16` 1.64% → 1.44%（−12.2%）；静态与运动立方体 mean 略差 0.8%~1.1%、`>16` 略好 1.1%~3.0%。
- Debug 四组 `errors=0 warnings=0`、stderr 0 字节。

- 全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/20261126-131220`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19076）。
## P5-5：`ITemporalUpscaler` 接口，TAA 接入（2026-11-26）

- 新增 `src/Render/TemporalUpscaler.h`：`TemporalUpscalerInputs` 声明输入（颜色/世界位置/历史/速度/深度），`ITemporalUpscaler` 定义 `upscalerName` / `isTemporal` / `getInputs` / `prepareFrame` / `getOutput` / `resize`。
- `TAAPass` 同时继承 `IRenderPass` 与 `ITemporalUpscaler`；`prepareFrame`、`resize` 签名本来就一致，加 `override` 即可，`getOutput` 返回 `resources.taaOutput[frame]`。
- `Application` 新增 `ITemporalUpscaler* temporalUpscaler`，初始化时指向 TAA 并打一行 `[Temporal] upscaler=taa temporal=1 inputs=color+worldPos+history+velocity`；每帧取抖动改走接口。
- A/B（`taa.enabled=true`，第 35 帧）：改前 HEAD 二进制 vs 新二进制，1920×1080 只有 155 个像素不同、全在 HUD 的 FPS 文字里，HUD 外 0 个像素。
- Debug 三组（默认 / `--no-taa` / 运动立方体）`errors=0 warnings=0`、stderr 0 字节。

- 全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/build/validation/20261126-133227`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19133）。

## P5-4：运动模糊改读速度图（2026-11-26）

- `PresentPass` 的 bloom / no-bloom 两个 set 1 布局各加一个速度图采样器（binding 3 / 2，最近邻），并 `pass.read(resources.gbVelocity[frame])` 进图。
- `present.frag` / `present_no_bloom.frag` 的 `CameraMotionVector` 换成 `MotionVector`：速度图非零就用它，为零的像素（天空盒/网格/几何天空/植被）回退到上一帧 VP 的相机重投影。
- 回退开关 `--motion-blur-matrix`（`Push.pad` 改名 `motionBlurMatrix`）退回 P5-4 之前的全屏相机速度。
- 读数：静态场景两条路径逐像素一致（HUD 外 0 个像素差异）；运动立方体场景 270,179 像素不同（13.029%）、δ≥8 的 2.261%，差异集中在旋转立方体轮廓与塔楼细栏杆上。
- Debug 四组（默认 / 运动 / 运动 + 回退 / `--no-bloom`）`errors=0 warnings=0`、stderr 0 字节。

- 全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/build/validation/20261126-135254`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19198）。

## P3 第零步：hitch 尺子与 P3 基线（2026-11-26）

- `src/RHI/FrameStats.cpp` 增加 60 帧 hitch 窗口：「最长 / 中位」超过 3 倍记一次违规；`report()` 多打 `[FrameTime] hitch | window=60 limit=3.00x windows= violations= worst= (max= median=)`。
- `selfTest()` 覆盖两个窗口：第 40 帧慢 4 倍 → 1 窗 1 违规、worst∈[3.9,4.1]；第二窗全等 → 窗口数 2、违规仍 1、最差不被覆盖。
- 基线（Release，`--cube-field 10000`，240 帧）：`pipelineBinds/frame=27.5 redundant=9.5 (34.5%)`、`[StateCache] saved=34.5%`、`barriers 37.1 calls / 27.0 entries`、median 6.62 ms / p95 8.38 / max 19.80、**hitch windows=4 violations=0 worst=2.54x**。
- 结论：P3 第二条验收现在就达标；第一条「状态切换 −40%」的 34.5% 已被 T-09 的 `StateCache` 吃掉，剩余冗余是跨 pass 与顶点/索引缓冲两类，排序消不掉。
- 结论：P3-4 的「首用即建」不成立 —— 管线都在 `initAll()` 建好，PSO 预热只影响启动时间。
- Debug 同场景 `hitch violations=1 worst=12.32x (max=112.18ms)`，是首帧冷启动，判据只在 Release 上用。

## P3 第二步：StateCache 覆盖顶点/索引缓冲（P3-2，2026-11-26）

- `src/RHI/StateCache.{h,cpp}`：`Slot` 增加顶点/索引缓冲的「上次绑的是谁」，新增 `bindVertexBuffer` / `bindIndexBuffer`，规则与管线一致（未登记的命令缓冲照发、`--no-pipeline-cache` 整体回退）。
- `Stats` 增加六个计数，`report()` 多打 `[StateCache] buffers | vertexBinds/frame= issued= skipped= (..%) | indexBinds/frame= ...`。
- 接入四处：`src/Scene/Model.cpp` 的 `Model::bind()`、`src/Render/GeometrySystem.cpp` 的 arena 绑定（3 处）、`src/Render/SceneGpuBatch.cpp` 的合并网格绑定（2 处）、`src/Render/ShadowPass.cpp` 的地形 arena 顶点绑定（2 处）。
- 读数（Release）：地形流式场景顶点 binds 31.5 → 12.9（省 59.0%）、索引 28.5 → 8.0（省 72.1%）；cube-field 与默认场景都是 5.0 → 3.0（省 40%）。
- 画面：默认 vs `--no-pipeline-cache` 第 60 帧 1920×1080 只差 274 像素，全在 HUD 的 FPS 文字上，面板外 0。
- 验证：Debug 五组 `errors=0 warnings=0`、stderr 0 字节；全量回归 96 项 ALL PASS（`build/validation/20261126-142524`）；DocCheck PASS（documents=82 lines=19346）。
- 未接入：`ShadowPass` 的地形 arena 索引绑定（收益小，留给需要时）。

## P3 第五步：参数块与绑定号交给构建期反射（P3-5，2026-11-26）

- 几何 pass 的 `SimplePushConstantData`（224 字节）改由 `mrt.vert.reflect.h` / `mrt.frag.reflect.h` 锁：3 条块大小 + 21 条成员偏移；管线布局与 6 处 `vkCmdPushConstants` 的大小直接用 `kPushBlockSize`。
- 光照 pass 的 set 0 改由 `lighting.frag.reflect.h` 锁：`sizeof(GlobalUbo) == 480` + 14 条成员偏移；`bindings[0]/[10]/[11]/[19]` 取 `kUboBinding` / `kMaterialFactorsBinding` / `kShadowMapsBinding` / `kBinding19Binding`，贴图 1~9 与聚簇 15~18 用「起点 + 连续性断言」驱动循环。
- 只改这两处：set 0 布局与 `GlobalUbo` 全引擎各一份，几何 push 结构体也只有一个 —— 逐 Pass 复制断言只会把「手写第二份真值」换个形状。
- 验收：编译期全绿；Debug 四组（流式 / 运动立方体 / 体素 / 300 盏灯）`errors=0 warnings=0`、stderr 0；读数与 P3 第二步逐位一致（`pipelineBinds 18.5 / redundant 35.1%`、顶点与索引各 40.0% 命中）；全量回归 96 项 ALL PASS；DocCheck PASS（documents=82 lines=19441）。
- 边界：描述符类型/数量仍手写（反射头有 `k<Name>Count`，留给下一步）；`kBinding16`~`kBinding19` 是无名绑定，只锁数字不加语义。

## P3 第六步：描述符类型与数量交给反射（P3-5 第二步，2026-11-26）

- `tools/SpirvReflect.cpp` 在每个 `k<Id>Count` 之后多生成一行 `k<Id>Kind`，把描述符种类编码成整数（`sampler 1 / combinedImageSampler 2 / image 3 / uniformBuffer 4 / storageBuffer 5 / unknown 6`，`0` 是「未映射」哨兵）。
- 新增 `src/RHI/DescriptorReflect.h`：`kKind*` 常量 + `constexpr VkDescriptorType descriptorTypeOf(uint32_t)` + `template <uint32_t Kind> kDescriptorType`。未映射映射到 `VK_DESCRIPTOR_TYPE_MAX_ENUM`，所以认不出的种类不会悄悄变成 `UNIFORM_BUFFER`。
- 三处调用点改成「类型 + 数量都取自反射」：`DescriptorManager::createGlobalLayout()`（set 0/1/2）、`SceneGpuBatch` 的计算布局（binding 0~3、5、6~7）与 set 3 实例布局（binding 0/1）。
- 断言网：set 0 的 8 条 `kDescriptorType<…Kind> == VK_DESCRIPTOR_TYPE_*` + 5 条数量；计算侧 7 条类型 + 编号断言（0/1/2/3/5/6/7，4 号留给已退役的 Hi-Z）；set 3 的 2 条类型 + 2 条数量 + `kBinding0Binding == 0 && kBinding1Binding == 1`。
- 验证：Release 2.1 s / Debug 2.6 s 构建通过；Debug 5 组（流式、运动立方体、体素、300 盏灯、300 盏灯 + 聚簇）全部 `errors=0 warnings=0`、stderr 0 字节；Release 读数与 P3 第二步逐位一致（管线 18.5/35.1%、顶点与索引各 40.0%）；全量回归 96 项 ALL PASS（`build/validation/20261126-151226`）。
- 留下的唯一手写量：set 1 的 4 个 bindless 材质数组按运行期 `capacity` 申请（反射侧 `Count = 0`，因为 shader 声明的是运行期数组）。

## P4 第零步：给阴影 Pass 立一把「每级联 GPU 时间」的尺子（P4，2026-11-26）

- P4 的验收有两条是时间比值（静态「阴影 Pass 的 GPU 时间下降 ≥ 60%」、动态「≤ CSM 的 1.5 倍」），但引擎现成的 `GpuTimer` 只按 pass 打一对时间戳，一整条 Shadow pass 只给一个总数 —— 三级联里某一级塌了会被平均掉。因此 `src/Render/ShadowPass.{h,cpp}` 自带最小计时：每帧槽 3 级联 × 2 个 `VK_QUERY_TYPE_TIMESTAMP`（池 `kFramesInFlight × 6 = 12`），`kTimingReportInterval = 60`。
- 落点：进 `VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT`、出 `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`；读回放在**下一轮同帧槽**的开头（先 `vkGetQueryPoolResults(… 64_BIT | WAIT_BIT)` 再 `vkCmdResetQueryPool`，顺序反了读数恒为 0）。每 60 帧一行 `[Shadow] timing | cascade0=…ms cascade1=…ms cascade2=…ms total=…ms | avg(60 frames)`；退出时 `reportTimingSummary()` 先 `vkDeviceWaitIdle` 再打 `[Shadow] timing summary | frames=N | … | min=… max=…`。
- 开关 `--stats-shadow-timing`（`src/Core/Diagnostics.h` 的 `Options::statsShadowTiming` + `main.cpp` 解析 + `src/Core/CliCatalog.cpp` 一条「剖析」），沿用 `TINY_ENABLE_GPU_TIMING` 编译期门控；不开时不建池，`timingQueryCount()` 计进 P0-6 的零开销守卫，`[Diag] zero-overhead guard` 多一列 `shadowQueries=`。
- CSM 基线（Release，`--selftest 300 --stats-shadow-timing --hidden`，三次重复逐位一致）：静态 Sponza（逐模型路径 309 draw）`0.041 / 0.029 / 0.040 → total=0.110ms`（min 0.098 / max 0.129）；`--cube-field 500`（GPU 合并间接）`0.014 / 0.018 / 0.011 → 0.043ms`；`--cube-field 500 --cube-field-motion 2000` `0.015 / 0.019 / 0.012 → 0.045ms`；标定 `--cube-field 4000` → `0.077ms`。整帧 4.29ms（233 FPS）。
- 读数带来三个结论：阴影只占整帧约 2.5%（「−60%」在 0.1ms 的基数上是 0.07ms 绝对量，验收需要更重的基准场景）；同一场景 GPU 合并路径比逐模型 309 draw 快 2.5 倍而三角形数相同（基线大头是提交/驱动开销，不是像素带宽）；动态与静态立方体同值（CSM 每帧全量重画，与「场景静态」这个标签无关）。
- 验证：Debug 六组（默认 / `--cube-field 10000 --cube-field-motion 500` / `--streaming-world 2 --gpu-scene-external 400` / `--voxel-world 1` / `--light-stress 300` / `--streaming-world 2 --streaming-fly 60`）全部 `errors=0 warnings=0`、stderr 0 字节；`--no-shadows` 降级为 `frames=0`；守卫关时 `PASS | shadowQueries=0`、开时 `SKIP | shadowQueries=12`。全量回归 96 项 ALL PASS；DocCheck PASS。

## P4-1：CSM 提到 4096 重立基线 + 稀疏页分配器（P4-1，2026-11-26）

- 第零步把验收架在 0.043~0.110 ms 的基数上（整帧 4.29 ms 的 2.5%），「−60%」只有 0.03~0.07 ms 的绝对量。用户裁决（2026-11-26）：先试网上的重型场景，拿不到就把 CSM 分辨率 2048 → 4096。实测 curl 能出网但候选全落空（Crytek Sponza 包与 Sun Temple 都 404，Khronos 新仓库里唯一的重型场景就是引擎已内置的 Sponza），Bistro / San Miguel 是 0.4~3 GB 的 OBJ 包还要手写场景 json → 走 4096（决策 77）。
- 4096 基线（Release，`--stats-shadow-timing`，每场景三次重复逐位一致）：静态 Sponza `0.058 / 0.063 / 0.083 → 0.204 ms`（min 0.190 / max 0.242）、`--cube-field 500` 0.076、`--cube-field 4000` 0.135、`--cube-field 20000` 0.381、动态 `--cube-field 20000 --cube-field-motion 2000` 0.442。像素数涨 4 倍、读数只涨 1.8 倍 —— 与第零步第二个结论一致（大头是提交/驱动，不是像素带宽）。
- P4-1 本体：新增 `src/Render/SparsePageAllocator.{h,cpp}`（101 + 386 行）。单图集、定长 128² 页、页号即物理索引、uint 环形空闲队列（FIFO 复用、O(1) 不扫描）、每页版本号（0 = 从未分发，复用时自增，页表靠它判内容失效）、脏位图 + 标记顺序列表（去重）、`validate()` 校验全部不变式（队列长度 + 活跃页数 = 总页数、队列无重复、活跃页不出现在队列里、位图与计数一致）。
- 两套自检：`selfTest()` 每次启动跑（64 页、线性分配 / FIFO 复用 / 版本号 / 双重释放 / 脏页去重 / 无泄漏六个确定性场景）；`stressTest()` 由 `--page-alloc-selftest` 触发（20 万次随机操作，释放概率 = 当前占用率让活跃页数在容量一半附近无界游走，再压一段「几乎全满 + 每 97 次释放一个」；与一份独立数组口径逐项对账，收尾先全释放再全分配，证明无泄漏、无重叠）。
- 读数：Release `ops=200000 pages=4096 alloc+free=8.3 ns/op`、`peak=4096`、`reuse=99050`、`exhausted=5996`；Debug 43.2~47.5 ns/op。4096 页全量重分配约 34 µs，所以 CPU 侧分配够用，GPU 侧 `page_alloc.comp` 推迟到 P4-4（那时才有消费者，现在写就是无法验证的死代码）。
- 验证：Debug 六组（默认 / `--page-alloc-selftest` / `--cube-field 4000` / `--streaming-world 2` / `--voxel-world 1` / `--no-shadows`）全部 `errors=0 warnings=0`、stderr 0 字节、exit=0；不带开关时启动自检静默通过。全量回归 96 项 ALL PASS（build/validation/20261126-161321，日志 201 行，FAIL=0）；DocCheck PASS。

## P4-5：阴影复用合并网格的可见实例表（P4-5，2026-11-26）

- 阴影 Pass 此前 CPU 逐实体 × 逐子网格提交：Sponza 一个实体 103 个子网格 × 3 级联 = 309 次 draw、零剔除。P2 第六步已经把静态几何合并成一块缓冲、每帧 compute 剔出紧凑间接命令，视图槽 2/3/4 一直空着 —— 这次把它们接上。
- 新判据 `SceneGpuBatch::isShadowBatchReady()`（`available && mergedActive && mergedMesh.valid && !instanceScratch.empty()`），**故意不吃** `gpuSceneMinInstances` 门槛（默认 256，Sponza 只有 103）：主视图在小场景逐子网格更省，但阴影是每级联重画一遍，门槛不成立。逐模型路径的跳过判据改成 `isEntityBatched()`，与 `buildInstances` 的 collectView 资格逐条同源。
- `ShadowPass::recordCascade` 每级联 `prepareView(视图 kShadowViewBase + cascade)` → 绑实例表 → push 64 字节 → 一条 `drawMergedIndirect`；`static_assert(kShadowViewBase + kCascadeCount <= kMaxViews)` 锁视图槽。回退开关 `--no-shadow-batch`（`FrameInfo::Settings::shadowGpuBatch`）。
- **顺手修掉一个老 bug**：逐模型阴影路径整轮只 push 一次 `world->matrix`，漏了 glTF 节点矩阵。Sponza 子网格顶点活在节点局部空间（0.008 缩放、原始坐标 449~541），于是被放大 125 倍画到离光源相机约 700 单位处、糊满整张 4096²（落盘读数 `mean=0.795`、覆盖 9,023,488 texel）。合并路径（`SceneGpuBatch.cpp:1266`）与主视图 F4（`GeometrySystem.cpp:1241-1255`）都乘过节点矩阵，只有这里漏了 → 此前记的「0.204 ms」是画错地图的开销，真实基线是 0.087 ms。
- 读数（Release 300 帧，三次一致）：静态 Sponza 0.087 → 0.063~0.066 ms（−24~28%）；`--terrain-test` 0.183 → 0.135；`--voxel-world 1` 0.083 → 0.066；`--light-stress 300` 0.087 → 0.063；`--streaming-world 2` 0.249 → 0.244（地形主导）。验收门槛②「静态 −60%」未达成 —— 基数修正后只到 −28%，要拿到 60% 得靠 P4-3/P4-4 的页标记跳过。
- 像素等价：`--terrain-test --no-water --no-ssr --no-taa` 第 40 帧 A/B 差 162 像素、Sponza 差 233 像素，bbox 全是 FPS 文字框 `(109,119)-(157,129)`。Debug 七组 `errors=0 warnings=0`、stderr 0 字节；守卫不带开关 PASS；全量回归 96 项 ALL PASS；DocCheck PASS。
- 顺带记录一个场景问题：默认相机在 `(-182.5, 6.69, 52.63)`、Sponza 世界中心在 `(-0.48, 5.21, -0.31)`，相距约 190 单位、远超 90 m 阴影距离 → 该场景的级联阴影本来就是空的（历史遗留，待收拾）。

## P4-2：虚拟阴影图先立资源形状（P4-2，2026-11-26）

- 新增 `src/Render/VirtualShadowMap.{h,cpp}`：一张 8192² 图集切 4096 张 128² 页、每帧槽一块 12 KiB 页表（3 个 clipmap × 32² entry）、每页瓦片里横排 7 级 HZB（64+32+16+8+4+2+1 = 127/128 texel）。
- 布局算术全部做成 `static` 函数 + 启动自检八项整数断言（图集坐标 / 页表索引 / HZB 连续性 / 3072 页预算 / stride 12288），把「布局错」与「P4-3/P4-4 的剔除、标记错」分开 —— 这两类错误的症状都是「阴影缺一块」。
- 显存只在 `--dump-vsm-resources` 时才建：depth D32 256.0 MiB + hzb RG16F 256.0 MiB = 512.0 MiB，页表 24.0 KiB。默认只建 CPU 侧（页分配器 + 页表镜像），因为 P4-2 之后还没有采样方，建满 512 MiB 既验证不了正确性，又会在 96 个进程的回归里反复分配。零开销守卫新增 `| vsm=<bytes>B` 一列，开关关着必须是 0 B。
- 为什么横排不进 mip 链：VSM 的 HZB 按页重算（脏页粒度是页），必须让一页的七级落在同一块连续 texel 里；mip 链既多 8/7 的显存，又没法独立寻址 128² 瓦片。
- 两个坑：①带开关跑完进程以 `0xC0000005` 收场 —— `Application::run()` 在 `:1450` 就 `vulkanDevice.reset()`，成员的析构晚于它，得在 `vulkanDevice.reset()` 之前显式 `virtualShadowMap->destroy()`；②`main.cpp(531): fatal error C1061: 编译器限制: 块嵌套太深` —— 参数解析那条 `else if` 链已顶到 MSVC 上限，启动型开关要写成链前的早退块。
- 验证：Debug 六组（默认 / `--dump-vsm-resources` / `--page-alloc-selftest` / `--cube-field 4000` / `--terrain-test` / `--no-shadows`）全部 `errors=0 warnings=0`、stderr 0 字节；全量回归 96 项 ALL PASS（build/validation/20261126-175133，日志 202 行，FAIL=0）；DocCheck PASS（documents=82 lines=19893）。读数不变（本步不进帧循环）。

## P4-3：三级 clipmap 与页对齐滚动（P4-3，2026-11-26）

- **尺度**：级比 4，三级覆盖 4.8 / 19.2 / 76.8 米，每级 32 x 32 页 = 4096 texel。
  第 0 级 853.3 texel/m，对 CSM 的 103.3 texel/m 是 **8.26 倍**，正好压住验收①的 8 倍门槛。
- **页网格**：每页 128 texel，所以 pageWorldSize = 0.150 / 0.60 / 2.40 m。相机位置拆到光空间
  两条直角轴上分别吸附到 pageWorldSize 整数倍（`snapToPageGrid()`），级中心只会整页跳。
- **矩阵**：`computeClipmapLevel()` 手写正交投影，深度 `z_ndc = (-z_view - r) / (2r)`，
  与 `ShadowPass::buildCascade()` 同一套约定；r 取外接球半径，斜着看也不裁。
- **证据**：静态 130 帧 `scrolls=0 aligned=1`；`--streaming-fly 60` 的 200 帧里每 60 帧
  `scrolls` 增 180（三级 x 60 帧，每帧都跨页）且始终 `aligned=1` —— 位移严格是页边长的整数倍。
- **零开销**：只有 `--dump-vsm-resources`（真的建了图集）才算 clipmap；默认路径输出与 P4-2 一致，
  守卫仍是 `vsm=0B`。
- **读数**：不变（`total=0.065ms`，改前 0.064）。这一步不画任何东西，与预期一致。

## P4-4：页标记（三条失效条件换一张脏页位图）（P4-4，2026-11-26）

- 形状：脏页位图每级 16 个 `uint64` = 1024 位，三级共 384 B，每帧清零重算，纯 CPU。
- 三条失效条件：光源方向变（全脏）/ 级中心整页滚动（整级脏）/ 投影者 `version` 变（只脏它盖住的页）。`version` = 变换矩阵 16 个 float 的 FNV 哈希 ^ `alphaMasked` ^ `alphaCutoff`。
- 页范围换算 `pageRangeForBounds()`：世界 AABB 投到光空间两条轴，除页边长后 `floor` 再裁到 `0..31`；整块在级外返回 false。
- 投影者采集 `ShadowPass::markVirtualShadowPages()`：逐子网格取局部包围盒（`subMesh.hasBounds` / `getNodeBounds` / 模型整体），8 个角过 `world->matrix * getNodeWorldMatrix(node)`；103 个子网格 = 103 个投影者。
- 读数：静态 130 帧稳态 `dirty=0/3072`（这是 P4-4b 跳过绘制、拿下验收②60% 的来源）；临时探针下单个小投影者每帧变身份 `dirty=6~8/3072`、光方向每帧转 0.002 rad `dirty=3072 light=1`、飞行 `scroll=3 dirty=3072`。
- 门控与验证：只在 `--dump-vsm-resources` 下执行（默认路径 `[VSM]` 行 0 条、守卫 `vsm=0B`）；Debug 七组 `errors=0 warnings=0`、stderr 0 字节；全量回归与 DocCheck 见决策 82 的验收表。

## P4-4b：把脏页画进图集（P4-4b，2026-11-27）

- 图集布局改成按级分块：8192² = 2 x 2 块 4096²，前 3 块给三级 clipmap、第 4 块空着；全局页号 = 级号 x 1024 + 级内页号。这样一整级在图上是一块连续矩形。
- `ShadowPass::recordVirtualShadowPages()` 三档执行：0 脏页 → 一条命令不发；某一级 1024 页全脏 → 一条全级绘制（`level.lightViewProjection` + 4096² viewport）；否则逐脏页（CSR 分桶 + 每页 viewport/scissor + `pageViewProjection`）。
- 投影者采集与页标记共用一次遍历，`PageCaster` 与 `CasterBounds` 按下标对齐；一次 `vkCmdBeginRendering` 覆盖整张图集，`loadOp` 在全脏时 `CLEAR`、否则 `LOAD`。
- 读数：飞行场景逐页路径 225,982 / 27,310 / 20,238 draws，整级一条后 309 / 184 / 93；地形场景 frame=1 `draws=15` 之后 `dirty=0` 一条命令不发；静态 Sponza 因相机离几何 190 m 而 `draws=0`（几何不在 clipmap 里）。
- 验证：Debug 七组 `errors=0 warnings=0`、stderr 0 字节；守卫两态；全量回归与 DocCheck 见决策 83 的验收表。
- 已知缺口：外部实例（`--cube-field`、粒子）不在 `MeshComponent` 里，标不到也画不到；图集内容还没有采样者（P4-6 才接）。

## P4-6：页表采样接进着色器（P4-6，2026-11-27）

- `shaders/lighting.frag` 新增 `SampleVirtualShadow(fragPosWorld, N, NdotL)`：`vsmParams.x < 0.5` 返回 -1（回退 CSM），否则按「能覆盖且最细」选级、查页表 entry、3 x 3 tap 返回 `lit / 9.0`；`CalcDirectionalLight` 里 `if (vsm >= 0.0) shadow = vsm;` 直接顶替 CSM 的 `SampleShadow`。`GlobalUbo` 尾部加 `vsmMatrices[3]` / `vsmLevels[3]` / `vsmParams`，`sizeof == 736` 由四条 `static_assert` 钉住。
- 描述符 set 0 新增 binding 20（8192^2 D32 图集，`COMBINED_IMAGE_SAMPLER`）与 21（每帧槽 12 KiB 页表，`STORAGE_BUFFER`）；未启用 VSM 时绑 2D 白替身 + 材质因子缓冲。
- 两处结构性修正：1）每级光相机沿光线退 90 m、近平面贴相机（`z_ndc = t/depthRange`），否则第 0 级只有 8.3 m 厚的深度壳，沿光线挡住它的屋顶全被近平面裁掉、整片判受光；2）bias 从固定 0.001 换成「按级 texel 世界尺寸 x 1.5~4」，`depthRange` 由合并 VP 的 z 行长度反解 1/D，不加 UBO 字段。
- 三个坑：构造里就建好的全局描述符把 binding 20 绑成 CSM 的**比较采样器**（sampler2D 采样恒 1.0 → 整幅无阴影，`vsm-nosh` 只剩 214 px 的文字框差）；回退路径把 `2D_ARRAY` 视图写进 sampler2D（`VUID-vkCmdDraw-viewType-07752`）；`ReflectionPass` 自建的 global set 没写 20/21（开 SSR 才 `VUID-vkCmdDraw-None-08114`）。
- 验收：texel 密度 8.26 倍不变；图集有 caster 剪影、覆盖 96.2%、页表无 `0xFFFFFFFF`、未覆盖 0 px；VSM 与 CSM 分类 1,619,707 px 对 1,619,707 px（`csm-only=0` / `vsm-only=0`）；Debug 七组 `errors=0 warnings=0`、stderr 0 字节；全量回归 97 项 ALL PASS（build/validation/20261127-150426）；DocCheck PASS。
- **本场景量不出像素级阴影差**：方向光只占画面约 4%（清零 83.18 -> 79.46），Sponza 是封闭室内、可见表面 100% 判阴影，所以 `vsm-csm` 恒为 0 是正确结果。基于像素差的判据在这种场景下不适用，替代证据见决策 84。
- 已知缺口不变：外部实例不进图集（P4-4c）；76.8~90 m 由 CSM 兜底；图集跨帧槽共用。
