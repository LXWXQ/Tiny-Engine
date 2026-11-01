---
id: status/milestones
title: 里程碑与实测基线
type: status
scope: 空
read-when: 对照历史进度、或需要"已知良好基线数字"
size: ~430 行
updated: 2026-11-02
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
| 回归 | 21 项全 PASS，stderr 全 0 字节 | `tools/Validate.ps1` |
| 验证层 | `errors=0 warnings=0` | 每次运行退出打印 |
| syncval | 0 errors / 0 warnings | `--validation-features sync` |
| 场景 | 103 子网格 + 天空盒 = `draw calls 104` | `[Perf]` 行 |
| 三角形 / 顶点 | 270459 / 811377（`vertices` 实际统计的是索引数） | 同上 |
| 描述符绑定 | `desc binds 1` | 同上 |
| 帧率 | 约 500 FPS（Debug） | 同上 |
| 材质槽位 | `26/256` 占用 | 启动日志 |
| GPU 时间分布 | Geometry ~1.0ms、Present ~1.1ms、SSAO ~0.58ms、其余 < 0.3ms | `--gpu-timing` |

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

> D-8～D-20 的逐轮细节已归档到 [milestones-archive](milestones-archive.md)；
> 本页保留 D-21 起的近期里程碑与当前实测基线。
## D-21：地形阶段 6 收尾（height/roughness + Surface Style）

- **PBR 纹理层**：`TerrainTextureGenerator` 在每层 albedo/normal 之外再生成
  height 与 roughness；`TerrainMaterial` 上传 16 张 256² 纹理。
  Terrain Set 2 扩展为 16 个固定 sampler：
  `0~3 albedo / 4~7 normal / 8~11 height / 12~15 roughness`。
- **Shader**：height 对四层基础权重做 0.72~1.28 调制并重新归一化；
  roughness 按层采样后混合，再叠加低频噪声。albedo 的双尺度抗平铺、
  normal 切线基和地形 SSAO opt-out 保持不变。
- **材质变体**：`TerrainConfig::surfaceStyle` 支持
  Natural / Arid / Alpine / Lush；只调整层权重、色调、法线强度和粗糙度，
  不重建纹理或网格。Inspector 增加 `Surface Style` 下拉框。
- **布局/序列化**：push constant 的 `terrainMaterialStyle` 填入原 padding，
  总大小仍为 208 字节。地形 JSON version 升到 3，新增 `surfaceStyle`；
  旧 v1/v2 由默认值 Natural 补齐。
- **测试**：
  - `TerrainSelfTest` 覆盖非法 Style、16 张纹理的尺寸/确定性/非恒定，
    以及三个 shader-only 字段不触发 geometry equality；
  - 编辑器 roundTrip 覆盖 `surfaceStyle=Alpine`、`uvTiling=1.7`；
  - `terrain-render` 覆盖 16-sampler Set 2 与 257² 输出。
- **验证（2026-11-01）**：
  - `[TerrainMaterial] 4 procedural layers ready | albedo+normal+height+roughness 256x256`
  - `[Terrain] selfTest PASS`
  - `[Editor] scene selfTest PASS | create/undo/redo + terrain create/generate`
  - `tools/Validate.ps1 -Incremental -SkipSetup`：**21/21 PASS**，
    stderr 全 0 字节，`[Validation] errors=0 warnings=0`。
  - p3 baseline（60 帧、warmup 30）通过；首轮 Present 计时抖动在同代码
    复跑时消失（Present -0.45%）。
- **阶段状态**：阶段 6 当前范围已完成。后续为外部美术纹理替换接口，
  以及阶段 7 的 JobSystem 异步 / chunk / LOD / 增量 SpatialIndex。
## D-22：地形外部纹理覆盖接口

- **加载规则**：`TerrainMaterial` 检查 `Resources/Texture/Terrain/` 下是否
  同时存在 16 张 PNG：
  `albedo/normal/height/roughness` × `sand/grass/rock/snow`。
  全部存在时优先加载外部资产；缺任意一张则整套回退到内置程序化纹理，
  避免部分覆盖造成层间风格/UV 不一致。
- **格式**：albedo 按 sRGB 加载；normal/height/roughness 按线性 UNORM 加载。
  命名与要求写在 `Resources/Texture/Terrain/README.txt`。
- **范围**：这是全局地形材质覆盖，不进入场景 JSON，也不支持每个地形独立
  贴图路径；Level 级风格仍由 `surfaceStyle` 控制。
- **验证（2026-11-01）**：
  - 临时生成 16 张 8×8 RGBA PNG 后启动：
    `[TerrainMaterial] 4 external PBR layers ready | .../Resources/Texture/Terrain/`
    `[Validation] errors=0 warnings=0`，stderr 0 字节；
  - 删除测试 PNG 后启动：回到
    `[TerrainMaterial] 4 procedural layers ready | albedo+normal+height+roughness 256x256`；
  - `terrain-render` 与编辑器 roundTrip 继续通过；
  - `tools/Validate.ps1 -Incremental -SkipSetup`：**21/21 PASS**，
    stderr 全 0 字节，`[Validation] errors=0 warnings=0`。
