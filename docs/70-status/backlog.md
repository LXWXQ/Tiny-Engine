---
id: status/backlog
title: 后续任务台账
type: status
scope: 空
read-when: 准备开始下一项地形/规模化工作，或需要确认剩余任务顺序
size: ~267 行
updated: 2026-11-27
---

# 后续任务台账

本页是后续工作的**执行顺序**；具体算法背景见
[terrain-plan](../80-roadmap/terrain-plan.md)，当前限制见
[known-limits](known-limits.md)，技术债见 [tech-debt](tech-debt.md)。

## 规则

1. 每次只推进一个任务；完成后再把下一项改为进行中。
2. 每项必须有可执行验收：回归测试、日志标记、文档与里程碑。
3. 不把"看起来更快"当完成；必须有数字或可复现行为。
4. 已完成的实现细节归档到 [milestones](milestones.md)，本页只保留状态与验收。

## 队列

| ID | 状态 | 任务 | 依赖 | 完成判据 |
|---|---|---|---|---|
| B-00 | 已完成 | GPU compute 高度场生成 | D-32 碰撞高度场 | GPU/CPU 参考最大误差 < 5e-4；同步/异步生成均使用 GPU |
| B-01a | 已完成 | 独立 chunk 实体 + 相机中心流送基础 | B-00、D-32 | 9 个独立 Model、接缝高度一致、移动卸载/重载、manager 往返序列化 |
| B-01b | 已完成 | 流送异步化与加载硬化 | B-01a | 8 槽 GPU 高度场异步请求；Scene 三阶段流水线；描述重建恢复 9 chunks |
| B-01c | 已完成 | 流送交互与性能硬化 | B-01b | chunk picking→manager；删除/撤销/重做；JSON 文件加载 smoke；max update 记录 |
| B-02 | 已完成 | 拓扑级 LOD stitching | B-01c | 全分辨率边界 + 粗内部；9 个 LOD 对/18 条边界无裂缝；skirts=off |
| B-03 | 已完成 | GPU LOD 选择 / GPU-driven culling | B-02 | compute 选层/剔除/写 indirect count；GPU/CPU parity + 过渡 PASS |
| B-04 | 已完成 | GPU compute 网格组装与 GPU 上传 | B-00、B-03 | `terrain_mesh.comp` 写 device-local 顶点/索引；单 chunk GPU/CPU 逐项 parity；流送 chunk 实际走 GPU mesh |
| B-05 | 已完成 | 异步 GPU 上传管线 | B-04 | 4 槽异步 submit/poll；257² 单 chunk 主线程 maxPoll 0.99ms 对比同步 8.36ms；流送 9 chunk 全 GPU |
| B-06 | 已完成 | 组件私有字段 + 统一销毁 + 区域索引 | D-33 | 编译器禁止 public 字段直写；生产销毁统一走 Scene（独立命令栈自检例外）；按区域查询 chunk |
| B-07 | 已完成 | 物理/角色控制器 + 地形雕刻 | B-02、D-32 | 角色贴地/碰撞稳定；笔刷修改只重建受影响区域 |
| B-08 | 已完成 | 地形内容资产与大规模材质 | B-02 | 4 个 Set2 材质集；外部 16 PNG 按需加载/程序化回退；`materialSetIndex` 序列化；按材质批次重绑（应用上限 4 集） |


## 已完成验收与归档队列已迁出

> B-00～B-08 的逐项验收（GPU 高度场误差、流送硬化、LOD stitching、异步上传、雕刻、
> 材质集…）与 2026-11-05 归档队列里**已完成**项（V-01、R-01…R-11、F5-*、F6-0x、E-P1…），
> 实现细节已在 [milestones-archive](milestones-archive.md) 的 D-34～D-44 与日期条目里；
> 本页不再复写验收标准——同一事实两处各写一遍，改一处必漏一处。

## 仍未开始 / 已被取代（2026-11-27 复核）

下表由上表筛出，只保留**还没有落地**的项目；完成一项就删一行，并在 milestones 追加记录。


| ID | 状态 | 任务 | 依赖 | 完成判据 |
|---|---|---|---|---|
| V-02 | 未开始 | 体素网格化的每帧预算与取消（当前每块一个 job，`processVoxelChunkBuilds()` 每帧最多上传 4 块，但提交阶段没有总预算） | V-04、JobSystem | 半径 12 首帧提交仍有 7.9 ms（Release），要压到 <2 ms；取消后不再上传 |
| V-03 | 未开始 | 体素 LOD（远距离 chunk 粗网格） | V-02 | 半径 24 的 triangles 显著下降，近处不糊 |
| V-05 | 未开始 | 体素世界分页/无限世界（当前半径上限 12，`Scene::kMaxVoxelRadius`） | V-04 | 半径 24 显存与实体数可控 |
| F6-03 | 未开始 | Compute Bloom（金字塔迁到 compute） | F6 | 输出与当前 bloom 视觉一致、GPU 时间下降 |
| F6-04 | 未开始 | 切线空间法线贴图（启用 `getNormalFromMap()`，先补切线或 TBN） | F4 | 法线贴图生效；`normalMaps` 不再白绑（tech-debt #3） |
| F4-01 | 未开始 | glTF 保留 node 层级（决策 13 的翻转）、primitive 独立变换、异步加载 | — | 载入后可改任意 node 局部变换并实时可见 |
| F4-02 | 未开始 | 模型 LOD 链（GPU 端 LOD 选级的前置，见 F1 剩余项） | F4-01 | 同一模型可切 2~3 级，切换无跳变 |
| F1-R1 | 已取代 | 粒子不接入 `ExternalInstance`：改由 F5-07 的独立组件与 `ParticlePass` 落地 | F5-07 | 不再以复用 ExternalInstance 为目标；粒子独立缓冲、独立前向 Pass |


