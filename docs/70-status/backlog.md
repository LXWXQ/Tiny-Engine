---
id: status/backlog
title: 后续任务台账
type: status
scope: 空
read-when: 准备开始下一项地形/规模化工作，或需要确认剩余任务顺序
size: ~120 行
updated: 2026-11-02
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

## B-00 已完成验收（D-34）

- `terrain_height.comp` 输出 normalized heightfield；
- GPU 与 CPU 参考在 33×33、含 ridge/detail/island/edge falloff 的配置上
  `maxDiff=2.98023e-07`；
- `--terrain-test` 的同步创建与异步重建都断言
  `HeightField::generatedOnGpu == true`；
- CPU mesh builder、主线程 Model 上传与后续 collision 高度场仍保持不变。

## B-01a 已完成验收（D-35）

- `TerrainStreamingComponent` manager + `TerrainChunkComponent` runtime tags；
- 每个 chunk 独立 `Model`（独立 TerrainComponent/MeshComponent/Bounds）；
- 相机 3×3 半径、最近优先、每帧最多 N 个加载、卸载滞回；
- `sampleOriginX/Z` 让独立 chunk 共享连续噪声域；接缝高度差 < 1e-4；
- manager 序列化模板，chunk 实体不逐项写入；
- `--terrain-test` 与编辑器自测验证 9 chunks / 9 Models / 卸载重载 / round-trip。

## B-01b 已完成验收（D-36）

- `TerrainComputeGenerator` 改为 8 个独立 in-flight slot；submit/poll/release/waitAll；
- `PendingTerrainBuild` 三阶段：GPU height pending → CPU mesh job → main-thread upload；
- 流送与手动 Generate 共用异步 GPU 高度场，主线程不再 fence 等待；
- 废弃/覆盖请求标记 abandoned，槽位在 fence 完成后回收；
- 编辑器捕获 `terrainWorld` 描述后实际 `buildFromDescription()`，重新流送 9 chunks；
- 自检断言创建 manager 后 GPU 请求已 in-flight 且本帧尚无 generated chunk。

## B-01c 已完成验收（D-37）

- Viewport 拾取命中 runtime chunk 后解析到 manager；Hierarchy 隐藏 chunk 实体；
- Inspector 显示 manager 的半径、预算、mapped chunks、in-flight GPU 请求与 update ms；
- `DeleteEntityCommand` 支持 streaming manager：删除级联 chunk，undo 重新 adopt，redo 正确删新 ID；
- `terrainWorld` 走真实 JSON 文件 save/load，再 `buildFromDescription()` 重建 9 chunks；
- 编辑器记录跨 4 chunk 移动时 `maxUpdateMs=0.6123`（Debug/本机样例）。

## B-02 已完成验收（D-38）

- 每个 chunk/LOD 的 chunk 外边界固定为全高度场分辨率；内部仍按 1/2/4 cell
  步长降采样。
- 四条过渡环带把全分辨率外环与粗网格内环按弧长参数三角化；四角共享索引，
  无 T-junction、无洞、无重复覆盖。
- `skirtsEnabled` 变为几何参数并可序列化；`--terrain-test` 和流送 smoke
  使用 `skirts=false` 运行，默认用户地形保留 true 作为保守回退。
- `TerrainSelfTest` 断言 skirts=off 时 `surfaceIndexCount==indexCount`、
  +Y 绕序、LOD 索引递减，以及 3×3 LOD 组合的 18 条邻接边边界位置/法线/UV/色
  一致。
- 无裙边截图：`.tmp/terrain_b02_noskirt.png`（1920×1061，验证层 0 错误）。

## B-03 已完成验收（D-39）

- 新增无附件 `TerrainGpuSelection` pre-graphics pass，仍在 graphics command
  buffer 上、dynamic rendering 之外执行。
- 每帧上传最多 1024 条 chunk record；compute 完成：
  - world AABB + frustum cull；
  - LOD 距离、15% 滞回、0.24s transition 状态；
  - 每 terrain entity 最多 32 条 `VkDrawIndexedIndirectCommand`；
  - `vkCmdDrawIndexedIndirectCount` 直接消费 GPU 写的 count。
- 每 frame slot 独立 LOD state / count / stats / debug buffer；稳定状态不读回 CPU。
- 过渡条目的 blend/mode 编码进 `firstInstance`，顶点输出 flat varying，片元
  dither 不再依赖逐 draw push constant。
- `--terrain-test` 强制 `lodDistanceScale=0.01 → 1.0`，同时验证稳定帧和过渡帧：
  - `[Terrain] GPU selection selfTest PASS | matches=3 records=4`
  - `[Terrain] GPU selection transition PASS | records=4`
- 无 `drawIndirectCount` 时保留 CPU command-building fallback；GPU 路径容量
  64 entities / 1024 chunk records / 每 entity 32 commands。