- **未附带**：仓库不附带外部美术纹理，默认仍使用确定性程序化纹理。
## D-23：地形阶段 7 第一轮（JobSystem 异步生成）

- **CPU 异步**：新增 `Scene::requestRegenerateTerrain()`。主线程复制不可变
  `TerrainConfig`，把 `HeightField` + `TerrainMeshBuilder` 提交给 JobSystem；
  worker 不触碰 Vulkan 对象。
- **主线程上传**：`Scene::update()` 每帧检查 `JobHandle::isComplete()`，
  完成后才构造 `Model`（staging 上传）、替换 `MeshComponent`、刷新 Bounds 与
  SpatialIndex。生成期间旧网格继续渲染。
- **版本令牌**：每次请求分配全局递增 request serial；同一实体可以被新请求
  supersede，旧任务结果与回调静默丢弃，避免慢任务覆盖新参数。
- **命令栈**：Inspector Generate 不再同步等待；成功回调在主线程构造
  `RegenerateTerrainCommand`，保留 old/new Mesh 的 undo/redo 语义。生成中按钮
  禁用并显示 `Generating on JobSystem...`。
- **生命周期**：`TerrainComponent::generating` 供 UI 状态；`clearEntities()` 丢弃
  待完成回调；`Scene::cleanup()` 等待所有 worker 完成后再销毁 Scene 依赖。
  `waitForTerrainGeneration()` 仅供自检/诊断使用。
- **保留同步 API**：`createTerrainEntity()`、`regenerateTerrain()`、
  场景加载和 `--terrain-test` 初始化仍可同步执行；异步目前只覆盖显式重建。
- **验证（2026-11-01）**：
  - `[Editor] async terrain generation PASS | stale discarded, revision=3`：
    连续两个请求只安装最新结果；
  - `[Terrain] async generation PASS | revision=2 | 66049 vertices, 393216 indices`：
    `--terrain-test` 覆盖 JobSystem + 主线程 Model 上传；
  - `tools/Validate.ps1 -Incremental -SkipSetup`：**21/21 PASS**，
    stderr 全 0 字节，`[Validation] errors=0 warnings=0`。
- **下一轮**：chunk/LOD/邻接缝合、增量 SpatialIndex、GPU compute 生成；
  当前无任务取消，supersede 的 CPU job 会跑完，GPU 上传仍有短帧尖峰。
## D-24：阶段 7 第二轮（协作取消 + 增量空间索引）

- **地形协作取消**：`PendingTerrainBuild::cancelRequested` 使用 `atomic<bool>`；
  新请求 supersede 旧任务时置位，`HeightField::build()` 与
  `TerrainMeshBuilder::build()` 每个顶点行检查并尽早退出。即使旧任务已接近
  完成，request serial 仍保证其结果与回调被丢弃。
- **编辑器**：Generate 在生成期间仍可再次点击；新请求使用当前参数 supersede
  旧任务，状态显示 `Generating on JobSystem... press Generate to replace.`。
  旧网格一直保留到最新任务完成。
- **SpatialIndex 增量更新**：`SpatialIndex` 为每个 entity 记录占用 cell，
  新增 `remove()`，`insert()` 变成 upsert（先移除旧 bucket）。`Scene` 维护
  `indexedBounds`，只在世界 AABB 变化时更新 grid，并删除 destroyed/skybox
  的旧候选。
- **仍未全量 dirty**：世界变换与逐实体 AABB 计算仍每帧扫描全部 Mesh；
  后续 D-27/D-28 已加入 Transform/Bounds dirty 与显式 dirty 队列。
- **验证（2026-11-01）**：
  - `[Terrain] selfTest PASS` 新增取消标志早退断言；
  - `[Scene] ECS/spatial selfTest PASS` 新增 SpatialIndex upsert 无残留 bucket、
    remove 幂等与 entryCount 断言；
  - `[Editor] async terrain generation PASS | stale discarded` 继续覆盖
    supersede；
  - `tools/Validate.ps1 -Incremental -SkipSetup`：**21/21 PASS**，stderr 全 0，
    `[Validation] errors=0 warnings=0`。
- **下一轮**：chunk/LOD/邻接缝合、Transform dirty flag / 区域 SpatialIndex、
  GPU compute 生成。

## D-25：阶段 7 第三轮（chunked submeshes + 逐块剔除）

- **配置**：`TerrainConfig` 新增 `chunkGridX/chunkGridZ`，默认 2×2，
  校验 `[1,4]`；属于几何参数，修改后需要 Generate。地形 JSON version 升到 4。
- **网格**：`TerrainMeshBuilder` 仍使用一份共享顶点缓冲，但索引按 chunk
  连续重排；每个 `TerrainSubMesh` 记录 `firstIndex/indexCount`、chunk 坐标和
  局部 AABB。边界顶点共享，所以同 LOD chunk 没有裂缝。
- **渲染**：`SubMesh` 新增可选局部 bounds；`GeometrySystem` 通过
  `SubMeshCuller` 对每个 chunk 做世界 AABB -> frustum 测试，不可见 chunk
  直接跳过 draw。`RenderTelemetry` 增加 `subMeshesCulled`，内置 Stats 显示
  `Chunks culled`。