## 更新方式

完成任务时：

1. 本页把状态改为 `已完成`，补一行里程碑/验证日志路径；
2. 在 [milestones](milestones.md) 追加 D-xx 记录；
3. 同步 `known-limits`、`tech-debt`、`terrain-plan` 中仍然成立/已撤销的条目；
4. 运行 `tools\Validate.ps1 -Incremental -SkipSetup` 与 `tools\DocCheck.ps1`。

## P2 待办：实例级剔除（remap）

**现状**：剔除是组级的（`shaders/scene_cull.comp` 每个组一次判定，写 5 word 的 indirect 命令）。一万个立方体合成 1 个组，组一旦可见就画满整组，`visibleRatio 0.62` 说的是「104 个组里 64 个可见」，与实例数无关。实测（`747a3e5` 起的只读度量，Release 40 帧每帧强制重建）实例级视锥可见比例：cube-field 10k **0.676**、30k **0.372**——目标 `visible/submitted <= 0.6` 在 30k 靠纯视锥就能过线，10k 需要叠加 Hi-Z。

**为什么分三步**：它能用纯布局/统计方式验证的部分先做，避免一次动 shader、描述符、顶点取值三处而无法定位。

1. **布局扩容（纯验证）**：`shaders/scene_cull.comp` 的 `stats[3]` 扩成 `stats[4]`；`src/Render/SceneGpuBatch.h` 的 `Stats` 现在是 13 个 `uint32_t`（对应 3 个 `uvec4` 的 12 个 + 1 个），必须补到 16 个（加 3 个 pad），否则扩 slot 会越界。验收：三档 `--stats-frame` 读数与原值逐位相同，`--validation-features sync` `errors=0`。 **已落地（2026-11-25，决策 58）**：`stats[4]` + `Stats` 16 个 uint + 反射头断言，三档读数逐位不变（见下方第 1 步读数）。
2. **绑实例缓冲 + GPU 统计**：`src/Render/SceneGpuBatch.cpp:156-175` 的 `computeBindings[5]` 加第 6 个 binding（`InstanceRecord[]`），池大小与 set 写入同步（`:190-202`、`:380-410`）；shader 对可见组的实例做视锥测试并 `atomicAdd` 计数。验收：GPU 侧比例与 CPU 侧 0.676 / 0.372 对得上（允许 ±2%）。 **已落地（2026-11-25，决策 59）**：`computeBindings[5] → [6]`（binding 5 = `InstanceRecord[]`）、push constant 16 → 32 字节（`uvec4 more`：`count` + `mode`）、同一命令缓冲里的第二遍 dispatch 写 `stats[3]`（测过 / 可见），`logCullSummary` 多打一行 `[SceneGpu] instance summary`。口径改成测整条实例缓冲（组门控要的「实例 → 组」表留到第三步），读数见下方第 2 步读数。
3. **紧凑索引表 + remap 取实例**：shader 写出可见实例的紧凑索引，`mrt.vert` 等走 `InstanceRecord` 的顶点着色器改为经该表索引，`vkCmdDrawIndexedIndirect` 的 `instanceCount` 用 remap 后的数量。验收：`instFrustum` 的比例变成实际提交比例，`--no-instance-cull` 可整体回退且画面逐像素一致。 **已落地（2026-11-25，决策 60）**：shader 用「测视锥 + 领名次 / 组前缀和 + 改命令 / 搬 remap」三段 dispatch 把可见实例压成每组一段紧凑区间（binding 6 = 每组 `uvec4`、binding 7 = `remap + 名次`），`mrt.vert` 经 set 3 binding 1 的 remap 表取记录，`--no-instance-cull` 写恒等表回退，读数见下方第 3 步读数。

