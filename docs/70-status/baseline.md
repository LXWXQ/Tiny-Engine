---
id: status/baseline
title: 三场景性能基线
type: status
scope: tools/bench.ps1, src/RHI/FrameStats.h, src/RHI/FrameStats.cpp, src/RHI/ResourceStats.h, src/RHI/ResourceStats.cpp, src/RHI/RenderGraph.cpp, src/RHI/Pipeline.cpp, src/Render/SceneGpuBatch.cpp
read-when: 改渲染图 / 屏障 / 管线绑定之前要一份可对比的数字，或怀疑性能回退时
size: ~159 行
updated: 2026-11-24
---

# 三场景性能基线

## 怎么跑

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench.ps1
```

默认 3 场景 × 3 次 × 600 帧，输出 `build/bench/<yyyyMMdd-HHmmss>/baseline.md`，
路径同时写入 `build/bench/latest.txt`。单场景快跑用
`-Scenario lights-512 -Runs 1 -Frames 120`。

判定分两层：exit 码、stderr 字节数、`[Validation] errors/warnings` 是**硬失败**；
FPS 波动超 `-MaxVariancePct` 只是**警告**，加 `-Strict` 才升级成失败——因为波动先反映机器忙，不先反映代码坏了（见下节实测）。

## 相机是固定的

每个场景都带 `--camera -4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`（`Scene::init` 里 Sponza 的默认机位）。
不这么做的话相机来自 `config/engine.json`，也就是"最后谁动过就听谁的"：同一份代码两次跑出的截图不同，
剔除读数也不同。加上 `--camera` 之前，本机 config 里存的是 `(-66.06, 15.92, -76.19)`——站在 Sponza 外面 100 米看向天空，
于是 `default` 场景实测只有 1 draw call / 8192 三角形，`visibleRatio = 0.00`，那种基线测不到任何几何。

## 四个场景

| 场景 | 附加开关 | 压的是什么 |
|---|---|---|
| `default` | 固定机位 | 104 draw calls / 270459 三角形，Sponza 庭院内视角，测几何 + 后处理 |
| `voxel-world` | `--voxel-world 1` | 106 draw calls / 336889 三角形，几何与提交压力 |
| `lights-512` | `--voxel-world 1 --light-stress 512 --clustered-lighting --clustered-gpu-build` | 512 盏点光 + GPU 建簇，光照压力 |
| `cube-field` | `--cube-field 10000` | 1 万个立方体走外部实例注入，压「每帧摊平 + 剔除」这条 CPU 路径（P2 的尺子） |

与 [ue-borrowing-plan](../80-roadmap/ue-borrowing-plan.md) P0-1 的口径偏差：本版之前 `cube-field`
（1 万立方体）在引擎里没有生成器，那一轮用 `--voxel-world 1` 当几何压力场景；2026-11-24 起 `--cube-field 10000` 已落地，几何压力改由它承担，`voxel-world` 保留给体素/地形路径；`editor-default`
用 `--selftest` 的 `default` 代替（编辑器面板不在 selftest 路径上）。两处偏差要以这份表和脚本里的场景表为准。

## 基线（2026-11-23 15:29，Debug，RTX 2070，600 帧 × 3 次；T-04b 默认生效）

| 场景 | FPS（中位） | 波动 % | 帧 ms | passes/帧 | 屏障调用/帧 | sameState | crossQueue | 绑定/帧 | 冗余 % | draw calls | desc binds | 三角形 | 中位 ms | p95 ms | 最长 ms | images/帧 | heap0 MiB | visible |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| default | 121.57 | 2.57 | 8.23 | 21 | 38 | 19 | 0 | 23.5 | 44.7 | 104 | 2 | 270459 | 7.62 | 8.88 | 94.17 | 0.24 | 964.7 | 0.61 |
| voxel-world | 103.66 | 2.81 | 9.65 | 25 | 47 | 34 | 0 | 38.5 | 50.6 | 106 | 4 | 336889 | 8.88 | 10.12 | 97.83 | 0.24 | 964.7 | 1.00 |
| lights-512 | 94.56 | 3.77 | 10.58 | 27 | 47 | 36 | 2 | 39.5 | 49.4 | 106 | 4 | 336889 | 10.12 | 11.88 | 101.23 | 0.24 | 964.7 | 1.00 |

`cube-field` 场景的基线（2026-11-24 10:44，Debug，300 帧 × 3 次）：
`106.57 FPS / 波动 2.89% / 9.38 ms / 16 passes / 27 屏障调用 / 18.9 绑定 / 35.1% 冗余 / 2 draw calls / 333144 三角形 / 中位 9.38 ms / p95 9.62 ms / 最长 137.65 ms / heap0 964.7 MiB / visible 0.62 / instances 10063 / flattenUs 18871`。
`passes = 16` 比 `default` 的 21 少，是因为这个场景不带 `--clustered-lighting`（聚簇的三个 Pass 不进图）。
`flattenUs = 18871` 是每帧把一万个实例摊平成 GPU 记录的时间——这就是 P2 要砍掉 70% 的那一段；
它是 Debug 构建下的数字，和发布构建不可比，但改造前后用同一个构建比就有意义。
注意 `visible = 0.62` 仍是**组级**读数：一万个立方体合并成一个组，组级剔除只能全剔或全留，
P2 要把粒度换成实例级，届时这个数才会真的变化。
上一版（T-04b 之前）是 115.91 / 98.73 / 89.57，`heap0` 1220.7 MiB、`images/frame` 0.27（160 张）。
T-04b 把「只在本帧内读写」的目标合成一张并把 GPU 侧两帧排开，三行分别涨 +4.9% / +5.0% / +5.6%，图少 15 张、堆少 256 MiB。
`default` 的 115.91 仍不能和更早的 164.80 比：那一版相机在场景外，只有天空盒（1 draw call）。
列的含义：

- `屏障调用/帧`：`vkCmdPipelineBarrier2` 的调用次数。T-03 之前它恒等于 `entries`（每个 mip × layer 各发一条）；T-03 之后同一个 Pass 内同键的屏障合进一次调用，于是 `calls < entries`（38 / 47 对 45.2 / 61.2）。
- `entries`：追加的屏障条数（`--stats-frame` 的第二列），是正确性度量——批处理前后不变。
- `sameState`：状态本来就相同、且是读访问，因此跳过的次数（不发屏障是对的）。
- `instances` / `flattenUs`：`[SceneGpu] cull summary` 里的每帧实例数与**结构摊平**耗时（微秒）。`flattenUs` 是 P2 的直接判据：一万个实例现在每帧要摊平近 19 ms，P2 要求降 ≥ 70%。
- `crossQueue`：跨队列资源交给 timeline 信号后跳过的次数。`lights-512` 的 2 是簇表 ranges / indices 两段，也是 T-02 通路是否走通的判据。
- `绑定/帧` 与 `冗余 %`：管线绑定次数，以及"与上一次绑的是同一条管线"的占比。冗余占比是 T-09 状态缓存能消掉的上限，不是它的目标值。
- `中位 / p95 / 最长 ms`：`[FrameTime]` 的桶直方图（0.25 ms 一格，0~250 ms，溢出桶单列）。最长帧出现在启动那几帧（建管线、上传纹理），所以它衡量的是首帧 hitch，不是稳态抖动。
- `images/帧`：`[Resources]` 的资源创建计数，是**全程平均**（145 张图在启动期创建，摊到 600 帧就是 0.24；稳态每帧是 0）。`--no-single-frame-targets` 回退时是 160 张。
- `heap0 MiB`：`[Memory]` 的堆用量最后一次采样（`vmaGetHeapBudgets`，10 Hz）。本机 heap0 是设备本地堆，现在是 964.7 MiB（T-04b 之前 1220.7）；`first` 与 `last` 相等说明稳态不增长。
- `visible`：`[SceneGpu] cull summary` 的 `visibleRatio`（可见组 / 提交组）。P2 的目标是 `visible/submitted ≤ 0.6`；`default` 的 0.61 已经贴着这条线，因为它只有一个房间的视角。

## 同帧目标合成一张（T-04b，默认开）

`--selftest` 默认走「同帧目标合成一张 + 本帧图形工作等上一帧」（`g_singleFrameTargets = true`）。
合并的是只在本帧内写、也只在本帧内读的目标：SSAO.Raw / SSAO / HDR.Scene / SSR / TAA.Output / Bloom 四级 /
平面反射的 4 张 G-Buffer + 深度 + 颜色 / 编辑器离屏视口。G-Buffer 与 TAA 历史保持每帧一份 —— 前者被上一帧的 Hi-Z 读，后者本来就是历史。

| 配置 | lights-512 FPS | default FPS | 图总数 | heap0 |
|---|---|---|---|---|
| 回退（`--no-single-frame-targets`） | 87.77 | 116.93 | 160 | 1220.7 MiB |
| 只开帧间等待（`--serialize-frames`） | 91.13 | — | 160 | 1220.7 MiB |
| 合成一张 + 等待（默认） | 92.50 | 118.90 | 145 | 964.7 MiB |

三行是同批次 600 帧 × 3 次的中位数。**绝大部分收益来自「本帧等上一帧」而不是合并**（87.77 → 91.13），
合并只再加 1.5%，但它顺手把 15 张全屏级目标（含 HDR 与 TAA 输出各 16.6 MB）变成一份，堆少 256 MiB。
`--resize-test` 六次尺寸变化：回退 424 张，默认 319 张（每次 resize 44 → 约 17.5）。

## 波动与判据

同一台机器、同一二进制，只改运行窗口，波动差一个量级：

| 窗口 | default | voxel-world | lights-512 |
|---|---|---|---|
| 120 帧（0.9~1.6 s） | 13.65% | 2.90% | 6.53% |
| 600 帧（3.7~6.8 s），批次 A | 0.91% | 1.21% | 1.88% |
| 600 帧，批次 B | 2.78% | 1.18% | 6.42% |
| 600 帧，本表批次 | 2.30% | 1.55% | 1.83% |
| 600 帧，T-04b 批（本表之上） | 2.57% | 2.81% | 3.77% |

同一场景的**跨批次**中位数也差约 4%（default 166.85 vs 160.82 FPS）。所以：

- 判据用**同批次 A/B 比例**，不用绝对 FPS，也不用跨批次中位数。
- 计划里"三次波动 < 2%"这条，在本机只有 600 帧窗口且机器空闲时才成立；它是机器属性，不是代码属性。
- 想拿"机器是否空闲"的证据时加 `-Strict`，平时不加，否则噪声会误伤提交。

## 什么时候重跑

改渲染图、屏障发射、管线绑定这三处之前跑一次留底；改完再跑一次同批次对比。
两次的数字并排算比例，结论与偏差写进 [milestones](milestones.md)。

## 摊平成本分解（2026-11-24，P2 入场）

`[SceneGpu] flatten breakdown` 只在重建结构的那一帧有样本，所以 `samples` 通常为 1。三组读数（Debug，30 帧自检）：

| 场景 | total | entities | external | merged | mirror | 备注 |
|---|---|---|---|---|---|---|
| default（Sponza，1 实体 824 子网格） | 33.55 ms | 17.9 ms（其中 `bounds` 17.3） | 0.0 | 15.4 ms | 0.1 ms | `bounds` 是一次性索引扫描 |
| `--cube-field 10000` | 56.52 ms | 17.6 ms | 23.3 ms | 8.6 ms | 5.6 ms | 每实例 ≈ 2.3 µs |
| `--cube-field 30000` | 126.25 ms | 17.2 ms | 74.6 ms | 12.7 ms | 17.0 ms | 每实例 ≈ 2.5 µs，线性 |

三条结论：

- 摊平的大头是**逐实例**的，不是排序或拼接（`sort` + `assemble` 合计 < 2%）。P2 的「摊平 CPU 时间 −70%」必须打在逐实例构造上，靠在 `buckets` 里做搬家省不出来。
- `bounds` 的 17 ms 是**一次性**的：`subMeshBoundsFor()` 首次调用要按索引扫整个模型，之后按 `geometryId` 缓存。它让"首帧摊平"看起来比稳态贵一倍多，正确做法是在加载期预热而不是留到第一帧。
- `mirror`（T-08 骨架的持久场景镜像）与实例数线性：10k → 5.6 ms、30k → 17.0 ms。等 P2 让持久缓冲变成权威数据，这笔钱从"额外成本"变成"构建成本"，届时 `flattenUs` 的口径要一起改。

复现：`TinyEngine.exe --selftest 30 --stats-frame --cube-field 10000 --camera -4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`。
## P2 第一步：实例 AABB 走中心/半长，子网格包围盒在加载期预热（2026-11-24）

两处改动，目标是 flatten 里最贵的两项。

**1. 世界 AABB 不再变换 8 个角点。** 原来每个实例对包围盒的 8 个角点各做一次 `mat4 * vec4`，再逐点取 min/max；现在用中心 + 半长：

```
localCenter = (min + max) * 0.5;  localHalf = (max - min) * 0.5;
worldCenter = instanceMatrix * vec4(localCenter, 1);
worldHalf   = abs(basis[0]) * localHalf.x + abs(basis[1]) * localHalf.y + abs(basis[2]) * localHalf.z;
```

对仿射变换这是**精确解**（不是保守近似），代价是 9 次乘法 + 一次 `mat4 * vec4`。`--cube-field 30000` 的 `external` 段因此从 69.2 ms 降到 40.2 ms。

**2. `subMeshBoundsFor()` 提前到加载期。** 它第一次被调用要按索引扫整个模型算每个子网格的包围盒，默认场景（Sponza，103 个子网格、几十万索引）要 16~17 ms，而这段成本原来落在**第一帧**的摊平里。现在 `SceneGpuBatch::prewarmModelBounds()` 在 `Application` 里按 `view<MeshComponent>` 去重预热，启动日志打 `[SceneGpu] prewarm | models=N`。默认场景 `bounds` 段从 16.6 ms 变成 0.0 ms。

摊平总时间（Debug，30 帧，`--camera -4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`）：

| 场景 | 改前 | 改后 | 变化 |
|---|---|---|---|
| default（Sponza） | 33.55 ms | 9.75~10.94 ms | −68% |
| `--cube-field 10000` | 56.52 ms | 34.31 ms | −39% |
| `--cube-field 30000` | 116.99 ms | 72.05 ms | −38% |

**帧率没有变化**（cube-field 10000，120 帧 × 3 轮：改前中位 88.75 FPS，改后 88.20 FPS，在噪声内）。原因是摊平只在**结构重建的那一帧**发生，稳态帧不跑这段：省下来的是首帧/结构变化帧的 hitch，不是每帧的固定成本。`[FrameTime] max` 因此从 90 ms 量级降到 76.5 ms。

剩下的大头（改后）：`merged`（`ensureMergedMesh`，9~13 ms，模型集合变化才跑）、`external`（逐实例 1.3 µs，30000 时 40 ms）、`mirror`（T-08 骨架，与实例数线性）。