- 过渡期截图：`.tmp/terrain_b03_blend.png`（LOD blends=2，draw calls=5）。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**；
  日志 `build/validation/20261101-203943`。

## B-04 已完成验收（D-40）

- 新增 `TerrainGpuMeshBuilder` 与 `shaders/terrain_mesh.comp`：
  - 单 chunk（`chunkGridX/Z == 1`）时 compute 直接组装全部 LOD；
  - 顶点按 44-byte `Vertex` 的 11 个 tight float 写入，避开 `std430 vec3`
    对齐差异；
  - 索引写入独立 SSBO，覆盖 B-02 的粗内部、全分辨率外环、四条过渡带和可选裙边；
  - 法线、顶点色、UV、局部/绝对色域与 CPU `TerrainMeshBuilder` 对齐。
- 输出 buffer 同时带 `STORAGE_BUFFER | VERTEX_BUFFER/INDEX_BUFFER | TRANSFER_SRC`，
  `Model::createFromGpuMesh()` 直接接管 VMA allocation，不再 staging copy。
- `Scene` 同步生成与异步流送都接入 GPU mesh：
  - 异步路径在 GPU heightfield readback 后、主线程组装并安装；
  - `TerrainComponent::meshGeneratedOnGpu` 与 `generationBackend` 记录来源；
  - 多 chunk/无 compute 设备保留 CPU builder + staging fallback。
- CPU 侧只保留全分辨率 picking mirror（位置 + 索引）和 submesh 元数据；
  collision 继续使用 `HeightField`，渲染不再消费 CPU 顶点/索引。
- `--terrain-test` 增加 GPU/CPU 网格 parity：
  - `[Terrain] GPU mesh assembly selfTest PASS | vertices=6691 indices=37824
    lods=3 skirts=on localColor=on maxPosError=0 maxAttrError=8.27056e-07`
  - 9 个流送 chunk 全部 `gpuMesh=9`，编辑器 picking/seam/unload/rebuild 通过。
- 顺带修复 B-03 隐藏的 LOD state 索引 bug：shader 之前把 chunk-slot
  `stateIndex` 当 scalar word offset，实际每槽有 4 个 int；现在统一用
  `stateIndex * 4`，并在 slot 复用时清理所有 frame slot + 增加
  `COMPUTE -> COMPUTE` state barrier。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`；DocCheck
  `documents=55 lines=7682`。
- 增强同步校验：`--validation-features sync` 的 terrain smoke 也保持
  `errors=0 warnings=0`。
- 日志：`build/validation/20261101-213544`。
- B-05 已在此基础上改为 4 槽异步提交/回收（见下）。

## B-05 已完成验收（D-41）

- `TerrainGpuMeshBuilder` 改为 4 个异步 slot：`submit()` 只创建输出 buffer、
  写 host-visible height/params、录制并提交 compute，不在主线程等待 fence；
  `poll()` 用 `vkGetFenceStatus` 观察完成，`release()/collectFinished()/waitAll()`
  负责取消、回收和关闭等待。单次提交不超过约 0.5ms（Debug/本机）。
- `prepareBuild()` 不再同步生成全分辨率 picking mirror；新增
  `buildPickingMirror()`，由 `std::async` 工作线程生成 positions/indices，
  与 GPU assembly 并行。`poll()` 只在工作线程完成后取结果，未完成时返回
  `Pending`，主线程不等待 future。
- Scene 三阶段流水线新增 `PendingTerrainBuild::gpuMeshRequest`：Stage 2a
  poll、2b 有空闲 slot 才 submit、2c 无 GPU/提交失败才 CPU fallback；
  slot 满时任务保持 pending，不再回退到主线程同步构建。替换、取消、销毁
  路径 release 请求，Scene 关闭时 `waitAll()`。
- `Scene` 暴露 `getMaxTerrainPollMs()`、`getTerrainPollCount()`、
  `resetTerrainPollStats()`、`getTerrainGpuMeshStats()`；`Scene::update()` 与
  `waitForTerrainGeneration()` 都记录 poll 时长。
- `--terrain-test` 新增 257×257 单 chunk B-05 回归：同步创建作为 baseline，
  随后异步重建；断言 GPU mesh、revision、callback、submit 计数，且
  `maxPoll < syncBaseline`。
- 实测：`[Terrain] async GPU upload PASS | resolution=257x257 chunks=1
  syncBaseline=8.3553ms asyncMaxPoll=0.9878ms maxSubmit=0.3255ms
  generation=7.6449ms spikeRatio=8.45849 revision=2 gpuMesh=1`
  （Debug/本机样例，数值随机器波动）。
- 流送 9 chunk 仍全部 GPU mesh：`chunks=9 independentModels=9 gpuMesh=9`；
  GPU mesh selfTest 保持 `vertices=6691 indices=37824 ...
  maxAttrError=8.27056e-07`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`；日志
  `build/validation/20261101-222753`。