**第 1 步读数（2026-11-25，纯布局验证）**：三档（default / cube-field 10k / 30k，120 帧固定机位）改前改后逐行比对 `[SceneGpu] cull stats` 与 `cull summary`，整数读数逐位相同：groups `103/104/104`、visible `63/64/64`、culled `40/40/40`、instances `63/10063/30063`、hizTests `63/64/64`、hizCulled `0/0/0`、screenTests `0/0/0`、external `0/10000/30000`、visibleRatio `0.61/0.62/0.62`，CPU 侧 `instFrustum` 仍是 `0/0`、`6756/10000`、`11157/30000`；只有 `flattenUs` / `ms/rebuild` 这类计时列不同。`--validation-features sync`（10k）`errors=0 warnings=0`、stderr 0 字节。也就是说第 2 步加 `InstanceRecord[]` binding 与 `atomicAdd` 时，`stats[3]` 这 4 个 word 已经有宿主、也不会再踩到别人。
**第 2 步读数（2026-11-25）**：新增 `[SceneGpu] instance summary` 行，120 帧固定机位三档：default `gpuInst=0.58 (7080/12154) tested/frame=103`（CPU 侧无外部实例，没有对照）；cube-field 10k `0.6747 (804288/1192154) tested/frame=10103` 对 CPU `instFrustum=6756/10000=0.6756`；30k `0.3726 (1323606/3552154) tested/frame=30103` 对 `11157/30000=0.3719` —— 差 0.09 / 0.07 个百分点，落在 ±2% 判据内。组级读数与第 1 步逐位相同（`103/63/40`、`10063 / total 9.06 ms`、`30063 / 17.33 ms`），三档 `errors=0 warnings=0`、stderr 0 字节；`--validation-features sync`（10k）同样干净。代价：30k 600 帧 × 3 轮交替 A/B，第二遍 dispatch ≤0.5%，median 帧时间同为 7.62 ms。全量回归 **96 项** ALL PASS（stderr 全 0，`build/validation/20261125-111243`）；DocCheck PASS（documents=81 lines=18077）。
**第 3 步读数（2026-11-25）**：提交实例数第一次等于实例级可见数 —— 120 帧固定机位，`cull summary` 的 `instances/frame`（= `stats[0].w`）从「可见组实例数之和」变成「紧凑后真正提交的实例数」：default `63.0 → 59.0`、10k `10063.0 → 6815.0`、30k `30063.0 → 11216.0`，与 `instance summary` 的 `submit/frame` 逐位一致；`gpuInst`（`0.58` / `0.6747` / `0.3726`）与第二步逐位相同，每帧比提交数多 1 个实例（所在组被组级剔除，紧凑化按 `commands[base+1] == 0` 跳过）。收益（600 帧 × 3 轮，30k）`142.19/142.86/142.08` 对 `125.86/126.09/125.66 FPS`（+13.1%，median 6.88 对 7.88 ms）；default 场景 202.55/203.82 对 204.81/204.65（−0.6~1.1%，实例太少时四段 dispatch 是纯开销）。`--no-instance-cull` 回退：两版 1920×1080 截图共 832 个差异像素、全部落在 HUD「Performance」面板内，面板外 0 差异（两次同样配置的运行本身也有 285 个像素在跳）。验证必须用 Debug（Release 的 `enableValidationLayers` 在 `NDEBUG` 下是 `false`）：三档 + `--no-instance-cull` + `--voxel-world 1/12` + `sync`（10k）+ `--cube-field-motion 500 --rebuild-structure 7` 全部 `errors=0 warnings=0`、stderr 0 字节。全量回归 **96 项 ALL PASS**（`build/validation/20261125-121228`）。

**第 4 步（测量，否决）读数（2026-11-25）**：逐实例 Hi-Z 遮挡「只数不裁」量了一遍 —— 参考机位 `--cube-field 10000` 120 帧执行 790656 次、判定被遮挡 **0**；平视三个机位 376264 / 391384 / 409416 次、`--voxel-world 1` 5768 次，全部 0；探针（`occludedByHiZ` 临时 `return true`）能让 `occInst` 跳到 0.66，链路是通的。原因是自遮挡（金字塔来自上一帧）、背景 `kFarDepth=1e9` 污染粗 mip、以及俯视平地上的阵列屏幕不重叠。结论：收益为 0，代码回退，`≤ 0.6` 的差距是机位相关的几何值（10k `0.67`、30k `0.37`）。

**回退开关**：`--no-instance-cull`（默认开）。关掉后 GPU 写恒等 remap、命令不再改写实例区间，顶点着色器仍走同一段取下标代码，画面与默认路径逐像素一致（验证方式见第 3 步读数）。

## P2 待办：灯数据搬进灯缓冲（P2-6）

**现状（搬家前）**：点光源数组在 `GlobalUbo.pointLights[MAX_POINT_LIGHTS = 100]`（`src/Core/FrameInfo.h`），3.2 KiB / 帧槽，灯数硬上限 100；`lighting.frag` 的逐灯循环直接读 UBO 数组。roadmap 的 P2-6 要求把它换成「灯数 + 缓冲句柄」，上限由缓冲容量决定，也是 P7-5 随机采样的前置（`docs/80-roadmap/ue-borrowing-plan.md:560`、`:738`）。

**已落地（2026-11-25，决策 62）**：UBO 删数组、只留 `numLights`（`sizeof(GlobalUbo)` 3680 → 480）；灯进 **set 0 binding 19** 的 SSBO（容量 `kMaxPointLights = 1024`、32 KiB / 帧槽、`DescriptorManager` 持有常驻映射缓冲，`uploadLights()` = `memcpy` + 整块 flush）；`applyLightBinding()` 覆盖全局 set 与反射通道自建 set 0；`--light-stress` 超过容量的部分被夹取而不是被静默截到 100。读数见下。

**读数（2026-11-25）**：容量 —— `--light-stress 0 / 100 / 512 / 1024 / 1500` 分别打 `applied=0 / 100 / 512 / 1024 / 1024`（最后一条 `requested=1500`）。画面（1920×1080 第 30 帧、固定机位、半径 12）：0 vs 100 盏 118,653 个像素不同（≥8 的 311 个）、100 vs 512 盏 208,703（≥8 的 2,446）、平均亮度 97.87 → 98.01；逐灯 512 vs 聚簇 512 差 9,785（0.47%），而「只上 100 盏」对聚簇 512 差 182,807（9.5%）。验证（必须 Debug）：六组用例 `errors=0 warnings=0`、stderr 0 字节；全量回归 **96 项 ALL PASS**（`build/validation/20261125-132707`）。

**回退开关**：无。这一步是纯数据通路搬家，没有行为开关；要回到旧行为只能回退提交（`pointLights` 数组与三份 shader 声明会一起回来）。

## P2 待办：退役组级剔除（P2-5）

**现状（退役前）**：剔除是「一条组 → 一次判定」，组的 AABB 过视锥/屏率/Hi-Z，不过就把整条 5 word 间接命令清零。`GroupRecord` 64 字节（两个 `vec4` 包围盒 + `draw` + `counts`），组 AABB 同时被 `gpuScene.ensurePrimitive` 使用。roadmap 的 P2-5 要求「`GroupRecord` 只保留合并网格的索引区间，剔除不再按组」，理由是合并缓冲的收益在 draw call、不在剔除（`docs/80-roadmap/ue-borrowing-plan.md:559`）。