- **测试**：
  - `TerrainSelfTest` 覆盖非法 chunk grid、chunk 数、索引区间连续覆盖与
    AABB 有效性；
  - `Scene::selfTest` 覆盖 SubMeshCuller 可见/不可见用例；
  - 编辑器 roundTrip 覆盖 `chunkGridX=3 / chunkGridZ=2`，并检查默认地形
    Model 有 4 个子网格且都带 bounds；
  - `terrain-render` 的 MustContain 增加 `chunks=4`。
- **验证（2026-11-01）**：
  - 正常视角截图：`Draw Calls: 5`，`Chunks culled: 0`；
  - 跨 chunk 边界视角：`Draw Calls: 3`（skybox + 2 个可见 chunk），
    `Chunks culled: 2`，画面无洞/裂缝；
  - `tools/Validate.ps1 -Incremental -SkipSetup`：**21/21 PASS**，stderr 全 0，
    `[Validation] errors=0 warnings=0`。
- **下一轮**：LOD 选择、邻接缝合/T 形裂缝处理、独立 chunk 实体与流送；
  当前仍是单个地形实体和一个总 SpatialIndex AABB。

## D-26：阶段 7 第四轮（chunk LOD + 裙边接缝）

- **配置**：`TerrainConfig` 新增 `lodLevelCount`（1~3，默认 3）和
  `lodDistanceScale`（0.25~4，默认 1）；Chunk/LOD 层级属于几何参数，
  改后需要 Generate，距离倍率只影响运行时选层。地形 JSON version 升到 5。
- **网格**：每个 chunk 按 `1/2/4` cell 步长生成 LOD0/1/2，全部索引区间仍在
  同一个 Model 中；每个 LOD 独立生成顶点切片和局部 AABB。每个 chunk/LOD
  生成一圈向下 `heightScale * 0.08` 的裙边，用来遮住相邻 chunk 选择不同 LOD
  时的 T 形裂缝。
- **运行时**：GeometrySystem 按相机到 chunk 世界 AABB 的距离只提交一个
  `terrainLod`；其他 LOD 计入 `LOD skips`，不可见 chunk 计入 `Chunks culled`。
  Stats 增加 `Terrain LOD: lod0 / lod1 / lod2`。
- **拾取**：`SubMesh` 记录 `terrainSurfaceIndexCount`；Model 的 CPU 射线拾取
  只使用 LOD0 的 surface 区间，不把其他 LOD 的重复三角形或裙边当成地面。
- **测试**：
  - `TerrainSelfTest` 覆盖非法 LOD 配置、每 chunk/LOD 索引覆盖、LOD 索引
    数量递减、surface/裙边分离、AABB 与法线/绕序；
  - `Scene::selfTest` 增加 `selectTerrainLod()` 的 0/1/2 距离断言；
  - 编辑器 roundTrip 覆盖 `lodLevelCount=2 / lodDistanceScale=1.6`，并检查
    默认 2×2×3 = 12 个地形子网格；
  - `terrain-render` MustContain 增加 `lods=3` 与 `submeshes=12`。
- **验证（2026-11-01）**：
  - 近景：`Draw Calls: 5`，`Terrain LOD: 4 / 0 / 0`，`LOD skips: 8`；
  - 中景：`Draw Calls: 5`，`Terrain LOD: 0 / 4 / 0`；
  - 远景：`Draw Calls: 5`，`Terrain LOD: 0 / 0 / 4`；
  - 混合层级：`Terrain LOD: 0 / 2 / 2`，画面无可见裂缝；
  - 跨 chunk 边界：`Draw Calls: 3`，`Chunks culled: 2`；
  - `tools/Validate.ps1 -Incremental -SkipSetup`：**21/21 PASS**，stderr 全 0，
    `[Validation] errors=0 warnings=0`。
- **下一轮**：拓扑级 stitching / LOD cross-fade、流送、独立 chunk 实体、
  GPU LOD/indirect 与 GPU compute 生成。当前 LOD 全部预生成，显存随层级增长。

## D-27：阶段 7 第五轮（Transform dirty + 增量 SpatialIndex）

- Transform/Bounds 增加 revision+dirty；setter 对未变化值早退。
- 世界变换只重建脏子树；父变化强制整棵子树，干净帧更新数为 0。
- SpatialIndex 只对 dirty bounds 重算世界 AABB/upsert；新增 world/spatial 更新计数。
- 命令恢复、CameraController、ImGui 相机面板均走 dirty setter。
- 测试覆盖 dirty 子树计数、sourceRevision、移动对象 1/1 更新、干净帧 0/0。
- `tools/Validate.ps1 -Incremental -SkipSetup`：21/21 PASS，stderr 全 0。
- **当时仍保留** O(N) dirty 标志扫描、stale sweep；D-28 已改成显式 dirty 集合（历史记录保留）。

## D-28：阶段 7 第六轮（显式 dirty 队列 + destroy 同步索引）

