---
id: status/backlog
title: 后续任务台账
type: status
scope: 空
read-when: 准备开始下一项地形/规模化工作，或需要确认剩余任务顺序
size: ~464 行
updated: 2026-11-26
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

## 归档时的剩余队列（2026-11-05）

体素四步（持久化 / 运行模式接管角色 / 贪心合并 + 贴图集 + AO / 异步生成 + 半径 12）已全部落地，
下面按“什么时候必须做”排序，供后续接续。2026-11-05 调整优先级：先完成 E-P1；其余未开始项
继续保留在此表，等以后再做，不另开临时清单。

| ID | 状态 | 任务 | 依赖 | 完成判据 |
|---|---|---|---|---|
| V-01 | 已完成 | 体素透明方块排序（玻璃拆独立网格 + 前向混合 pass，按面从远到近） | F3 | 透明方块叠放不出现“后面先画”的黑边；截图与手工目测一致 |
| R-01 | 已完成 | TAA 时域抗锯齿（Halton 抖动 + 未抖动 VP 历史重投影 + 3x3 AABB clamp） | — | 远处几何边缘与纹理闪烁明显下降；`--no-taa` 可关；回归项 `f3v-taa`/`f3v-taa-off` 通过 |
| R-02 | 已完成 | 体素图集 mip 跨格渗色（远处规则白点/摩尔纹） | — | 远处草地/石面规则白点消失，近景纹理与玻璃图集正常；`f3-voxel` 断言 `atlas=512 tile=64 pitch=128 gutter=32` |
| R-03 | 已完成 | 流送地形 chunk 边界材质断层（高度层权重改用全局 `baseHeight`/`heightScale`） | — | 合批与非合批截图均无矩形沙/雪色块；`f2b-terrain-batch`/`f2b-terrain-batch-off` 通过 |
| R-04 | 已完成 | 场景树父子结构（体素 chunk 归入 Voxel World 子树，按坐标稳定排序并默认展开） | — | Hierarchy 中 chunk 缩进在世界节点下、树线正常；`e8-editor-mode` 断言 `parented=9` |
| R-05 | 已完成 | 运行模式角色接入（真实模型 + 视线出生 + WASD/Shift 跑 + 6 段骨骼动画状态机） | B-07、R-01 | 示例体素世界可见角色、不坠出世界、无黑色/棋盘格；`e9-runtime-character` 断言 `skinned=1 animations=18` 与 6 个 clip；`--runtime-mode` 可直达 |
| R-06 | 已完成 | 水面规则黑白格（删除 3 个高频正弦 ripple，改 1024x1024 无缝 FFT 法线贴图两层滚动，18m/42m） | — | 黑白菱形格消失；近景波纹连续、无平铺接缝；`f3v-water` 断言水波法线加载，75 项 ALL PASS（日志 `build/validation/20261106-113028`） |
| R-08 | 已完成 | 水面发暗/发绿观感（平面反射暗部最多补 34% 天空环境底色；深水色提亮约 2 倍；远水 alpha 0.92→0.89） | — | 低机位同机位暗水区域 luma 76.2→81.8、蓝色 94.2→99.6，岛影仍可见；diff>2 共 840553 像素；75 项 ALL PASS（日志 `build/validation/20261106-164734`） |
| R-09 | 已完成 | 低渲染倍率水面平铺亮暗格（18m 法线低频；改为高通 K>=32 法线贴图） | — | 0.37x 同机位网格消失、全 1920 正常；对比 `build/water_grid_fix_compare.png`；75 项 ALL PASS（日志 `build/validation/20261106-173309`） |
| R-10 | 已完成 | 高角度水面泡沫规则菱格（波峰项铺满水面 + 双正弦乘积；改为岸边门控 + 法线贴图低频噪声） | — | 高角度同机位泡沫菱格消失、水面只剩连续波纹/云影；低角度水面无规则格、泡沫不再铺满水面；75 项 ALL PASS（日志 `build/validation/20261106-180307`） |
| R-11 | 已完成 | 人物 SSAO 黑色花瓣（G-Buffer Position 从 RGBA16F 改为 RGBA32F，消除远坐标量化误差） | — | 用户原机位、原 SSAO 参数下角色干净；关 SSAO / 原点对照 / 32F 对照三组截图一致；79 项 ALL PASS（日志 `build/validation/20261111-103956`） |
| V-02 | 未开始 | 体素网格化的每帧预算与取消（当前每块一个 job，`processVoxelChunkBuilds()` 每帧最多上传 4 块，但提交阶段没有总预算） | V-04、JobSystem | 半径 12 首帧提交仍有 7.9 ms（Release），要压到 <2 ms；取消后不再上传 |
| V-03 | 未开始 | 体素 LOD（远距离 chunk 粗网格） | V-02 | 半径 24 的 triangles 显著下降，近处不糊 |
| V-04 | 已完成 | 体素几何走 `TerrainMeshArena` / 间接合批（每 chunk 一份 `Model` + 一次 bind -> 共享 arena + 专用实例表） | F1 合批路径 | 625 块整批只占 1 条 `vkCmdDrawIndexedIndirect`；半径 12 的 arena 725888 顶点 / 1088832 索引不溢出，验证 0/0（日志 `build/validation/20261106-102034`） |
| V-05 | 未开始 | 体素世界分页/无限世界（当前半径上限 12，`Scene::kMaxVoxelRadius`） | V-04 | 半径 24 显存与实体数可控 |
| F6-01 | 已完成 | 水面近场 SSR（半分辨率 48 步屏幕空间追踪，读 HDR + G-Buffer 世界位置，与平面反射/IBL 混合） | F6 | 反射可见、与 IBL/平面反射结果有明显差异且无严重噪点；关平面反射+关波浪时 SSR 开/关差异 91918 像素；`f3v-ssr`/`f3v-ssr-off` 通过（日志 `build/validation/20261106-105750`） |
| F6-02 | 已完成 | 自动曝光（compute 统计平均对数亮度 + CPU 按帧适应，默认关） | F6 | 已完成，见 [决策 42](../../docs/10-architecture/decisions.md) 与提交 `f47db26` |
| F6-03 | 未开始 | Compute Bloom（金字塔迁到 compute） | F6 | 输出与当前 bloom 视觉一致、GPU 时间下降 |
| F6-04 | 未开始 | 切线空间法线贴图（启用 `getNormalFromMap()`，先补切线或 TBN） | F4 | 法线贴图生效；`normalMaps` 不再白绑（tech-debt #3） |
| F6-05 | 已完成 | 体积云（场景级 `scene.clouds` 组件 + 全屏 raymarch `CloudPass`） | F6 | coverage/密度/高度/厚度可调；透明表面读到带云 HDR；`f6-clouds` 通过；云不进平面反射/阴影作为边界 |
| F6-06 | 已完成 | 太阳盘 + 镜头光晕（场景级 `scene.sun` 组件 + 全屏后效 `SunFlarePass`） | F6、F5-06 | 8 参数可调；方向/光色跟随 `scene.environment`；5 点深度判定日盘遮挡；圆形散景鬼影/色散/光条（太阳在背后/出画不产生）；TAA 后 Bloom 前纯加色；`f6-sun` 通过；默认关闭，云遮挡为边界 |
| F6-07 | 已完成 | 地形植被（场景级 `scene.vegetation` 组件 + 程序化草/树 + `VegetationPass`） | F5-06、F3 | 12 参数可调；6 卡片/每张 4 叶草、针叶 6 层、阔叶 7 团簇；Geometry 后 SSAO 前写 G-Buffer；草/针叶/阔叶 3 draw；树 shadow pipeline 投 CSM、草不投并用接触圆盘兜底；地形采样缓存/32m 索引/0.15s 稳定 + 0.5s 强制刷新；`f6-vegetation` 通过；默认关闭 |
| F6-08 | 已完成 | 局部体积雾（实体级 `effects.fogVolume` 组件 + 盒形 raymarch `FogVolumePass`） | F5-06、F6-05 | 14 参数可调；`+ Add Component` → “体积效果”可发现；组件 `center`/`size`/`rotation` 定义雾盒、最多 8 个；Lighting/Cloud 后、反射/透明前预乘 alpha 写 HDR；读 G-Buffer 位置截断、32³ 噪声、高度衰减/边缘淡出/HG 相函数；`f6-fog-volume` 通过；默认不添加 |
| F6-09 | 已完成 | 植被视觉/性能优化（2026-11-13） | F6-07 | 草 6 卡片/4 叶，针叶 6 层、阔叶 7 团簇；树干风权重归零、树根 4 邻点取最低+下沉、接触圆盘；生成 1416ms→约 9ms；`f6-vegetation` 与 60/180 帧回归 0/0 |
| F6-10 | 已完成 | 阴影 PCF/PCSS 可选（2026-11-13） | F6-a | `scene.shadows.filterMode` 0=5x5 PCF / 1=blocker-search PCSS；UI Combo、engine.json、GlobalUbo、GLSL 全链路；旧配置缺省 PCF；selftest 60/180 validation 0/0 |
| F4-01 | 未开始 | glTF 保留 node 层级（决策 13 的翻转）、primitive 独立变换、异步加载 | — | 载入后可改任意 node 局部变换并实时可见 |
| F4-02 | 未开始 | 模型 LOD 链（GPU 端 LOD 选级的前置，见 F1 剩余项） | F4-01 | 同一模型可切 2~3 级，切换无跳变 |
| F1-R1 | 已取代 | 粒子不接入 `ExternalInstance`：改由 F5-07 的独立组件与 `ParticlePass` 落地 | F5-07 | 不再以复用 ExternalInstance 为目标；粒子独立缓冲、独立前向 Pass |
| E-P1 | 已完成 | 启动失败的原生 MessageBox（UX-04） | — | 双击启动失败时用户能看到可读提示 |
| F5-01 | 已完成 | 元数据驱动 ECS：组件注册表 + SystemScheduler + Inspector Add Component + 序列化 v3 | — | 实体可加/删/改 Spin/Orbit，Inspector 编辑并撤销；场景存读保留 components；`selftest-default` 断言 ECS component selfTest |
| F5-02 | 已完成 | 预制体资产：保存选中子树为 .prefab.json、实例化、撤销/重做 | F5-01 | 含 effects.spin 的 cube 父子树可保存/读取/实例化；undo 消失、redo 换新 ID 恢复；e1 断言 prefab selfTest |
| F5-03 | 已完成 | 内置效果/运动组件第一批：Bob、Pulse、Sway、LookAt、Follow | F5-01 | `+ Add Component` 列出 7 个 `effects.*`；场景存读/预制体/撤销自动生效；`selftest-default` 断言 `components=7 systems=7` |
| F5-04 | 已完成 | 原生组件接入统一 Add Component：Character/Render Tag 可增删，Managed 组件置灰说明；v3 序列化去重 | F5-01 | 16 个描述符（7 effects + 9 native）；Character/Render Tag 经 Inspector 增删改并撤销；原生组件不写入 v3 `components`；`components=16 systems=7` |
| F5-05 | 已完成 | 组件编辑工作流：属性范围/提示/单属性重置、组件复制/粘贴/重置、Add Component 置顶 + 搜索 | F5-04 | 7 effects 与 Character 属性有范围/提示；组件可跨实体复制粘贴并撤销；Add 弹窗可搜索；`selftest-default` 断言属性元数据且全套回归通过 |
| F5-06 | 已完成 | 场景级组件系统：阴影/Bloom/SSAO/SSR/运动模糊/TAA/水面/雾/环境作为 scope=Scene 组件，可添加/启用/调参/移除，v4 序列化 | F5-05 | Inspector 无选中可管理；旧场景兼容；场景存读保留；自检断言 25 描述符与 sync |
| F5-07 | 已完成 | 粒子组件：实体级 CPU 发射器（16 属性）+ 独立前向透明 `ParticlePass` | F5-06 | `--particle-demo` 空场景可发射；Water 后/TAA 前按距离排序混合；26 描述符/8 系统；`f6-particles` 与 240 帧 `alive=376` 验证层 0/0 |
| F5-08 | 已完成 | 粒子 sprite 资源与贴图选择：Kenney CC0 14 张转 RGBA，`effects.particles.texture`（第 17 属性）+ Inspector 选择器，`ParticlePass` 启动 bindless 预加载 | F5-07 | Inspector 可选/清除贴图并撤销；启动日志 `textures=14 failed=0 status=ready`；`f6-particles` 断言 `texture=Particles/Kenney/fire_01.png`；空/失效回退圆形；新增贴图需重启 |
| F5-09 | 已完成 | 组件目录完整分类与可发现性：Add 弹窗列出全部 exposed 组件（含已挂载打勾）、中文分类、地形/体素保留入口并可新建空实体；无选中时 Scene Components 旁一键「创建体积雾对象」 | F5-05、F5-06 | 场景/实体 scope 弹窗不再为空；体积雾在“体积效果”下可发现，无选中时也能一键创建；截图 `build/editor_component_catalog.png` / `editor_scene_catalog.png` / `editor_fog_entry.png`；编辑器自检 0/0 |