**已落地（2026-11-25，决策 63）**：`GroupRecord` 32 字节（`uvec4 draw` + `uvec4 counts`）；模式 0 改名 `kModeCommandSetup` 只写基线命令；可见性完全由模式 1 的逐实例判定决定，模式 2 成为唯一裁决策；屏率误差剔除降到实例粒度（公式不变）；组级 Hi-Z 遮挡退役（金字塔仍在建）。`shaders/scene_cull.comp` 395 → 325 行。

**读数（2026-11-25）**：`instances/frame` default 59.0 → 60.0、10k 6815.0 → 6816.0、30k 11216.0 → 11217.0，提交量第一次等于逐实例可见数（60 / 6816 / 11217，分别等于 `gpuInst` 的 7080、804288、1323606 除以 118 帧）；`gpuInst` 不变（0.58 / 0.6747 / 0.3726）；`visibleRatio` 0.61/0.62/0.62 → 0.58/0.59/0.59（口径变化，不是退化）。画面：default 面板外 0 像素差异，10k 面板外 9,425 像素变化全在左下角 `(0,1000)-(151,1079)`（旧组级判定误杀的实例）。验证（必须 Debug）9 组 `errors=0 warnings=0`；全量回归 **96 项 ALL PASS**（`build/validation/20261125-140206`）。

**回退开关**：无（这一步删代码，回退只能 revert 提交）。`--no-instance-cull` 的语义在本次改变：从「退回组级剔除」变成「完全不剔除」（模式 0 基线 + 模式 4 恒等 remap），提交量等于实例总数。

**遗留**：Hi-Z 金字塔仍在构建但没有消费者（`stats.occlusionTests/Culled` 恒 0，`--no-hiz` 仍可用），需要在「逐实例遮挡复活」与「整条拆除」之间选一个。

## P2 待办：实例记录的上一帧世界矩阵（P2-2 收尾）

**现状**：`InstanceRecord` 只有当前帧的世界矩阵与法线矩阵，`GpuScene::Instance` 也一样；P5-1 的运动矢量需要「上一帧的模型矩阵」才能算出重投影位置，计划里 P2-2 的字段清单本来列了它，T-08 落地时缺这一项。

**已落地（2026-11-26，决策 64）**：`InstanceRecord` 224 → 288 字节（尾部 `glm::mat4 previousModel`，offset 224 有 `static_assert`）；`GpuScene::Instance` 144 → 208（`previousTransform`）；`shaders/scene_cull.comp`、`shaders/mrt.vert` 的镜像结构同步加字段（只为 stride，本步不读）。填值：实体 `worldTransform.previousMatrix * nodeMatrix`、外部缓存命中 `external.transform`、未命中但同绑定 `cache.transform`、首次出现用当前帧。新增 `[SceneGpu] instance history | instances=… moved=… (last rebuild)`。

**读数（2026-11-26，Release 120 帧）**：default `instances=103 moved=0`、10k `10103/0`、30k `30103/0`、`--cube-field-motion 500` `10103/10000`；三档 `instances/frame`（60 / 6816 / 11217）与 `gpuInst`（0.58 / 0.6747 / 0.3726）与 P2-5 逐位相同。MSBuild 与 Ninja 两套构建读数一致。

**回退开关**：无（纯数据铺设，没有分支可切；要退只能 revert 提交）。

**遗留**：P5-1 的 velocity 消费者未接（这一刀只铺数据）；`--rebuild-structure n` 时「上一帧」退化成「上次重建」，P5-1 落地时要重新确认；A2（阴影/反射各一份剔除与紧凑可见表）尚未开工。

**收尾补丁（2026-11-26）**：`shaders/mrt.frag` 补 `previousModel`、`kTerrainChunkInstanceStride` / `kVoxelBatchInstanceStride` 改 288；漏改会让片元材质与地形/体素合批读错位（验证层与读数都查不出，靠像素 A/B 才发现）。补丁后回归 96 项 ALL PASS（`build/validation/20261126-094557`）。

## P2 待办：反射视图自己的剔除与紧凑命令（A2 上半）

**现状**：反射通道走 CPU 逐实体（`GeometrySystem::render(cmd, frameInfo, &viewOverride)` 的每个 GPU 批分支都带 `!reflection`）；主视图的命令按主视锥算，反射不能复用。

**已落地（2026-11-26，决策 65）**：`SceneGpuBatch` 加视图层（`kMaxViews = 2`、`ViewResources`），`prepareView` / `recordDrawView` 按视图跑四段 dispatch 与间接 draw；实例与组表只由主视图上传；描述符池与写入按 `kFramesInFlight * kMaxViews` 扩；反射的 compute 剔除放进独立的无参数 pass；门槛沿用 `useGpuPath && hasInstances`。

**读数（2026-11-26）**：水面 + 400 外部实例场景 `reflection view | groups=103 instances=503 cull=on`；主视图 `gpuInst=0.91 (26680/29174)`、`submit/frame=460`；与 HEAD 的 CPU 反射逐像素对比 1920×1080 差 347 像素（HUD 外 8 个、差值 1/255）；Debug 六组 `errors=0`；全量回归 96 项 ALL PASS（`build/validation/20261126-102435`）。

**回退开关**：无独立开关；实例数低于 `gpuSceneMinInstances`（默认 256）时反射自动回到 CPU 逐实体，`--no-instance-cull` 时反射视图与主视图一起走恒等 remap。