- 增强：`--validation-features sync` 的 terrain smoke 保持
  `errors=0 warnings=0`（本项不新增同步特性）。

## B-06 已完成验收（D-42）

- `TransformComponent` / `BoundsComponent` 字段全部私有；外部只通过 getter
  与 setter 访问，编译器阻止 public 字段直写。新增 `setPose()` 一次恢复
  平移/欧拉/缩放/四元数并只标脏一次，供命令 undo/redo 保留完整快照。
- `SpatialIndex` 在统一 3D grid 之外增加粗粒度 XZ region bucket：
  `setRegionSize()`、`queryRegion()`、`queryRegionRange()`、
  `regionCellCount()`；AABB 覆盖超过 1024 个 region 的对象进入 oversized
  列表，range 超过 4096 个 region 时退化为全量候选。
- `Scene` 将 region size 对齐 streaming chunk size，新增
  `queryTerrainChunksInRegion()` / `queryTerrainChunksInRegionRange()`，
  只返回当前 streaming manager 的有效 chunk，不再扫描全部 chunk map。
- 统一销毁：`clearEntities()` 与 `buildFromDescription()` 相机复用都改走
  `Scene::destroyEntity()`；后者统一清理 Model、SpatialIndex、dirty 集合与
  流送映射。独立命令栈 selfTest 的局部 registry 是带注释的唯一例外。
- `--terrain-test` 新增 5×5 region 查询断言：chunks=9，且全部属于当前
  manager；`Scene::selfTest` 覆盖 region 重建、单 region/range 查询和
  remove 后 bucket 清理。
- 实测：`[Terrain] spatial region query PASS | chunks=9 regions=5x5`；
  `[Scene] ECS/spatial selfTest PASS | hierarchy=T*Ry*Rx*Rz*S | grid cells=8`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`；日志
  `build/validation/20261101-225723`。

## B-07 已完成验收（D-43）

- 新增 `terrain::TerrainBrush`：Raise/Lower/Smooth/Flatten 四种模式、smoothstep
  衰减、世界坐标中心、`HeightField` 归一化高度增量、min/max 刷新。
- `Scene::sculptTerrain()` 先用 B-06 region 查询找出与笔刷相交的 streamed chunk，
  再按世界 AABB 补手动地形；每个受影响 `HeightField` 先复制并重建 Model，
  全部成功后才替换 `MeshComponent`/`TerrainComponent` 并退休旧 Model。
  `--terrain-test` 断言 9 个 chunk 中仅 1 个重建、其余 revision/Model 不变。
- 新增 `CharacterControllerComponent` + `createCharacterEntity()`/
  `updateCharacterController()`：O(地形实体数) 局部包围盒高度采样、重力、跳跃、
  step height、坡度限制、ground snap 与 grounded 状态；不依赖物理库。
- 新增 `Scene::sampleTerrainGroundHeightWorld()`、`queryGroundAtWorld()` 和
  `raycastTerrainWorld()`；编辑器笔刷使用最近地形三角形命中，不再每帧遍历
  全部 LOD0 三角形做角色贴地。
- 编辑器：Inspector Terrain/Streaming 分组新增 Sculpt Brush（Ctrl+LMB），
  viewport 命中后调用 `sculptTerrain()`；Create 菜单新增 Character，
  角色组件可序列化；Play 模式 WASD + Space 驱动选中角色，编辑相机输入暂停。
- 实测：`[Terrain] sculpt PASS | chunks=9 rebuilt=1 vertices=21 radius=2.5 mode=Raise`；
  `[Terrain] character controller PASS | grounded=1 maxGroundError=0 jumpHeight=0.95 walkDistance=5.99991`；
  `[Terrain] character serialization PASS | captured=1`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`；日志
  `build/validation/20261101-234432`。
- 已知边界：雕刻只改运行期 `HeightField`/Model，不写入场景 JSON；保存重载或
  Play 快照恢复会回到程序化高度，持久化留到后续地形编辑需求。

## B-08 已完成验收（D-44）

- `TerrainMaterial` 改为有界材质集库：最多 4 个 Set2 set，每个 set 12 张固定
  `sampler2D`；set 0 启动时从根目录加载，set 1~3 按需从 `setN/` 加载。
- 每个 set 要求 16 张 PNG 齐全（4 层 albedo/normal/height/roughness）；缺图
  整套回退 256² 程序化纹理。`height.r + roughness.g` 在 CPU 侧打包，GPU 仍只
  持有 12 张纹理。
- `DescriptorManager` 分配 4 个 terrain descriptor set；`TerrainConfig` 新增
  `materialSetIndex`（0..3）并随场景 JSON 保存；仅切换材质不触发网格重建。