- Scene 新增 `dirtyTransformEntities` / `dirtySpatialEntities`；
  `markTransformDirty()` / `markSpatialDirty()` 由创建、移动、命令恢复、
  地形上传和销毁路径显式调用。
- `updateWorldTransforms()` 只从 dirty 集合找根节点，更新脏节点及其后代；
  干净帧不再扫描全部 Transform。
- `refreshSpatialIndex()` 只消费 dirty spatial 集合；干净帧不扫描 Mesh/Bounds，
  也不再做 indexedBounds stale sweep。
- `Scene::destroyEntity()` 同步从 SpatialIndex、indexedBounds 和 dirty 集合移除。
- `SetTransformCommand` / `DeleteEntityCommand` 接受可选 Scene*；编辑器路径传入
  并通知 dirty 队列。创建/地形/删除命令恢复也会 mark。
- 测试：Scene dirty 子树计数、Editor 移动对象 world/spatial=1、再次刷 0/0。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` 21/21 PASS，stderr 全 0，
  `[Validation] errors=0 warnings=0`。
- 仍保留：dirty 通知是显式契约，直接写 public 字段或 registry.destroy 可绕过；
  只有统一网格，无 chunk/区域索引。

## D-29：阶段 7 第七轮（地形纹理打包 + 旧 Model 延迟回收）

- TerrainMaterial 在 CPU 侧把每层 height.r 与 roughness.r 打包成 RGBA：
  R=height、G=roughness、B/A 预留。外部 16 张 PNG 约定不变，但同层
  height/roughness 必须同分辨率。
- GPU Set2 从 16 个 sampler 降到 12 个（4 albedo + 4 normal + 4 packed）；
  片元采样从约 20 次/像素降到 16 次。
- 内置程序化路径与外部 PNG 全量覆盖路径都验证通过；
  `[TerrainMaterial] ... packed(height/roughness)` 为稳定日志。
- Scene 新增 4 帧旧 Model 延迟回收队列：同步/异步地形重建、destroy、clear
  都会把被替换 Model 先保留 4 帧，避免 GPU 在途帧访问已销毁缓冲。
- 编辑器自检断言 regenerate 后旧 Model 已进入退休队列。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` 21/21 PASS，stderr 全 0，
  `[Validation] errors=0 warnings=0`；外部 16 PNG 临时覆盖启动测试 PASS。
- 仍保留：地形 Set2 仍有 12 个固定 sampler；LOD/流送/GPU compute 等阶段 7
  项未完成。

## D-30：阶段 7 第八轮（LOD 滞回 + 抖动 cross-fade）

- `SubMeshCulling` 新增 LOD 滞回与过渡状态机：
  - 粗化需超过 enter 阈值 15%，细化需低于 exit 阈值 15%；
  - 换层持续 0.24s，稳定层只在过渡完成后切换。
- GeometrySystem 按 `(entity, model, revision)` 缓存每 chunk 状态；
  过渡期同时提交 from/to 两个子网格，其余 LOD 仍计入 `LOD skips`。
- fragment 用 interleaved gradient noise 生成屏幕空间互补 mask：
  outgoing 保留 `noise >= blend`，incoming 保留 `noise < blend`；
  push constant 使用原有尾部 padding，大小仍为 208 字节。
- Stats 新增 `LOD blends` 与 `LOD switches`；过渡帧 draw call 会暂时增加。
- 测试：
  - Scene selfTest 覆盖 15% 滞回边界、初始选层、0.24s 过渡中间值与完成态；
  - shader 自动编译/SPIR-V 校验通过；诊断动画帧出现 `LOD blends: 2`，
    过渡完成后回到 `0`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，stderr 全 0，
  `[Validation] errors=0 warnings=0`。
- 首次回归仅 `p3-baseline-diff` 的 Present timing 抖动超阈值（视觉 diff 0.008），
  重跑通过；其余 20 项一次通过。
- 仍保留（B-02/D-38 已用拓扑级 stitching 取代裙边缝隙方案）：抖动过渡在强运动模糊下可能有短暂噪声；
  流送、GPU LOD/indirect 与 GPU compute 生成仍未做。

## D-31：阶段 7 第九轮（地形 indirect 合批）

- Device 按物理设备支持启用 `multiDrawIndirect`；不支持时 GeometrySystem
  自动回退逐子网格直绘。
- GeometrySystem 为每个在飞帧创建 host-visible
  `INDIRECT_BUFFER|STORAGE_BUFFER`，容量 4096 条
  `VkDrawIndexedIndirectCommand`。
- 稳定 LOD 帧把每个可见 chunk 的一个命令写入 buffer，每地形实体只调用
  一次 `vkCmdDrawIndexedIndirect`；过渡帧仍逐 from/to 子网格直绘，保留
  逐 chunk 的 dither blend。
- Stats 新增 `Terrain indirect groups`；逻辑 drawCalls 仍按命令数统计。
- 诊断近景：`Draw Calls: 5`（skybox + 1 个地形 indirect group），
  `Terrain indirect groups: 1`，`Terrain LOD: 4 / 0 / 0`，画面与直绘一致。