**遗留**：阴影视图（A2 下半）未接；反射视图的 `stats` 不回读（只有一次性 `reflection view` 行）；`config/engine.json` 的相机写回会让 `terrain-render` 缺 marker，跑回归前必须还原。
## P2 待办：阴影视图自己的剔除与紧凑命令（A2 下半）

**现状**：主视图与平面反射已有视图层（决策 65），阴影仍是 CPU 逐子网格提交 —— 每帧 3 个级联 × 所有静态网格，首帧 `drawCalls=309`。

**已落地（2026-11-26，决策 66）**：`SceneGpuBatch::kMaxViews = 5`（0 主视图、1 反射、2/3/4 级联）；`prepareView(..., bool screenSpaceTest = true)` 与公开的 `drawMergedIndirect`；新 `shaders/shadow_gpu.vert`（set 3 实例表 + remap，push 只留光空间矩阵）；`ShadowPass` 并存两条管线，三个级联在渲染通道之前各派发一次四段 dispatch，渲染里每个级联一条 indirect；`Application::initAll()` 之后 `setSceneGpuBatch()`。

**读数（2026-11-26）**：Debug 四组 `errors=0 warnings=0`（含 `--no-instance-cull`、`--voxel-world 1` 不强开的情形）；阴影 A/B 第 40 帧总差 77,254 像素、δ≥8 仅 2,681（0.13%）、全在阴影边缘；主视图 `gpuInst=0.91 (17480/19114) submit/frame=460`；全量回归 **96 项** ALL PASS（`build/validation/20261126-111338`）。

**回退开关**：无独立开关；`useGpuPath && hasInstances` 不成立时（实例数低于 `gpuSceneMinInstances`，或持久化被关）阴影自动回到逐模型路径。

**遗留**：阴影视图用上一帧的组/实例表（视图 0 的 `prepare` 在几何 pass 里，晚于阴影），每帧重建的场景滞后一帧；地形/体素/动态物体仍走逐模型路径。
## P5 待办：运动矢量与 TAA 重投影（P5-1 数据面已落地）

**现状**：TAA 用「当前帧/上一帧矩阵」反推重投影，没有逐像素运动矢量；相机静止、物体运动时会把旧像素混进来。

**已落地（2026-11-26，决策 67）**：`GBufferFormat::Velocity`（RG16F）作为第 6 张 G-Buffer 附件；`mrt.vert` 算当前/上一帧裁剪位置、`mrt.frag` 写 `location = 4`；天空盒/网格/几何天空/植被写 0；`GeometrySystem` / `VegetationPass` 管线扩到 5 个颜色附件；反射 G-Buffer 同步补第 5 张（否则 `VUID-vkCmdDrawIndexed-Indirect-colorAttachmentCount-06179`）。

**读数（2026-11-26）**：Debug 三组（默认 / `--streaming-world 2 --gpu-scene-external 400` / `--cube-field 10000 --cube-field-motion 500`）全部 `errors=0 warnings=0`、stderr 0；本步无消费者，画面不变。

**回退开关**：无（附件恒在，速度为 0 时等价于「没有运动」）。

**遗留**：P5-2 把 TAA 改成读 velocity + YCoCg 方差裁剪；P5-3 薄几何/去遮挡检测；P5-4 运动模糊改读 velocity；地形/体素合批的实例行 `previousModel` 还是 0 矩阵（当前不被读）。

## 其他遗留：Hi-Z 金字塔没有消费者（2026-11-26）

P2-5 退役组级遮挡后，`scene_cull.comp` 不再声明 `hizPyramid`，但 `GeometryPass` 每帧仍然 `buildHiZ`（`--no-hiz` 可关）。两种处理：接给 P6 meshlet 的遮挡输入，或者整条链拆掉。暂时保留，别在没量过代价前删。

**P5-1 全量回归（2026-11-26）**：96 项 ALL PASS（`build/validation/20261126-114143`），stderr 全 0 字节。

**反射视图的 stats 回读（2026-11-26 已做）**：`readStats(frame, viewIndex)` 对 view 1 累加到独立的 `reflectionTotals`，日志多一行 `[SceneGpu] reflection summary | …`；实测 `visibleRatio=0.30 / visible=31 culled=72 / submit/frame=430 / gpuInst=0.85`（同帧主视图 0.23 / 24 / 423 / 0.84）。

**P5-2（2026-11-26，决策 68）**：TAA 改读速度图重投影（速度为 0 时回退矩阵），3×3 邻域换成 YCoCg AABB ∩ `mean ± 1.25σ`；TAA 的 set 1 扩到 4 个采样器。读数：运动场景离 GT 更近（mean 4.831 → 4.775），静态两版互差 mean 0.572；Debug 两组干净。

**待补**：`--velocity-probe`（把速度图拷回主机，与 CPU 端投影比对，判据 < 0.5 像素）与「静态区域时域方差 −30%」的自动化统计 —— 这两条是 roadmap 给 P5-2 定的量化验收，尚未实现。

**P5-2 全量回归（2026-11-26）**：96 项 ALL PASS（`build/validation/20261126-122409`）；第一次跑到 `f2-streaming-world` 时 JobSystem 并行加速比判据没过（当时并发了别的活，CPU 被抢），单独重跑三次都是 5.65~6.66x PASS —— 那条判据对机器负载敏感。

**P5-2 验收补实（2026-11-26，决策 68 补测）**：`--velocity-probe` 与 `--taa-matrix-reprojection` 落地。读数：静态 `meanDiff=0.475/0.484 px`、`maxDiff=0.487 px`、`>0.5px=0.00%`（`< 0.5 像素` 判据达标，下限由 `R16G16_SFLOAT` 决定）；运动 `meanDiff=1.195/1.285 px`、`>0.5px=76.10%/79.75%`。`静态区域时域方差 −30%` **未达成**（整帧时域差 2.395 vs 2.370），机理见决策 68。Debug 三组干净。