- `GeometrySystem::render` 在相邻地形实体材质集变化时重绑 Set2，并统计
  `terrainMaterialBatches` / `terrainMaterialSwitches` /
  `terrainMaterialFallbacks`；非法索引回退 set 0。
- Inspector Terrain/Streaming 分组新增 Material Set 下拉；切换走
  `SetTerrainConfigCommand`，支持撤销/重做与序列化。
- 自检临时生成真实 16 PNG 外部 set，验证外部加载、程序化回退、4 集上限与
  `materialSetIndex` 捕获；实测：
  `[Terrain] material sets PASS | resident=4 max=4 external=1 fallback=3 serialized=1`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，stderr 全 0，
  `[Validation] errors=0 warnings=0`；日志 `build/validation/20261102-001837`。
- 已知边界：仓库不附带外部美术资产；材质集切换未按材质排序/合批，交替材质会
  增加 descriptor switch；地形雕刻持久化仍是独立后续项。

## 归档时的剩余队列（2026-11-04）

体素四步（持久化 / 运行模式接管角色 / 贪心合并 + 贴图集 + AO / 异步生成 + 半径 12）已全部落地，
下面按“什么时候必须做”排序，供后续接续。

| ID | 状态 | 任务 | 依赖 | 完成判据 |
|---|---|---|---|---|
| V-01 | 已完成 | 体素透明方块排序（玻璃拆独立网格 + 前向混合 pass，按面从远到近） | F3 | 透明方块叠放不出现“后面先画”的黑边；截图与手工目测一致 |
| V-02 | 未开始 | 体素网格化的每帧预算与取消（当前每块一个 job，`processVoxelChunkBuilds()` 每帧最多上传 4 块，但提交阶段没有总预算） | V-04、JobSystem | 半径 12 首帧提交仍有 7.9 ms（Release），要压到 <2 ms；取消后不再上传 |
| V-03 | 未开始 | 体素 LOD（远距离 chunk 粗网格） | V-02 | 半径 24 的 triangles 显著下降，近处不糊 |
| V-04 | 未开始 | 体素几何走 `TerrainMeshArena` / 间接合批（当前每 chunk 一份 `Model` + 一次 bind） | F1 合批路径 | 625 块的 draw calls 从数百降到个位数 |
| V-05 | 未开始 | 体素世界分页/无限世界（当前半径上限 12，`Scene::kMaxVoxelRadius`） | V-04 | 半径 24 显存与实体数可控 |
| F6-01 | 未开始 | SSR（屏幕空间反射，读 HDR + depth） | F6 | 反射可见、与 IBL 结果有明显差异且无严重噪点 |
| F6-02 | 未开始 | 自动曝光（compute 直方图 + 曝光纹理） | F6 | 室内外切换后 2 秒内亮度收敛 |
| F6-03 | 未开始 | Compute Bloom（金字塔迁到 compute） | F6 | 输出与当前 bloom 视觉一致、GPU 时间下降 |
| F6-04 | 未开始 | 切线空间法线贴图（启用 `getNormalFromMap()`，先补切线或 TBN） | F4 | 法线贴图生效；`normalMaps` 不再白绑（tech-debt #3） |
| F4-01 | 未开始 | glTF 保留 node 层级（决策 13 的翻转）、primitive 独立变换、异步加载 | — | 载入后可改任意 node 局部变换并实时可见 |
| F4-02 | 未开始 | 模型 LOD 链（GPU 端 LOD 选级的前置，见 F1 剩余项） | F4-01 | 同一模型可切 2~3 级，切换无跳变 |
| F1-R1 | 未开始 | 粒子真实接入（`ExternalInstance` 入口已就位，还没有生产方） | F6 | 粒子与 ECS 实例共用分组/合批/剔除 |
| E-P1 | 未开始 | 启动失败的原生 MessageBox（UX-04 仍缺） | — | 双击启动失败时用户能看到可读提示 |

> V-01 已完成（2026-11-04）：玻璃拆独立网格 + `GlassPass`（Lighting 后、Bloom 前，按面质心远→近混合），glass-only chunk 免 Model；`--voxel-glass-demo` 与 `f3-voxel-glass` 覆盖。实现细节见 [milestones](milestones.md#v-01体素玻璃透明排序glasspass)，验证日志 `build/validation/20261104-162505`。

## 更新方式

完成任务时：

1. 本页把状态改为 `已完成`，补一行里程碑/验证日志路径；
2. 在 [milestones](milestones.md) 追加 D-xx 记录；
3. 同步 `known-limits`、`tech-debt`、`terrain-plan` 中仍然成立/已撤销的条目；
4. 运行 `tools\Validate.ps1 -Incremental -SkipSetup` 与 `tools\DocCheck.ps1`。