- 首次验证暴露 `multiDrawIndirect` feature 未启用和 VMA 持久映射析构错误；
  已修复并补 fallback。最终 `tools/Validate.ps1 -Incremental -SkipSetup`
  **21/21 PASS**，stderr 全 0，`[Validation] errors=0 warnings=0`。
- 仍保留：LOD/可见性选择仍在 CPU；GPU 选层与 compute 写 indirect 参数、
  流送、拓扑级 stitching 尚未做。
## D-32：阶段 7 第十轮（CPU 地形碰撞查询）

- `TerrainComponent` 新增运行期 `shared_ptr<terrain::HeightField> cpuHeightField`：
  - 与高度场/网格在同一 JobSystem 任务中构建；
  - GPU 上传成功后才随 `TerrainComponent` 原子替换；
  - 不参与序列化，但无 GPU 句柄，可安全进入撤销/重做快照。
- `Scene` 新增查询 API：
  - `sampleTerrainHeightLocal()`：局部 XZ 双线性采样，O(1)，越界拒绝；
  - `sampleTerrainHeightWorld()`：从世界包围盒上方垂直 raycast，支持实体旋转/缩放；
  - `raycastTerrain()`：任意世界空间射线，返回最近命中点/距离。
- 世界查询复用 LOD0 picking 三角形，排除裙边与其他 LOD。
- 同步创建、`regenerateTerrain()`、异步生成完成路径均维护碰撞高度场；
  重生成撤销会恢复旧 `HeightField` 与旧 Model 的同代数据。
- `selfTestWithScene()` 新增：
  - 33×33 HeightField 尺寸与非空断言；
  - 局部高度与高度场采样一致；
  - 世界垂直采样与 raycast 命中中心高度；
  - 越界局部/世界查询拒绝；
  - 重生成 undo 恢复旧碰撞高度场。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 未做：物理引擎/角色控制器接入、运行期雕刻、地形专用空间加速结构。

## D-33：阶段 7 第十一轮（自动 dirty 通知）

- `TransformComponent` / `BoundsComponent` 增加 owner callback：
  - `setTranslation()` / `setScale()` / `setEuler()` / `setOrientation()` /
    `markDirty()` 直接通知所属 Scene；
  - `Scene::attachDirtyTracking()` 在实体创建、场景加载、重生成后绑定；
  - 快照恢复函数先 `clearDirtyOwner()`，emplace 后再重新绑定，避免撤销到
    新实体时回调仍指向旧 entity ID。
- Scene 创建路径全部接入：相机、默认场景实体、`createEntity()`、
  `createMeshEntity()`、`createTerrainEntity()`、`BuildFromDescription()`、
  异步地形完成与命令恢复。
- 编辑器回归删除显式 `markTransformDirty()`，验证 setter 能自动入队；
  新增 Bounds `markDirty()` 自动 spatial 更新断言。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 仍保留：public 字段直写与 `registry.destroy()` 可绕过回调；后续流送阶段
  应改为组件私有字段 + 区域索引 + 统一销毁 API。

## D-34：阶段 7 第十二轮（GPU compute 高度场）

- 新增 `TerrainComputeGenerator` 与 `shaders/terrain_height.comp`：
  - GPU 执行 Perlin/fBm、octave seed、ridge/detail、island/edge mask 与 height curve；
  - storage buffer 输出 normalized heightfield，fence 同步后 readback 到 `HeightField`；
  - `HeightField::generatedOnGpu` 记录来源，CPU 路径仍作为失败 fallback。
- 生成路径：
  - 同步创建 / 场景加载 / `regenerateTerrain()` 先 GPU 高度场，再 CPU MeshBuilder；
  - `requestRegenerateTerrain()` 主线程完成 GPU 高度场，CPU 网格仍提交 JobSystem；
  - `--terrain-test` 与编辑器自检断言同步/异步均使用 GPU 高度场。
- GPU/CPU 参考（33×33，含 ridge/detail/island/edge falloff）：
  `maxDiff=2.98023e-07`，阈值 `5e-4`。
- 新增 [backlog](../70-status/backlog.md) 后续任务台账，下一项为 B-01 独立 chunk
  实体与流送。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 仍保留：GPU dispatch/fence/readback 在主线程等待；顶点/索引组装与 GPU 上传
  仍在 CPU。

## D-35：B-01a（独立 chunk 实体 + 流送基础）

- `TerrainConfig` 新增 `sampleOriginX/Z` 与 `localColorRange`：
  - 独立 chunk 以世界中心作为噪声采样原点，边界高度连续；
  - 流送 chunk 关闭局部颜色范围拉伸，避免跨 chunk 色带。
- 新增 `TerrainStreamingComponent` manager 与 `TerrainChunkComponent` 运行期标记：
  - manager 只存 chunk 模板、loadRadius、unloadPadding、maxLoadsPerFrame；
  - 每个 chunk 是独立 ECS 实体，拥有独立 Model/Mesh/Bounds；
  - 相机 3×3 半径、最近优先、每帧加载预算、卸载滞回。
- 场景描述支持可选 `terrainWorld`，chunk 实体不逐项序列化，加载后按 manager
  模板重建。