全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/20261126-125624`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19016）。

**P5-3（2026-11-26，决策 69）**：邻域换 5×5 十字并**按世界半径过滤抽头**，夹制盒只用有效抽头（σ 1.25 → 1.0），覆盖率把历史权重压到 0.45~1.0 倍；`--taa-classic-kernel` 回退。读数：植被薄几何 mean 1.339 → 1.300（−2.9%）、`>16` 1.64% → 1.44%（−12.2%）；静态/运动立方体 mean 略差 0.8%~1.1%、`>16` 略好 1.1%~3.0%。Debug 四组干净。

全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/20261126-131220`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19076）。

**P5-5（2026-11-26，决策 70）**：新增 `src/Render/TemporalUpscaler.h`（`TemporalUpscalerInputs` + `ITemporalUpscaler`），`TAAPass` 多继承 `IRenderPass` 与它，`Application` 只认 `ITemporalUpscaler*`（`temporalUpscaler`），每帧抖动走接口。验收：改前/改后二进制同配置第 35 帧逐像素只差 155 个像素（全在 HUD 的 FPS 文字里，HUD 外 0 个）；Debug 三组 `errors=0 warnings=0`、stderr 0 字节。

**回退开关**：无（`--no-taa` 关掉的是时域部分，接口本身没有开关意义）。