> V-01 已完成（2026-11-04）：玻璃拆独立网格 + `GlassPass`（Lighting 后、Bloom 前，按面质心远→近混合），glass-only chunk 免 Model；`--voxel-glass-demo` 与 `f3-voxel-glass` 覆盖。实现细节见 [milestones](milestones.md#v-01体素玻璃透明排序glasspass)，验证日志 `build/validation/20261104-162505`。
>
> 同日修复示例体素世界全黑：异步 chunk 落地后未登记 `dirtySpatialEntities`，建不出 `WorldBoundsComponent` 被全部剔除；`clearEntities()/cleanup()` 同时补上 pending 作业取消标记，加载示例等待网格落地。详见 [milestones](milestones.md#示例体素世界黑屏修复--运行模式-wasd-方向修正2026-11-04)。
>
> TAA 已完成（2026-11-05）：插在 `GlassPass` 后、`BloomPass` 前；读 `hdrScene` + G-Buffer.Position + 上一帧历史，写 `taaOutput` 与 ping-pong 历史；关闭时权重/抖动归零但 Pass 仍执行。实现与验证细节见 [milestones](milestones.md#taa-时域抗锯齿2026-11-05)，回归日志 `build/validation/20261105-094857`。
>
> 同日修复体素图集格边界白线/远处闪烁：`fract()` 在每格边界的导数尖峰把 mip 推到最粗级，平均到图集里 7 个纯白未用格，形成白线；改用 `textureGrad` 显式梯度 + 半纹素内缩 + 未用格填草绿。详见 [milestones](milestones.md#体素图集格边界白线--远处闪烁修复2026-11-04)。
>
> R-05 已完成（2026-11-05）：运行模式挂 Quaternius `Character.glb`（CC0，4 蒙皮网格 / 29 骨骼 / 18 动画），CPU 蒙皮 + 3 帧环形动态缓冲；出生点改为沿视线 ray-march 找地面，Shift 跑、Space 跳，状态机 Idle/Walk/Run/Jump/Jump_Idle/Jump_Land。期间修复纯色材质走棋盘格、动画名带 `CharacterArmature|` 前缀、蒙皮模型进静态合批、yaw 弧度二次转换四个问题。详见 [milestones](milestones.md#运行模式角色与骨骼动画2026-11-05)，回归项 `e9-runtime-character`。
>
> 2026-11-05 的后续：显式梯度后远处仍有规则白点/摩尔纹，根因是全局 mip 仍把相邻图集格平均；图集扩为 512×512、每格 64px 内容 + 32px edge-clamp gutter，`mrt.frag` 与 `glass.frag` 同步新映射。详见 [milestones](milestones.md#体素图集-mip-跨格渗色与摩尔纹修复2026-11-05)。
>
> 同日修复流送地形 chunk 边界的材质断层：`mrt.frag` 的高度层权重原先用每个 chunk 的局部 Y 范围归一化，同一世界高度在边界两侧得到不同 `h`；`GeometrySystemGpu` 与 `GeometrySystem` 统一改用 `TerrainConfig::baseHeight`/`heightScale`，合批与非合批截图都恢复连续。详见 [milestones](milestones.md#流送地形-chunk-边界材质断层修复2026-11-05)。
>
> 同日把场景树补成真正的父子结构：`createVoxelWorld()` 对每个 chunk 调 `setParent(entity, world)`，chunk 不再与 `Voxel World` 并列成根；世界节点默认展开、按 `chunkZ/chunkX` 排序，Hierarchy 使用 `DrawLinesFull` 树线。详见 [milestones](milestones.md#场景树体素-chunk-归入世界子树2026-11-05)，验证日志 `build/validation/20261105-125330`。
>
> E-P1 已完成（2026-11-05）：交互式启动失败先弹原生 `MessageBoxW`，正文含阶段、原因、`startup-error.log` 路径、`--help` 与 Vulkan 下一步；`--hidden` / `--selftest` / 截图等无人值守路径保持静默。详见 [milestones](milestones.md#e-p1启动失败原生-messagebox2026-11-05)。
>
> C1 半分辨率平面反射已完成（2026-11-05）：`ReflectionPass` 用镜像相机重画 960x540 的 G-Buffer 与延迟光照，`WaterPass` 从 Set0 binding12 采样；开关为 `water.planarReflection`，默认开启。
>
> F6-01 水面近场 SSR 已完成（2026-11-06）：`SSRPass` 读 HDR + G-Buffer 位置/法线，在半分辨率 960x540 上做 48 步屏幕空间 DDA，结果写 `ssrColor` 并从 Set0 binding13 进入 `water.frag`；命中置信度再乘粗糙度与距离衰减，与平面反射/IBL 混合。屏幕外/超过 `ssrMaxDistance`（默认 200m）回退。开关 `ssr.enabled` 与 `--no-ssr`，关平面反射+关波浪时开/关差异 91918 像素。详见 [milestones](milestones.md#f6-01水面近场-ssr2026-11-06)，验证日志 `build/validation/20261106-105750`。
>
> R-06 水面黑白格已完成（2026-11-06）：规则黑白菱形格来自 3 个高频正弦 ripple 的解析法线在 Fresnel/高光下的干涉，关 `waveStrength` 即消失；现改为 1024x1024 无缝 FFT 水波法线（`Resources/Texture/Water/water_normal.png`，CC0），片元按 18m/42m 两层滚动、`textureGrad` 足迹采样，Set0 binding14 常驻。截图 `build/Debug/water_normal_test.png` 与修复前 `build/Debug/water_check.png` 对比黑白格消失；`f3v-water` 新增 `[Tiny] Water normal map loaded` 断言。详见 [milestones](milestones.md#r-06水面法线贴图替换高频正弦波2026-11-06)，验证日志 `build/validation/20261106-113028`。
>
> V-04 已完成（2026-11-06）：体素不透明几何在 `applyVoxelChunkMesh()` 里先临时上传 `Model`，再 `TerrainMeshArena::acquire/copyInto` 并 `releaseGpuBuffers()`，`Model` 只留 CPU 几何；`GeometrySystem` 按可见集合写专用 224B 实例表与 `VkDrawIndexedIndirectCommand`，整批一条 indirect。半径 12 实测 `chunks=625 arena=625 vertices=725888 indices=1088832`，可见 535 块整批 `commands=535`，材质槽池 256→1024；`f3-voxel-arena-625` 覆盖。详见 [milestones](milestones.md#v-04体素-arena-间接合批2026-11-06)。
>
> F5-01 元数据驱动 ECS 已完成（2026-11-06）：`ComponentRegistry` 用模板描述符注册组件（Bool/Int/Float/Vec3/Color/String 六种属性），`SystemScheduler` 在 `updateWorldTransforms()` 前后跑 Simulation/PostTransform；内置 `effects.spin`、`effects.orbit`。Inspector 增加 + Add Component / 属性编辑 / 移除，统一走 `SetComponentRecordCommand`；`SceneSerializer` v3 把 components 作为可选数组，未知组件跳过。`selftest-default` 断言 `[Scene] ECS component selfTest PASS`，编辑器自检覆盖 capture → JSON → build → undo/redo。全套 75 项 ALL PASS（日志 `build/validation/20261106-120327`）。详见 [milestones](milestones.md#f5-01元数据驱动-ecs2026-11-06)。
>
> F5-02 预制体资产已完成（2026-11-06）：复用 v3 `SceneDescription` 作为 `.prefab.json`，`Scene::captureDescription(root)` 只捕获选中子树、`Scene::instantiatePrefab()` 追加实例化并在失败时逆序回滚；`editor::prefab` 负责校验/读写，`InstantiatePrefabCommand` 提供 undo/redo（redo 新 ID）。文件菜单“保存选中为预制体...”、创建菜单“实例化预制体...”。编辑器自检 `[Editor] prefab selfTest PASS`，`e1-batch-commands` 断言该行；全套 75 项 ALL PASS，验证日志 `build/validation/20261106-124323`。详见 [milestones](milestones.md#f5-02预制体资产2026-11-06) 与 [prefabs](../90-editor/prefabs.md)。
>
> F5-03 第一批效果/运动组件已完成（2026-11-06）：新增 `effects.bob`（沿轴浮动）、`effects.pulse`（缩放脉冲）、`effects.sway`（绕轴摆动）、`effects.lookAt`（朝向目标，可限速）、`effects.follow`（匀速靠近并停在 stopDistance）。全部走 `ComponentRegistry` 元数据注册，Inspector、场景存读、预制体与撤销/重做自动生效；系统增量改变换，保留用户手动拖动/缩放。`[Scene] ECS component selfTest PASS | components=7 | systems=7` 覆盖七个系统完整周期。详见 [milestones](milestones.md#f5-03内置效果运动组件第一批2026-11-06)。

> F5-04 原生组件接入已完成（2026-11-06）：`ComponentRegistry` 描述符新增 `addable`/`removable`/`drawProperties`/`serialized`/`required`/`canRemove` 与 `unavailableReason`；新增 `src/Scene/NativeComponents.*` 注册 9 个 `native.*`（Name/Transform/Hierarchy 必需只读，Character Controller 8 个 Float 与 Render Tag 可增删，Camera/Terrain/Streaming/VoxelWorld 为创建菜单托管、菜单置灰）。`captureComponentRecords` 与加载循环按 `serialized` 过滤，原生组件仍走专用字段，不重复写 v3。Inspector 的 Character 参数与 Character/Render Tag 移除均走 `SetComponentRecordCommand`；`DeleteEntityCommand` 增加 Character 显式快照。`[Scene] ECS component selfTest PASS | components=16 | systems=7` 覆盖 native 增删与去重；`[Editor] prefab selfTest PASS` 断言 capture 只含 effects.spin 且 Character radius 保留。全套 75 项 ALL PASS（日志 `build/validation/20261106-150438`），DocCheck PASS（documents=68 lines=11864）。详见 [milestones](milestones.md#f5-04原生组件接入统一-add-component2026-11-06)。
>
> F5-05 组件编辑工作流已完成（2026-11-10）：`PropertyDescriptor` 增加 `hasRange/minimum/maximum/dragSpeed/tooltip` 纯 UI 元数据，7 个效果与 Character 的 8 个参数全部登记范围、拖动速度和中文提示；Inspector 按元数据生成 Drag 控件，右键可重置单个属性。新增跨实体组件剪贴板与复制/粘贴/重置菜单，`+ Add Component` 移到组件区顶部并支持名称/分类/id 搜索，组件头默认展开。所有写入仍走 `SetComponentRecordCommand`。`[Scene] ECS component selfTest` 新增属性元数据断言。详见 [milestones](milestones.md#f5-05组件编辑工作流2026-11-10)。
>
> F5-06 场景级组件系统已完成（2026-11-10）：新增 `ComponentScope { Entity, Scene }` 与 9 个 `scene.*` 组件（environment/shadows/bloom/ssao/ssr/motionBlur/taa/water/fog），挂在 Scene 构造时创建的隐藏设置实体上；Scene scope 只能挂它、实体组件不能挂它，Hierarchy 与普通实体捕获看不到。`syncSceneSettings()` 把存在且启用的组件写回 `EngineSettings`，缺失或禁用即关闭。场景 v4 新增顶层 `sceneComponents` 与 `sceneComponentsAuthored`，旧文件从当前 settings 初始化；Inspector 无选中时显示 Scene Components，添加/调参/移除走撤销栈；`RenderSettingsPanel` 只保留 engine.json Save/Reload 与 GPU 批调试。`[Scene] ECS component selfTest PASS | components=25 | systems=7`，编辑器自检覆盖 v4 往返；全套 75 项 ALL PASS，验证日志 `build/validation/20261110-105403`，DocCheck PASS（documents=68 lines=12179）。详见 [milestones](milestones.md#f5-06场景级组件系统2026-11-10)。
>
> F5-07 粒子组件已完成（2026-11-10）：新增实体级 `ParticleEmitterComponent`（`effects.particles`，16 个属性，Inspector 可添加/调参/撤销），CPU 系统在 PostTransform 200 以 xorshift 随机数做锥形发射、重力、寿命与尺寸/颜色插值，全局上限 16384；新增独立 `ParticlePass`，在 Water 后、TAA 前以 billboard 前向混合进 HDR，深度测试不写、按相机距离远→近排序，每帧一份 host-visible 实例缓冲与一次 draw。`--particle-demo` 创建喷泉，`[Particle] emitters=1 alive=376 drawcalls=1`；场景自检 26 描述符/8 系统；全套 76 项回归 ALL PASS（含 `f6-particles`），日志 `build/validation/20261110-113649`。详见 [milestones](milestones.md#f5-07粒子组件2026-11-10)。
>
> F5-08 粒子 sprite 资源与贴图选择已完成（2026-11-10）：引入 Kenney Particle Pack（CC0）14 张 512x512 sprite 到 `Resources/Texture/Particles/Kenney/`，上游黑底灰度就地转 RGBA（A=亮度）；`effects.particles` 增加第 17 个属性 `texture`（相对 `Resources/Texture`，空=程序化圆形），Inspector 用专用选择器只列 `Particles/` 下的 sprite，支持过滤/重新扫描/清除，写入仍走 `SetComponentRecordCommand`。`ParticlePass` 启动时递归预加载贴图（最多 64 张）并分配 bindless 材质槽，实例增加 `materialId`，shader 未命中时回退程序化圆形；`--particle-demo` 改用 `Particles/Kenney/fire_01.png`。验证：`[Particle] textures=14 failed=0 status=ready`、`[Particle] demo emitter created | ... texture=Particles/Kenney/fire_01.png`、240 帧截图 `build/particle_texture.png` 可见火焰柱；全套 76 项 ALL PASS，验证日志 `build/validation/20261110-124840`。详见 [milestones](milestones.md#f5-08粒子-sprite-资源与贴图选择2026-11-10)。
>
> F5-09 组件目录完整分类已完成（2026-11-12）：用户反馈“添加组件的窗口啥都没有、找不到体积雾”。根因是旧 Add 弹窗跳过已挂载组件，而场景组件默认全部挂在隐藏设置实体上，所以 Scene Components 弹窗恒为空；地形/体素分支又直接隐藏入口。现在弹窗先收集当前 scope 全部 `exposed` 组件（包括已挂载），按中文分类用分隔标题聚合；已挂载项打勾置灰并提示“参数在下方组件列表里编辑”，未挂载项走 `componentAddBlockReason` 显示禁用原因。地形/体素保留 Add 入口，提示先“新建空实体并选中”，再添加体积雾等通用组件。分类本地化为 效果/运动/体积效果/角色/渲染/托管。截图 `build/editor_component_catalog.png`（实体 scope，体积雾在“体积效果”下）与 `build/editor_scene_catalog.png`（场景 scope，全部场景组件打勾）。同日再次反馈“UI 上找不到体积雾”：无选中对象时 `Scene Components` 旁新增「创建体积雾对象」按钮，直接新建 `Fog Volume` 实体、挂 `effects.fogVolume`、放到相机前方 18m 并选中；Scene scope 弹窗同时说明“体积雾 / 粒子是对象组件，不在这个列表里”。截图 `build/editor_fog_entry.png`。
>
> F6-05 体积云组件已完成（2026-11-10）：新增场景级 `scene.clouds`（10 属性，默认关闭）与 `CloudPass`，插在 Lighting 后、PlanarReflection/SSR/Glass/Water 前，读 G-Buffer 位置在 64³ 四通道噪声里 raymarch，4 步太阳自阴影、预乘 alpha 写 HDR、无深度附件；云层随场景 v4 `sceneComponents` 存读，`engine.json` 独立 `clouds` 节点。`--cloud-demo` 空场景一键出云；`f6-clouds` 断言初始化/首绘/0-0，全套 77 项 ALL PASS（日志 `build/validation/20261110-140504`，含 resize 后重绑 `gbPosition` 的 td03 用例）。云只进主 HDR，不进平面反射与 CSM 阴影，单层无时域。详见 [milestones](milestones.md#f6-05体积云组件2026-11-10)。
>
> F6-08 局部体积雾已完成（2026-11-11）：新增实体级 `effects.fogVolume`（14 属性）与 `FogVolumePass`。组件的 `center`/`size`/`rotation` 定义局部盒体（size 为全尺寸），再乘实体 WorldTransform，一个场景最多 8 个；Inspector 提供专用 Fog Volume 面板（Placement/Appearance/Lighting/Quality + 尺寸预设 + 视口青色线框），Render 窗口保留场景组件快捷开关。地形/流送地形/体素对象通过 `canAddComponent` 禁止挂通用组件；Add 弹窗完整分类、已挂载打勾，地形保留入口并可新建空实体。Pass 插在 Cloud 后、PlanarReflection/SSR/Glass/Water 前，读 G-Buffer 世界位置把步进截到最近不透明表面，盒内做高度衰减、边缘淡出、32³ 可平铺噪声与 HG 相函数，预乘 alpha 写 HDR，无深度附件、不写 G-Buffer；`--fog-demo` 空场景 + 流送地形一键出雾，`f6-fog-volume` 断言初始化/创建/首绘/0-0。雾只进主 HDR，不进平面反射与 CSM 阴影，单盒步数上限 32，作为已知边界。2026-11-12 补无选中对象的「创建体积雾对象」快捷入口，避免把“对象组件不在场景组件列表里”误判成组件缺失。详见 [milestones](milestones.md#f6-08局部体积雾2026-11-11)。
>
> F6-07 地形植被已完成（2026-11-10）：新增场景级 `scene.vegetation`（12 参数，默认关闭）与程序化 `VegetationGenerator`/`VegetationPass`，草为 3 张 60° 交叉卡片、树为 6 棱柱树干 + 3 层 8 边锥形树冠；Pass 插在 Geometry 后、SSAO 前，LOAD 四张 G-Buffer + 深度并写开，草/树各一次实例化 draw，因此参与 SSAO/级联阴影/延迟光照。`--vegetation-demo` 空场景 + 流送地形一键出草树，90 帧日志 `grass=10930 trees=117 draws=2`、`[Validation] errors=0 warnings=0`；新增 `f6-vegetation`，全套 79 项 ALL PASS（日志 `build/validation/20261110-174339`），DocCheck PASS。这条 2026-11-10 记录是初版边界（草/树不投自身阴影、模型为程序化低模）；2026-11-13 的 F6-09 已把树接入 CSM shadow pipeline、草接触圆盘兜底并升级网格/采样，详见 F6-09。详见 [milestones](milestones.md#f6-07地形植被2026-11-10)。
>
> R-08 水面暗反射补天空底色已完成（2026-11-06）：用户标注的远水发暗不是“黑天空反射”，旧 R-07 alpha 覆盖率在三种机位下都只有 UI 像素差（≤2537），已完整回退。`shaders/water.frag` 改为按反射亮度只提亮暗部（`0.34 * (1-smoothstep(0.04,0.22,luma))`），并把 `shallow/deep` 提到 `(0.050,0.160,0.167)`、`(0.016,0.058,0.075)`，远水 alpha 上限 0.92→0.89；亮水面/天空倒影保持原对比度。低机位 [16,8,140] 同机位 before/after 见 `build/water_tune_compare_v2.png`，水面区域 luma 76.2→81.8、蓝色 94.2→99.6。详见 [milestones](milestones.md#r-08水面暗反射补天空底色2026-11-06)，验证日志 `build/validation/20261106-164734`。

> R-09 低渲染倍率水面平铺亮暗格已完成（2026-11-06）：根因是 18m 法线贴图的低频脊线（K<32）在 `--render-scale 0.37`（用户 1536x864 窗口、约 710px 宽的 3D 视口）下每块只占约 43px，随平铺重复成规则的斜向亮暗格；1920 全分辨率看不出来，也不是 SSR/平面反射/TAA 缺陷。修复是把 `Resources/Texture/Water/water_normal.png` 换成高通 FFT 法线（K>=32，1024²、cutoff=32、exponent=1.9、slope_rms=0.22、seed=44），只保留 18m 平铺里约 0.56m 以下的细纹，低频形状交给 4 个解析大波；`shaders/water.frag` 未改。新增 `tools/GenerateWaterNormal.py` 可复现（重新生成与仓库内文件逐字节一致）。0.37x 前后图 `build/grid_before_lowres.png` / `build/grid_hp32_head_lowres.png`，全 1920 `build/grid_hp32_head_full.png`，对比 `build/water_grid_fix_compare.png`。详见 [milestones](milestones.md#r-09低渲染倍率水面平铺亮暗格2026-11-06)，验证日志 `build/validation/20261106-173309`。

> R-11 人物 SSAO 黑色花瓣已完成（2026-11-11）：角色世界坐标约 `(-142, 15, -112)`，G-Buffer Position 的 RGBA16F 在 `|x|≈144` 处 ULP 为 `0.125`，大于 SSAO 的 `bias=0.047`，深度比较把同一表面判成互相遮挡，形成黑色花瓣；关 SSAO、挪到原点、Position 临时改 32F 三组对照均消失。修复为 `GBufferFormat::Position = VK_FORMAT_R32G32B32A32_SFLOAT` 并加 `static_assert`，采样器本来就是 NEAREST，代价是每像素多 8 字节。同机位 before/after 见 `build/ssao_repro_on.png` / `build/ssao_r11_after.png`；79 项 ALL PASS，验证日志 `build/validation/20261111-103956`；详见 [milestones](milestones.md#r-11g-buffer-世界位置精度修复2026-11-11) 与 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 38。
>
> R-10 高角度水面泡沫规则菱格已完成（2026-11-06）：红框里的亮暗格不是 SSAO/反射/背景，也不是 18m 法线（R-09 已换高通）；逐项禁用后确认只要关掉 `foam` 混合水面就完全平滑。根因在 `shaders/water.frag` 的旧泡沫：`crest` 项没有乘 `shore`、铺满整个水面，且图案是 `sin(p.x*1.7)*sin(p.y*2.1)` 的乘积，俯视时形成规则菱形棋盘格。修复为 `shore = 1-smoothstep(0,1.8,thickness)` 门控、`crest=smoothstep(0.10,0.30,bigWaveHeight)`，噪声改采 `waterNormalMap` 的 LOD1.5 低频（`foamMask=smoothstep(0.34,0.74,noise)`），`foam=clamp(shore*foamMask*(0.75+0.25*crest),0,1)`；深水不再产生泡沫。同机位 before/after 见 `build/highgrid_C_before_samecamera.png` / `build/highgrid_C_after_samecamera.png`，对比图 `build/foam_grid_compare.png`；验证日志 `build/validation/20261106-180307`。经验归档见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 36；详见 [milestones](milestones.md#r-10高角度水面泡沫规则菱格2026-11-06)。

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