- `--terrain-test` 内置 smoke：9 chunks / 9 independent Models / 清理后无残留；
  编辑器自测覆盖接缝高度 `< 1e-4`、移动卸载/重载、manager JSON round-trip。
- 修复流送卸载时在遍历 map 中调用 `destroyEntity()` 导致 map 迭代器失效的崩溃；
  改为先收集、循环结束后统一销毁。
- 诊断截图：可见 5 chunks -> `Draw Calls 6`、`Terrain indirect groups 5`、
  `Terrain LOD 5/0/0`，画面无明显边界裂缝。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 下一项 B-01b：GPU 高度场流送异步化、场景描述实际重建流送、任务预算基线。

## D-36：B-01b（流送异步化 + 加载硬化）

- `TerrainComputeGenerator` 改为 **8 个独立 in-flight slot**：
  - 每槽独立 command buffer/fence/storage buffer/readback/descriptor set；
  - `submit()` 异步提交，`poll()` 只查 fence，`release()` 标记废弃，
    `collectFinished()` 回收 fence 完成的废弃槽位，`waitAll()` 仅用于清理。
- `PendingTerrainBuild` 改为三阶段：
  1. GPU heightfield pending（主线程只 poll）
  2. CPU mesh JobSystem pending（fence 完成后才提交）
  3. main-thread upload/ECS install
- 手动异步 Generate 与流送共用同一条异步高度场流水线；主线程不再
  `vkWaitForFences` 等高度场。同步场景加载仍保留阻塞 `generate()`。
- 覆盖请求 / 卸载 chunk 会把 GPU 请求标记 abandoned，槽位在 fence 完成后回收。
- 编辑器自检：
  - 创建 manager 后立即断言 GPU 请求已 in-flight，且本帧还没有 generated chunk；
  - 捕获 `terrainWorld` 描述后实际 `buildFromDescription()`，重新流送 9 个独立
    chunk/Model，验证 `rebuild=OK`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 下一项 B-01c：跨 chunk picking/删除撤销语义、JSON 文件加载 smoke、
  跨 chunk 帧时间基线。

## D-37：B-01c（流送交互与性能硬化）

- Viewport 拾取命中 runtime `TerrainChunkComponent` 后，通过
  `Scene::resolveTerrainChunkOwner()` 解析到 streaming manager；日志保留命中
  chunk 坐标。Hierarchy 隐藏 runtime chunks，避免选择下一帧可能卸载的实体。
- Inspector 新增 `Streamed Terrain`：半径、卸载 padding、每帧预算、mapped chunks、
  in-flight GPU height 请求、last/max streaming update ms。
- `DeleteEntityCommand`：
  - 若目标是 runtime chunk，先解析 manager；
  - snapshot 包含 `TerrainStreamingComponent`；
  - undo 重建 manager 后调用 `Scene::adoptStreamingTerrainManager()` 重新流送；
  - redo 修复为销毁 undo 新建的实体 ID，而不是已失效的原始 ID。
- `terrainWorld` 覆盖真实 JSON 文件 save/load，再由 `buildFromDescription()` 重建
  9 个独立 chunk/Model。
- 编辑器跨 4 chunk 移动样例记录 `maxStreamingUpdateMs=0.6123`
  （Debug/本机基线，不作为跨机性能门槛）。
- 编辑器 PASS：
  `chunks=9 independentModels=9 seam=OK unload=OK rebuild=OK picking=OK
  deleteUndo=OK maxUpdateMs=0.6123`
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 下一项 B-02：拓扑级 LOD stitching；先做可关闭裙边的 A/B 与索引/绕序断言。

## D-38：B-02（拓扑级 LOD stitching）

- 每个 chunk/LOD 的 chunk 外边界固定为**全高度场分辨率**，内部仍按
  LOD 1/2/4 cell 步长降采样。
- 四条过渡环带按归一化段参数把全分辨率外环与粗网格内环三角化：
  - 外环四个角与内环共享索引；
  - 每个四边形只生成一组不重叠三角形；
  - +Y 绕序由 `windingFacesPositiveY` 断言；
  - 相邻 chunk 的任意 LOD 组合共享同一组边界位置、法线、UV 与色。
- 极小 chunk/LOD 自动回退到全分辨率网格，保持边界契约。
- `TerrainConfig::skirtsEnabled` 新增并进入 `geometryEquals()`/JSON；默认 true
  作为保守回退，`--terrain-test` 与流送 smoke 使用 `skirts=false`。
- `TerrainSelfTest` 新增：
  - skirts=off 时 `surfaceIndexCount == indexCount`；
  - 3×3 LOD 邻接组合、18 条边界的位置/法线/UV/色一致断言。
- 预编译截图：`.tmp/terrain_b02_noskirt.png`（1920×1061，无裙边渲染，
  stderr 0，validation 0/0）。
- 编辑器 PASS：
  `chunks=9 independentModels=9 seam=OK stitch=OK skirts=off ...`
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 日志：`build/validation/20261101-195549`。
- 下一项 B-03：GPU LOD 选择 / GPU-driven culling。

## D-39：B-03（GPU LOD 选择 / GPU-driven culling）