全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/build/validation/20261126-133227`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19133）。

**P5-4（2026-11-26，决策 71）**：运动模糊改读逐像素速度图（`PresentPass` 两条路径各加一个最近邻速度图采样器，binding 3 / 2），速度为 0 的像素回退到上一帧 VP 的相机重投影；回退开关 `--motion-blur-matrix`。读数：静态场景两条路径逐像素一致（HUD 外 0 个像素），运动立方体场景 270,179 像素不同（13.029%）、δ≥8 的 2.261%，差异集中在物体轮廓与细栏杆。Debug 四组干净。

**回退开关**：`--motion-blur-matrix`（忽略速度图，退回全屏相机速度）。

**遗留**：P5-5 的 `ITemporalUpscaler` 只覆盖时域上采样，运动模糊仍是 Present 里的一段代码，不参与接口。

全量回归（2026-11-26，Debug）**96 项 ALL PASS**（`build/validation/build/validation/20261126-135254`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=19198）。

## P3 待办：绘制命令与 PSO（P3-1 ~ P3-5）

**现状**：P3 的第二条验收（首帧 hitch）在 2026-11-26 立起尺子，基线 `windows=4 violations=0 worst=2.54x`，已达标。

**已落地（尺子）**：`src/RHI/FrameStats.cpp` 的 60 帧 hitch 窗口 + `[FrameTime] hitch` 行 + 两个窗口的 `selfTest`。

**测量结论**：
- 「状态切换下降 ≥ 40%」：`[StateCache] saved=34.5%`（T-09 已吃掉），`pipelineBinds/frame=27.5 redundant=9.5`。
- 剩余冗余两处：跨 pass 重复绑同一条管线、`vkCmdBindVertexBuffers` / `vkCmdBindIndexBuffer` 每实体重复发。
- P3-4 的「PSO 首用即建」不成立：所有管线都在 `initAll()` 里创建，预热只能省启动时间。

**已落地（2026-11-26，P3-2 前一半）**：`StateCache` 现在也记顶点/索引缓冲的「上次绑的是谁」（`bindVertexBuffer` / `bindIndexBuffer`），接进 `Model::bind()`、几何 arena、合并网格、阴影地形 arena 四处。读数（Release）：地形流式场景顶点 31.5 → 12.9 次/帧（省 59.0%）、索引 28.5 → 8.0（省 72.1%）；cube-field 与默认场景 5.0 → 3.0（省 40%）。画面与 `--no-pipeline-cache` 逐像素一致。

**下一步候选**（按收益/成本排序）：
1. TM-12 的着色器参数块从 SSAO 推广到几何/光照 pass（P3-5，纯收益项）。
**P3-5 已落地（2026-11-26，决策 74）**：几何 pass 的 push 参数块与光照 pass 的 set 0（`GlobalUbo` + 绑定号）
改由 `mrt.vert.reflect.h` / `mrt.frag.reflect.h` / `lighting.frag.reflect.h` 当唯一真值，
共 3 条块大小 + 35 条成员偏移 + 3 条连续性断言。代价是反射头成了编译前置（换生成器后要先编译一次）；
描述符类型与数量仍手写，留给下一步。**回退开关：无**（只加编译期断言，运行期行为逐位不变）。
- **P3-5 第二步已落地（2026-11-26，决策 75）**：`tools/SpirvReflect.cpp` 多生成 `k<Id>Kind`（整数种类，`0` = 未映射）；新增 `src/RHI/DescriptorReflect.h`（`kDescriptorType<Kind>` 编译期映射，未映射落到 `VK_DESCRIPTOR_TYPE_MAX_ENUM`）；`DescriptorManager::createGlobalLayout()` 与 `SceneGpuBatch` 的计算/set 3 布局的类型与数量全部改认反射；断言 20 条。Debug 5 组验证层干净、Release 读数不变、回归 96 项 ALL PASS。**回退开关：无**（纯编译期重排，读数不变）。遗留：`tools/SpirvReflect.cpp` 的 `descriptorKindCode()` 与 `src/RHI/DescriptorReflect.h` 的编号是人工同步（两处都有注释，没有自动比对）。

2. 阴影 pass 的地形 arena 索引绑定也接进缓存（当前只接了顶点绑定）。
3. `MeshDrawCommandList` + 桶排序：只有在上面两件事之后仍有明显跨 pass 冗余时才值得做。

**回退开关**：`--no-draw-command-list`、`--no-pso-precache`（这两个开关尚未实现，等对应的机制落地时一起加）。

## P4 待办：虚拟阴影图（P4-1 → P4-5 → P4-2 → P4-3/4 → P4-6）

**已定（2026-11-26 用户裁决，见决策 77 / 78）**：
1. 验收基准：CSM 分辨率 2048 → 4096。三档固定基准 —— 静态 Sponza 0.204 ms、静态 `--cube-field 20000` 0.381 ms、动态 `--cube-field 20000 --cube-field-motion 2000` 0.442 ms。不再去找网上的重型场景：Crytek Sponza 包与 Sun Temple 都 404，Bistro / San Miguel 是 0.4~3 GB 的 OBJ 包 + 手写场景 json。
2. 顺序：P4-1 → P4-5 → P4-2 → P4-3/P4-4 → P4-6。P4-5（阴影几何复用 `visibleIdList`）提前，因为第零步读数显示逐模型 309 draw 比 GPU 合并路径慢 2.5 倍，提交开销是基线大头。
3. `SparsePageAllocator` 不与 P7 距离场共用：保持单图集、定长 128² 页、uint 环形队列的最小形状。

**进度**：
- [x] P4-1 稀疏页分配器：`src/Render/SparsePageAllocator.{h,cpp}` + 启动自检 + `--page-alloc-selftest`（20 万次随机操作对账，Release 8.3 ns/op、peak=4096）
- [x] P4-5 阴影几何复用 `visibleIdList`：`ShadowPass` 每级联一次 `prepareView(视图 2/3/4)` + 一条 `drawMergedIndirect`，替掉 103 子网格 × 3 级联 = 309 次 CPU 提交；判据 `SceneGpuBatch::isShadowBatchReady()` 故意不吃 `gpuSceneMinInstances`；回退 `--no-shadow-batch`。实测 −24~28%（0.087 → 0.063~0.066 ms），**门槛②的 60% 未达成**（见决策 79）
- [x] P4-2 图集 / 页表 / 每页 HZB：`src/Render/VirtualShadowMap.{h,cpp}` 定死布局（8192² 图集 / 4096 张 128² 页 / 每帧槽 12 KiB 页表 / 每页瓦片横排 7 级 HZB，127/128 texel），启动自检八项整数断言；显存只在 `--dump-vsm-resources` 时建（512.0 MiB + 24.0 KiB），零开销守卫新增 `| vsm=<bytes>B`。读数不变（本步不进帧循环，与预期一致）
- [x] P4-3 三级 clipmap 与页对齐滚动：级比 4、4.8 / 19.2 / 76.8 米，每级 32 x 32 页 = 4096 texel；第 0 级 853.3 texel/m = CSM 的 8.26 倍（验收①的分母）；中心按光空间两轴吸附到页网格（0.150 / 0.60 / 2.40 m），静态 130 帧 `scrolls=0`、飞行每 60 帧 `scrolls` 增 180 且 `aligned=1`。只在 `--dump-vsm-resources` 下计算，读数不变（决策 81）
- [x] P4-4 页标记：`VirtualShadowMap::markPages()` 三条失效条件（光源方向 / 级中心整页滚动 / 投影者 `version`）换一张每级 16 个 `uint64` 的脏页位图（三级共 384 B）；投影者由 `ShadowPass::markVirtualShadowPages()` 逐子网格取世界 AABB（过节点矩阵）。静态 130 帧稳态 `dirty=0/3072`、单个小投影者每帧变身份 `dirty=6~8/3072`、光方向每帧转 0.002 rad `dirty=3072 light=1`、飞行 `scroll=3 dirty=3072`（决策 82）
- [x] P4-4b 把页画进图集 + 跳过未脏页：`ShadowPass::recordVirtualShadowPages()` 三档（0 脏页不发命令 / 整级全脏走「整级一条」/ 其余逐脏页 CSR 分桶）；图集布局改成按级分块（2 x 2 块 4096²，全局页号 = 级号 x 1024 + 级内页号）。飞行场景 225,982 / 27,310 / 20,238 draws → 309 / 184 / 93；地形静态 frame=1 `draws=15` 之后一条命令不发（决策 83）
- [ ] P4-4c 把外部实例纳入页标记与绘制：`--cube-field` / 粒子 / `setExternalInstances` 不在 `MeshComponent` 里，这一版标不到也画不到（实测 `--cube-field 4000 --cube-field-motion 200` 是 `draws=0`）
- [x] P4-6 `shaders/lighting.frag` 页表采样 + `--no-vsm` 回退：`SampleVirtualShadow()` 按「能覆盖且最细」选级、查页表 entry、3 x 3 tap 返回 `lit / 9.0`，`vsmParams.x < 0.5` 或未覆盖返回 -1 回退 CSM；`GlobalUbo` 尾部加 `vsmMatrices[3]` / `vsmLevels[3]` / `vsmParams`（`sizeof == 736`，四条 `static_assert`），set 0 新增 binding 20（图集）/ 21（页表）。顺手修三层：构造期建描述符早于 `setVsmEnabled` 导致 binding 20 绑成 CSM 比较采样器（整幅无阴影）、回退路径把 `2D_ARRAY` 视图写进 sampler2D（`viewType-07752`）、`ReflectionPass` 自建 global set 漏写 20/21（`None-08114`）。图集内容、覆盖 96.2%、VSM 与 CSM 分类 1,619,707 px 全等；七组 + 全量回归 97 项 + DocCheck 全绿（决策 84）

**待决（动 P4-2 之前要定）**：
1. 页表放哪个描述符集：`lighting.frag` 的 set 0 binding 11 现在是 `sampler2DArray shadowMaps`，而页表要「SSBO + 图集采样器」两个绑定 —— 加进 set 0 会改全局布局（`DescriptorManager::createGlobalLayout()` 与反射头），加 set 1 会碰 bindless 材质数组。**P4-2 已把这边的形状定死**：页表是每帧槽 12 KiB 的 `STORAGE_BUFFER`（`VirtualShadowMap::getPageTableBuffers()`），图集是单张 8192²（depth D32 + hzb RG16F），所以两种方案要加的绑定数与类型都已经确定，剩下的是选集。
2. 每页一次 draw 的提交方式：**已经量过（P4-3 动手前，`build\p4step3\flood-*.log` / `floodvp-*.log`）**。
   在 cascade 0 里灌 N 条空 `vkCmdDraw`，并另灌一组「每条前面加一次 `vkCmdSetViewport` + `vkCmdSetScissor`」
   模拟逐页提交：N=8000 时 cascade0 从 0.019 ms 涨到 0.029 ms（+10 µs），N=3000 带 viewport/scissor 时
   0.019 → 0.026 ms（+7 µs，约 2.3 ns/条），30 帧墙钟差异全在噪声里。
   **结论：早前按「P4-5 的 75 ns/draw」外推的 0.2~0.3 ms 是错的** —— 那个差值是逐模型路径换顶点缓冲 /
   push / 描述符的代价，不是命令本身的代价。逐页提交（3072 条）在这个 GPU 上是微秒级，所以 P4-4 可以
   直接「每页一次 viewport+scissor+draw」，不必先做多级间接 draw 的命令压缩。
   **P4-4b 带上真实几何再量了一次，结论反转了一半**：命令本身确实便宜，但逐页提交会把同一个子网格
   重复提交到它覆盖的每一页 —— 飞行场景每帧 225,982 / 27,310 / 20,238 draws，改成「整级一条」
   （`level.lightViewProjection` + 4096² viewport）后降到 309 / 184 / 93。所以真正的成本在
   「同一份几何被重复发」，不在命令流。
3. `--no-shadow-batch` 与将来的 `--no-vsm` 要不要合并成一个「阴影走 CSM/走 VSM」的开关：前者只能退回 CPU 侧的实体，`setExternalInstances` 灌进来的外部实例（`--cube-field`、粒子、GPU 场景外部实例）只有合并路径画得到，所以它不是等价开关。

**回退开关**：`--no-vsm`（P4-6 落地时加；CSM 路径保留到验收后 3 个版本）。

**遗留**：
- P4 第零步的 Debug 首帧 `max=5.867ms`（P4-5 复测是 8.012ms）是冷启动样本（首帧建资源），不进平均值；要稳态比较看 `min` 与每 60 帧的行。
- P4-5 修掉的老 bug：逐模型阴影路径漏乘 glTF 节点矩阵（`Model::getNodeWorldMatrix(subMesh.node)`）。任何新写的「逐子网格提交」路径都要问一句「这个 `mat4` 是实体空间还是节点空间」—— 漏了不会报错，只会静默画出一张位置和尺度都错的地图。
- 默认 Sponza 场景的相机在 `(-182.5, 6.69, 52.63)`、模型世界中心在 `(-0.48, 5.21, -0.31)`，相距约 190 单位、远超 90 m 阴影距离 → 该场景的级联阴影本来就是空的。要么改场景默认相机，要么按场景尺度缩放 `kMaxShadowDistance`，这事与 P4 无关但会持续污染所有阴影相关 A/B。
- `prepareView` 只在 `viewIndex == 0` 时上传实例表/组表，级联视图吃的是本帧上传之前的表（上一帧的结构）。结构变化的那一帧阴影可能用旧表，下一帧自愈；要消掉这个窗口得让首个级联也参与上传判定。
- 销毁顺序：`Application::run()` 在 `src/Core/Application.cpp:1450` 就 `vulkanDevice.reset()`（连带 `vmaDestroyAllocator`），而成员析构在它之后 —— 任何自己持有 VMA 资源的新成员都必须在 `vulkanDevice.reset()` 之前显式销毁，否则带资源退出时以 `0xC0000005` 收场、日志却是干净的（P4-2 的 `virtualShadowMap` 就撞了一次）。
- 页标记（P4-4）与页绘制（P4-4b）都只认 `MeshComponent` 里的投影者，外部实例（`--cube-field`、粒子、`setExternalInstances`）标不到也画不到 —— 实测 `--cube-field 4000 --cube-field-motion 200` 是 `draws=0`。要补得从 `SceneGpuBatch` 的实例表取包围盒（P4-4c），否则 VSM 上那些实例没有阴影而 CSM 有，A/B 会显示成「VSM 更省但不是等价画面」。
- 三级 clipmap 覆盖 76.8 米，比 CSM 的 90 米阴影距离短 13.2 米。P4-6 换掉 CSM 之前必须定远场归属
  （CSM 兜底 / 加第 4 级 / 按距离淡出），否则远处会出现一圈没有阴影。
- `main.cpp` 的参数解析是一条巨大的 `else if (arg == …)` 链，已经顶到 MSVC 的块嵌套上限：再加一条就 `main.cpp(531): fatal error C1061: 编译器限制: 块嵌套太深`。新增「只在启动阶段用一次」的开关要写成链前的早退块（`if (arg == …) { …; continue; }`），不要再往链尾塞。