- `GeometryPass::addToGraph()` 在 `Geometry` 之前注册无附件
  `TerrainGpuSelection` pass：
  - 仍录制在 graphics command buffer；
  - dispatch 位于 dynamic rendering 之外；
  - 不引入独立 compute queue 的跨队列 ownership/信号量。
- 每帧上传最多 1024 条 chunk record，容量 64 terrain entities：
  - world matrix、局部 bounds、distanceScale、lodCount；
  - commandBase、stateIndex、reset、entitySlot；
  - lod0/lod1/lod2 firstIndex/indexCount。
- `terrain_lod.comp` 在 GPU 上完成：
  - world AABB + 六平面 frustum cull；
  - LOD 距离、15% 滞回、0.24s transition 状态；
  - per-entity atomic command count；
  - indirect command / stats / debug SSBO 输出。
- 每个 terrain entity 预留 32 条 `VkDrawIndexedIndirectCommand`。Graphics
  每实体调用一次 `vkCmdDrawIndexedIndirectCount`，draw count 完全由 GPU 写、
  GPU 读，不回读 CPU。
- 过渡命令的 blend/mode 量化为 `firstInstance` payload；`mrt.vert` 解码成
  flat varying，`mrt.frag` 继续用 Bayer dither 做 from/to 互补覆盖。GPU
  indirect 过渡不再依赖逐 draw push constant。
- 每 frame-slot 独立 state/count/stats/debug buffer；无 `drawIndirectCount`
  设备保留 CPU selection/command-building fallback。
- `--terrain-test` 启用 CPU parity mirror，并临时切换
  `lodDistanceScale=0.01 -> 1.0` 强制过渡：
  - `[Terrain] GPU selection selfTest PASS | matches=3 records=4`
  - `[Terrain] GPU selection transition PASS | records=4`
- 过渡期截图：`.tmp/terrain_b03_blend.png`
  - `Draw Calls: 5`
  - `LOD blends: 2`
  - `Terrain indirect groups: 1`
  - validation 0/0。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 日志：`build/validation/20261101-203943`。
- 下一项 B-04：GPU compute 网格组装与 GPU 上传。

## D-40：B-04（GPU compute 网格组装与 GPU 上传）

- 新增 `TerrainGpuMeshBuilder` + `shaders/terrain_mesh.comp`，把单 chunk
  terrain 的顶点/索引组装迁到 compute：
  - 顶点输出 11 个 tight float（position/color/normal/uv），与 CPU
    `Vertex` 的 44-byte 布局逐字节对齐；
  - 索引输出覆盖 B-02 全分辨率外边界、粗内部、四条 transition band 与可选裙边；
  - 参数/高度场走 host-visible SSBO，顶点/索引直接写 device-local buffer。
- `Model::createFromGpuMesh()` 接管外部 VMA 顶点/索引 allocation；渲染路径不再
  经过 staging copy。CPU 只生成全分辨率 picking mirror 和 submesh 元数据。
- `Scene` 同步/异步生成都接入 GPU mesh；`TerrainComponent` 增加
  `meshGeneratedOnGpu`，流送 9 个 chunk 全部标记 GPU 生成。多 chunk 手动地形与
  无 compute 设备继续走 CPU builder fallback。
- GPU/CPU parity selfTest 逐顶点、逐索引、逐 submesh 校验：
  - `[Terrain] GPU mesh assembly selfTest PASS | vertices=6691 indices=37824
    lods=3 skirts=on localColor=on maxPosError=0 maxAttrError=8.27056e-07`
  - `[Terrain] streaming chunk test PASS | chunks=9 independentModels=9 gpuMesh=9`
- 修复 B-03 持久 LOD state 索引：`stateIndex` 是 chunk slot，不是 scalar word
  offset；shader 统一用 `stateIndex * 4` 访问 4 个 int，并在 slot 复用时清理
  所有 frame slot、补 `COMPUTE -> COMPUTE` barrier。否则 stateIndex≥1 的 chunk
  在 LOD 过渡时会读写相邻槽，造成 from/to 翻转。
- 编辑器 PASS：
  `chunks=9 independentModels=9 seam=OK stitch=OK skirts=off gpuMesh=OK
  unload=OK rebuild=OK picking=OK deleteUndo=OK`。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`。
- 增强同步校验：`--validation-features sync` 的 terrain smoke 保持
  `errors=0 warnings=0`。
- 日志：`build/validation/20261101-213544`。
- B-05 已完成，见下一条 D-41。

## D-41：B-05（异步 GPU 上传管线）

- `TerrainGpuMeshBuilder` 从 B-04 的单命令缓冲 + 同步 fence 等待改为 4 个异步
  slot。每个 slot 拥有独立 command buffer、fence、descriptor set、host-visible
  height/params buffer 和输出 VBO/IBO；`submit()` 只做 buffer 准备、参数上传、
  compute 录制与 `vkQueueSubmit`，立即返回 request id。
- `poll()` 用 `vkGetFenceStatus` 查询完成；`release()` 标记 abandoned，
  `collectFinished()` 在 fence 完成后回收，`waitAll()` 供取消/关闭使用。
  `build()` 保留为同步加载包装器，内部仍走 submit/poll。
- 全分辨率 CPU picking mirror 移出 `prepareBuild()`：新增
  `buildPickingMirror()` 在 `std::async` 工作线程生成 positions/indices，
  与 GPU assembly 并行。`poll()` 对 future 做零等待检查，未完成返回
  `Pending`，不在主线程等待；`destroySlotResult()`/`waitAll()` 才等待 future。
- `Scene` 新增 `PendingTerrainBuild::gpuMeshRequest` 与三阶段逻辑：
  - Stage 2a：poll 正在执行的 GPU mesh 请求；
  - Stage 2b：有空闲 slot 才 submit；slot 满时任务保持 pending，不再回退
    主线程同步构建；
  - Stage 2c：GPU 不可用或 submit/poll 失败才提交 CPU JobSystem fallback。
  替换、取消、删除与 Scene 关闭路径会 release 请求或 `waitAll()`。
- 新增 Scene 遥测：`getMaxTerrainPollMs()`、`getTerrainPollCount()`、
  `resetTerrainPollStats()`、`getTerrainGpuMeshStats()`；`Scene::update()` 与
  `waitForTerrainGeneration()` 都记录 poll 耗时。
- `--terrain-test` 新增 257×257 单 chunk 回归（skirt off、绝对色域）：
  - 同步 `createTerrainEntity()` 提供 baseline；
  - 异步 `requestRegenerateTerrain()` 后断言 callback、revision≥2、
    `meshGeneratedOnGpu`、`generationBackend` 含 GPU mesh、submit 计数增加，
    且 `maxPoll < syncBaseline`；
  - 实测日志：
    `[Terrain] async GPU upload PASS | resolution=257x257 chunks=1
    syncBaseline=8.3553ms asyncMaxPoll=0.9878ms maxSubmit=0.3255ms
    generation=7.6449ms spikeRatio=8.45849 revision=2 gpuMesh=1`
    （Debug/本机，数值随机器波动）。
- GPU mesh selfTest 保持：
  `vertices=6691 indices=37824 lods=3 skirts=on localColor=on
  maxPosError=0 maxAttrError=8.27056e-07`。
- 流送 9 chunk 全部 GPU mesh：`chunks=9 independentModels=9 gpuMesh=9`；
  17×17 流送也验证了多 slot 并发提交/回收路径。
- 验证：`tools/Validate.ps1 -Incremental -SkipSetup` **21/21 PASS**，
  stderr 全 0，`[Validation] errors=0 warnings=0`；日志
  `build/validation/20261101-222753`。
- 下一项 B-06：组件私有字段 + 统一销毁 + 区域索引。

## D-42：B-06（组件私有字段 + 统一销毁 + 区域索引）
- `TransformComponent`/`BoundsComponent` 私有 + getter/`setPose`；`SpatialIndex` 增加 XZ region bucket，Scene 对齐 chunk size 并提供 chunk 查询。
- `clearEntities()` 与相机复用改走 `Scene::destroyEntity()`；生产代码只剩该函数内一处 `registry.destroy()`，`--terrain-test` 覆盖 5×5 region 查询。
- 自检 `spatial region query PASS | chunks=9 regions=5x5`、`gpuMesh=OK`；验证 `21/21 PASS`，stderr 0，`errors=0 warnings=0`，日志 `20261101-225723`。下一项 B-07。

## D-43：B-07（物理/角色控制器 + 地形雕刻）
- `terrain::TerrainBrush` 支持 Raise/Lower/Smooth/Flatten；`Scene::sculptTerrain()` 用 B-06 region 查询只重建相交 chunk，9 chunk smoke 仅重建 1 个，其余 revision/Model 不变。
- 新增 `CharacterControllerComponent`/`createCharacterEntity()`/`updateCharacterController()`/`sampleTerrainGroundHeightWorld()`/`raycastTerrainWorld()`；编辑器 Sculpt Brush、Create Character、Play WASD 驱动，角色组件可序列化。
- `[Terrain] sculpt PASS ... rebuilt=1`、`character controller PASS ... maxGroundError=0`、`character serialization PASS | captured=1`；`21/21 PASS`，日志 `20261101-234432`。B-08 下一项；雕刻仍为运行期状态。

## D-44：B-08（地形内容资产与大规模材质）
- `TerrainMaterial` 最多 4 个 Set2 材质集；set 0 启动加载根目录，set 1~3 按需 `setN/`，16 PNG 不齐全套程序化回退。
- `DescriptorManager` 分配 4 个 terrain descriptor set；`TerrainConfig::materialSetIndex` 序列化，`geometryEquals()` 忽略材质切换。
- 渲染按相邻地形实体材质集变化重绑 Set2，统计 `terrainMaterialBatches`/`Switches`/`Fallbacks`；Inspector 增加材质集下拉。
- 实测 `[Terrain] material sets PASS | resident=4 max=4 external=1 fallback=3 serialized=1`；`21/21 PASS`，stderr 0，`errors=0 warnings=0`，日志 `build/validation/20261102-001837`。
