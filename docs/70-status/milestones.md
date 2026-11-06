---
id: status/milestones
title: 里程碑与实测基线
type: status
scope: 空
read-when: 对照历史进度、或需要"已知良好基线数字"
size: ~930 行
updated: 2026-11-06
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
| 回归 | 38 项全 PASS，stderr 全 0 字节 | `tools/Validate.ps1` |
| 验证层 | `errors=0 warnings=0` | 每次运行退出打印 |
| syncval | 0 errors / 0 warnings | `--validation-features sync` |
| 场景 | 103 子网格 + 天空盒；CPU 路径 `draw calls 104`，F1-b 合并模式 `draw calls 2`（1 次 multidraw + 球体仍走旧路径）（相机朝向 Sponza 时；无头自检的默认视角由 `config/engine.json` 决定） | `[Perf]` 行 |
| 三角形 / 顶点 | 270459 / 811377（`vertices` 实际统计的是索引数） | 同上 |
| 描述符绑定 | `desc binds 2`（Set0/1/2 一次 + Set3 实例 SSBO 一次；后者由所有图形管线静态使用，GPU 批关闭时也要绑） | `[SelfTest] telemetry` / `[Perf]` 行 |
| 帧率 | `--selftest 300` 约 240~260 FPS（Debug，随相机/场景内容变化） | 同上 |
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

> D-8～D-24 的逐轮细节已归档到 [milestones-archive](milestones-archive.md)；
> 本页保留 D-21 起的近期里程碑与当前实测基线。
## D-25：阶段 7 第三轮（chunked submeshes + 逐块剔除）
- **配置**：`TerrainConfig` 新增 `chunkGridX/chunkGridZ`，默认 2×2，校验 `[1,4]`；属于几何参数，修改后需要 Generate。地形 JSON version 升到 4。
- **网格**：`TerrainMeshBuilder` 仍使用一份共享顶点缓冲，但索引按 chunk连续重排；每个 `TerrainSubMesh` 记录 `firstIndex/indexCount`、chunk 坐标和局部 AABB。边界顶点共享，所以同 LOD chunk 没有裂缝。
- **渲染**：`SubMesh` 新增可选局部 bounds；`GeometrySystem` 通过`SubMeshCuller` 对每个 chunk 做世界 AABB -> frustum 测试，不可见 chunk直接跳过 draw。`RenderTelemetry` 增加 `subMeshesCulled`，内置 Stats 显示`Chunks culled`。
- **测试**：- `TerrainSelfTest` 覆盖非法 chunk grid、chunk 数、索引区间连续覆盖与
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
## D-45：代码/文档审查修复（异步任务生命周期 + Set2 描述符 + 工具链）
- **P0 use-after-free**：`Scene::processTerrainGeneration()` 在任务被取代或实体销毁时
  直接 `erase` 掉 `shared_ptr`，而 JobSystem worker 仍用裸指针写 `mesh`/`heightField`/
  `error`（多 chunk 走 CPU builder 时可达）。新增 `retiringTerrainBuilds`：句柄未完成的
  任务先摘入 retiring 列表，`drainRetiringTerrainBuilds()` 在句柄完成后回收；
  `waitForTerrainGeneration()` / `cancelPendingTerrainGenerations()` 同步等待 retiring 任务。
- **P1 描述符**：`createTerrainLayout()` 的 16 个 binding 补上 `UPDATE_AFTER_BIND |
  PARTIALLY_BOUND` 与 `UPDATE_AFTER_BIND_POOL`；此前运行期切换材质集调用
  `updateTerrainTextures()` 会触发 `VUID-vkUpdateDescriptorSets-None-03047`。
- **P2**：`TerrainComputeGenerator::findRequest()` 不再跳过 abandoned 槽位，`poll()` 能
  真正返回 "GPU terrain request was abandoned"，且只在 fence 完成后回收槽位；
  `collectFinished()` 化简为 `status != VK_NOT_READY`。
- **工具链**：`tools/Validate.ps1` 对 `$null` stdout 做显式归一化，避免 PowerShell 5.1
  把 `[string]$null` 留成 `$null` 后 `IndexOf` 抛 `InvokeMethodOnNull`、整个套件中断且
  不报 FAIL。
- **文档**：docs/README 补 29 个 shader、`90-editor/` 类型与目录树；8 篇编辑器文档
  `size` 改为 `~N 行`；根 README 已知边界去掉"编辑器"；doc-map 补 TerrainMaterial 行；
  TD-11 澄清流送 chunk 仍走 GPU mesh。
- **回归**：`--terrain-test` 新增 `[Terrain] build task retirement PASS`：临时禁用 GPU
  高度场让 `requestRegenerateTerrain()` 同步提交 CPU job，紧接着 `destroyEntity()`，
  断言任务仍被跟踪（`tracked=1`）且 `waitForTerrainGeneration()` 后 drain 干净。
  Scene 为此暴露 `getPendingTerrainBuildCount()` / `getRetiringTerrainBuildCount()` /
  `setTerrainGpuHeightEnabled()` 三个自检钩子。
- **窗口**：无人值守运行默认不显示窗口（`--hidden`），另有 `--minimized` / `--visible`；
  启动打印 `[Window] presentation=hidden|minimized|visible`，`terrain-render` 断言 hidden。
  `--minimized` 先隐藏创建、渲染器就绪后再 show+iconify（早 iconify 会让客户区变 0x0、
  G-Buffer 附件创建失败），且与自检/截图组合时退化为 hidden 以免不出帧。
- **文档拆分**：地形材质/Set2 独立为 `docs/30-systems/terrain-materials.md`，
  `terrain.md` 600→555 行，给后续内容留出余量。
- **实测**：`--selftest 20 --empty-scene --terrain-test` PASS（`errors=0 warnings=0`，
  `async GPU upload PASS ... gpuMesh=1`、`material sets PASS ... resident=4`、
  `streaming chunk test PASS ... gpuMesh=9`）；`tools/Validate.ps1 -Incremental -SkipSetup`
  21/21 PASS，日志 `build/validation/20261102-101809`。
## D-46：实现 `RenderGraph::setPassEnabled`（跨帧 Pass 开关）
- **问题**：`passes` 每帧重建（`beginFrame` 与 `endFrame` 末尾都会 `reset + clear`），
  旧实现遍历当前帧的 `passes`，帧间调用时列表为空 → 静默无效（tech-debt TD-13 /
  weak-points 原第 10 条）。
- **实现**：新增与 Vulkan 无关的 `PassSwitchboard`（精确匹配 / `*` 前缀匹配 / 最近设置优先 /
  未命中诊断），`addGraphicsPass`、`addComputePass` 注册时 `resolve` 重放；同帧内已注册的
  Pass 只按命中的规则重放，避免覆盖别处 `setEnabled(false)` 的反例写法。新增
  `clearPassOverrides()` / `isPassEnabled()` / `getPassSwitchboard()` 与
  `getLastFrameExecutedPasses()` / `getLastFrameSkippedPasses()` / `wasPassSkippedLastFrame()`。
- **可观测**：未命中规则在 `endFrame` 末尾打印一次（stdout，不破坏 stderr 0 字节基线）；
  簿记清单让"关掉的 Pass 真的没录"可断言，也给调试面板留了口子。
- **回归**：`[RenderGraph] setPassEnabled selfTest PASS | prefix + exact + last-wins + diagnostic`；
  `[RenderGraph] pass bookkeeping selfTest PASS | executed=14 skipped=0`（`--no-bloom` 为 7）；
  `--selftest 120` 245 FPS、验证层 0/0；21 项 `Validate.ps1` ALL PASS
  （`build/validation/20261102-132535`）；DocCheck PASS。
- **文档**：rendergraph.md 新增 §2.2（跨帧语义 + 禁用会跳过屏障声明的代价与原错误串）；
  tech-debt 增 TD-13 并从"未关闭"表移除；weak-points 删除该条；test-matrix 第 3 项补标记。
## D-47：还清 TD-14（writeDepth 隐式改写）并修正过期技术债
- **TD-14**：`RenderGraph::Pass::writeDepth` 删掉"`depth==0 && stencil==0` 视为未设置、
  静默改成 `{1.0, 0}`"的两行。清屏值一律由调用方显式给出：`GeometryPass` 本来就传
  `clearDepth.depthStencil = {1.0f, 0}`，`EditorOverlayPass` 走 `LOAD`，所以这次改动零行为
  变化；引入阴影贴图时不会再被"想清 0 却被改写成 1.0"坑到。头文件注释同步说明。
- **过期债清理**：`releaseMaterialSet()` 从 B-06 起就有生产调用方
  （`Scene::clearEntities()` 先归还槽位再销毁实体），tech-debt 原第 3 条已不成立，删除。
- **回归**：`--selftest 120` 236 FPS、验证层 0/0、pass bookkeeping `executed=14 skipped=0`；
  21 项 `Validate.ps1` ALL PASS；DocCheck PASS。
## F1-a：GPU 驱动提交第一刀（SceneGpuBatch）
**问题**：几何提交仍是 `for (entity : visibleEntities)` 逐实体逐子网格的 CPU 决策，实例数一多就成瓶颈，也是 F6（阴影/SSR）的前置。

**实现**：
- `shaders/scene_cull.comp`：64 线程/组，6 平面视锥剔除，写 5-uint indirect 命令 + 每模型计数器；
- `src/Render/SceneGpuBatch.{h,cpp}`：224 字节 `InstanceRecord` 摊平、双缓冲槽 SSBO、容量不足时 device idle 后重建、`vkCmdDrawIndexedIndirectCount` 按模型提交；
- `SceneGpuCull` 无附件 pass 挂在 `Geometry` 之前；`GeometrySystem` 跳过交给 GPU 批的普通网格，并在整帧开头补绑 set 3（否则旧路径绘制触发 `VUID-vkCmdDrawIndexedIndirectCount-None-08600`）；
- `gpuScene` 标志进 `SimplePushConstantData`，`mrt.vert/frag` 从实例缓冲取矩阵与材质；
- 阈值 `gpuSceneMinInstances = 256`：小场景仍走 CPU 路径，`--gpu-scene` 可强制、`--no-gpu-scene` 可关闭。

**可观测**：`[SceneGpu] GPU batch ready | instancesPerFrame=256 models=32 drawIndirectCount=1`、`[SceneGpu] indirect batch | models=1 instances=103`；遥测 `drawCalls/triangles/vertices` 与 F1 前基线一致（104 / 270459 / 811377）。

**回归**：`--selftest 120` errors=0 warnings=0；`Validate.ps1` 新增 `f1-gpu-scene`；像素级比对（相机 `[0,6,45]`）GPU 路径与 CPU 路径 `max diff = 0`。
**性能**：300 帧多轮 default 248.1/248.0、cpu 237.7/257.1、forced 248.1/228.2 —— 噪声大于差异，当前 103 实例下两条路径相当，收益留给实例数上来之后。
## F1-b：合并顶点/索引缓冲（一次 multidraw）
**问题**：F1-a 按模型分组、每个子网格一条间接命令，103 个子网格仍是 103 条命令；multidraw 的收益要等命令能跨模型才拿得到，而间接命令本身不带顶点缓冲绑定。
**实现**：`Model` 为可批模型保留完整 CPU 顶点/索引（地形不留）；`ensureMergedMesh()` 按模型集合算 FNV 签名（指针 + 顶点数 + 索引数），顶点/索引分别拼进一份 device-local 缓冲、索引保持原样、每模型记 `baseVertex` 并填进间接命令的 `vertexOffset`（等价 `vkCmdDrawIndexed` 语义），所有实例共用 `ranges[0]`/`counts[0]`，一条 `vkCmdDrawIndexedIndirectCount` 画完；重建走自建 `uploadPool` + staging + 单次命令（屏障 TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ|INDEX_READ）。F1-d 起回退直接落到 CPU 路径（`--no-merged-buffers` 强制）。
可观测：`[SceneGpu] merged buffers built | vertices=192496 indices=786801 vertexMB=8 indexMB=3`；相机朝向 Sponza 时遥测 `draw calls 2 / triangles 270459 / vertices 811377`（合并前 104）。 **回归**：`Validate.ps1` 新增 `f1b-merged-buffers` 与 `f1b-merged-off`，全套 24 项 ALL PASS；三路像素比对（合并 / 每模型 / CPU）相机 `[0,6,45]` 下 `max diff = 0`。
**性能**：同模式轮间波动约 24 FPS，仍以噪声为主；确定收益是 draw call 104 -> 2，不是本机 FPS。
## F1-c：按组实例化 + 材质排序 + Hi-Z 遮挡剔除
**实现**：分组键改成 `(模型, 子网格)`（实例连续，`instanceCount = N`，一条命令画完所有实例）；去掉 counters 缓冲（组自己那 5 个 uint 直接写，被剔除写 0）；组按并集世界 AABB 剔除；合并模式按 `materialSlot` 排序、非合并按 `(model, materialSlot)`；包围盒缓存改用 `Model::getGeometryId()`（600 帧/64 条淘汰）；Hi-Z 金字塔从 `GBuffer.Position` 反算线性视深度（`R32_SFLOAT`、`floor(log2(max(w,h)))+1` 级、布局恒 GENERAL、手动屏障），`HiZBuild` 挂在 `Geometry` 之后写另一槽（错帧一帧），剔除时投影成屏幕矩形、按矩形像素选级、4 角 `textureLod` 取最大值，`nearest > farthest + 0.05` 判遮挡；`--no-hiz` 可关。
**可观测与回归**：`[SceneGpu] cull stats | groups=103 visible=103 culled=0 instances=103 hiz=1 hizLevels=11 hizEnabled=1 hizTests=103 hizCulled=0`（每 60 帧一行）；新增 `f1c-hiz-off`，全套 25 项 ALL PASS、stderr 0 字节；`--resize-test` / `--render-scale 0.5` 验证层 0/0（顺带修掉 G-Buffer 重建后遗留旧 `VkImageView` 导致描述符写入崩溃、录制期 `vkUpdateDescriptorSets` 让命令缓冲失效两个 bug）。
**Hi-Z 有效性**：当前 `hizCulled = 0` 是正确结果 —— Sponza 的 103 个子网格各自覆盖整栋建筑一大块，组级保守剔除几乎不命中；把 `kOcclusionBias` 临时改成 -100000 可验证全链路（`hizTests=103 hizCulled=103`、`visible=0`）。收益等大量小实例（粒子/植被/体素 chunk）。
## F1-d：稳定帧快速路径 + 同步校验 0 错误
**问题**：从 F1-a 起每帧都重新摊平实例（世界矩阵 + 每组并集 AABB），静态场景实测 439µs/帧；`--validation-features sync` 跑出 180 条同步危害，全部来自 Hi-Z 的跨帧读写与屏障。
**实现**：结构签名（FNV-1a：entity、`sourceRevision`、材质槽、材质指针、`getGeometryId()`、子网格数、局部 AABB 六个 float、`gpuMergedBuffers`）；签名命中就跳过 `buildInstances()` 与 `instances/groups` 上传，只写 240 字节 `frameData` 再 dispatch（F1-e 起 256 字节）；每槽位记 `(signature, capacityGeneration)`，容量重建后两槽都重传。Hi-Z 起始屏障改成 `GENERAL→GENERAL` + `srcAccess = SHADER_READ`（等上一帧剔除的采样），逐级屏障给下一级 `SHADER_READ`。
**可观测**：`[SceneGpu] cull stats | ... stable=60/60 rebuilds=0 flattenUs=9`（重建帧 439）；`--validation-features sync` → `[Validation] errors=0 warnings=0`。
**回归**：新增 `f1d-stable-path`（要求 `stable=`/`rebuilds=`/`flattenUs=`）与 `f1d-sync-validation`（sync 0 错误），全套 **27 项 ALL PASS**、stderr 0 字节。
## F1-e：屏幕空间误差剔除 + 外部实例 + 跨模型单条提交
**实现**：① 删掉非合并模式的逐模型 bind 中间路径，合并缓冲成为唯一 GPU 提交路径（`useGpuPath()` 要求 `mergedActive`，否则整帧回 CPU）；② `FrameData` 加 `vec4 screen`（阈值像素、视口高、投影 `[1][1]`），`scene_cull.comp` 用 `radius * 0.5 * H * P11 / distance < 阈值` 做实心 SSE 剔除，`stats[2]` 记 `screenTests/screenCulled`；③ 新增 `ExternalInstance { model, subMeshIndex, transform, materialSlot }` 与 `setExternalInstances()`，外部实例与 ECS 实例共用分组 `(模型, 子网格)`、合并缓冲与同一条 indirect 命令，结构签名混入它们。
**可观测**：`[SceneGpu] external instances | count=<n> source=debug-injection`、`cull stats` 尾部 `screenTests=.. screenCulled=.. external=..`；`--gpu-scene-min-px 400` 时 `screenTests=103 screenCulled=103`（整屏剔空），`--gpu-scene-external 4` 时 `instances=107`。
**回归**：新增 `f1e-screen-space` 与 `f1e-external-instances`，全套 **29 项 ALL PASS**（`build/validation/20261102-160351`）；`--no-merged-buffers` 仍走 CPU 路径 `104 / 270459 / 811377`。
**边界**：SSE 只做剔除、不是 LOD 选级（模型没有 LOD 链）；外部实例与 ECS 共用分组，同子网格不同材质会归到一组；粒子每帧变化会击穿结构签名 —— 数据确实变了，这是预期行为。
## F2-a：流送世界模式（可持续运行的载体）
**问题**：流送地形此前只在 `--terrain-test` 冒烟块里创建（17x17、半径 1），正常运行与编辑器里没有流送世界 —— 「无限世界」缺一个能跑的载体。
**实现**：`--streaming-world [radius]` 在构造期 `createStreamingTerrain(seed 9001, 33x33, chunk 32, heightScale 12, lod 3, 半径 1..8, maxLoadsPerFrame 4)`；`--streaming-fly [speed]` 让相机沿半径 220 的圆周前移（yaw = -a，与 forward=(sin y,0,cos y) 一致），把加载/卸载从「一次性」变成「持续发生」；`Scene` 新增 loaded/unloaded 累计与 lastFrame 增量计数。半径 3 = 49 个 chunk 实体，低于地形 GPU 选择的 64 slot 上限。顺带把配置自检的临时目录改成「建目录 + 写探针」并在不可写时退回工作目录 `.tiny-config-selftest/`（只读 TEMP 的受限环境里不再假失败）。
**可观测**：`[Streaming] world ready | radius=..`、`[Streaming] chunks=<n> loaded=<n> unloaded=<n> maxUpdateMs=<ms>`；静态半径 2 → `chunks=25`；飞行 150 帧 → `chunks=35 loaded=54 unloaded=19`（带 unloadPadding 的滞回环），验证层 0/0。**回归**：新增 `f2-streaming-world`（要求 `chunks=25`）与 `f2-streaming-fly`（要求 `unloaded=`），全套 **31 项**。
## F2-b：多 chunk GPU 合批（arena + 实例表 + 一条 indirect）
**问题**：流送世界的每个 chunk 是一份独立 Model，渲染时逐 chunk `bind` + 逐 chunk `drawIndexedIndirectCount`，半径 3 就是 49 次绑定与 49 次提交。
**实现**：新增 `TerrainMeshArena`（96 槽，每槽 4096 顶点 / 20480 索引），可见 chunk 的整块 VBO/IBO 惰拷贝进槽位；世界矩阵不烘进顶点，改为每帧写进 set 3 的 chunk 实例表（64 行 x 224B，复用 `InstanceRecord` 布局），间接命令带 `firstIndex = 子网格基址 + arena 索引基址`、`vertexOffset = arena 顶点基址`、`firstInstance = 实体槽号`；筛选后干涉 `terrain_lod.comp`、`mrt.vert`、`mrt.frag` 各自读取该行。绘制侧先 `vkCmdFillBuffer` 清空 2048 条命令区（64 实体 x 32 命令），只发一条 `vkCmdDrawIndexedIndirect`；CPU 间接游标在 GPU 选择启用时从 2048 起。任一可见 chunk adopt 失败则整帧回退（全有或全无），`--no-terrain-batch` 强制回退。
**可观测**：`[Terrain] mesh arena ready | slots=96 vertexMB=16 indexMB=7`、`[Terrain] batched draw | chunks=<n> arenaSlots=<n> commandSpan=2048`、`[Terrain] terrain batching disabled | falling back to per-chunk draws`；`--streaming-world 2` 遥测从 `draw calls 13 / groups 12 / binds 2` 变为 `draw calls 2 / groups 1 / binds 3`，`triangles 22464 / vertices 67392` 不变。
**回归**：新增 `f2b-terrain-batch` 与 `f2b-terrain-batch-off`，全套 **33 项**；批量 vs `--no-terrain-batch` 两路截图逐像素一致（忽略 ImGui 面板 `max diff = 0`）；`--streaming-fly 60` 150 帧 `chunks=35 loaded=54 unloaded=19`、0/0；`--validation-features sync` 0/0。
**坑**：实例表的 `misc` 是 `vec4` 浮点，写 `uint` 位模式 1 会变成 1.4e-45，着色器取整后 terrainMode 变 0、整块地形走棋盘格占位——必须写 `1.0f` 并在着色器里 `uint(x + 0.5)`。
## F2-c：浮动原点（场景坐标不再随相机走远而抖）
**问题**：世界坐标一直是 `float`，相机走到万米量级后矩阵与深度开始抖；流送世界也没有「原点会移动」的概念。
**实现**：registry 里所有 `TransformComponent` 只存局部坐标，绝对位移记在 `Scene::worldOrigin`（`glm::dvec3`）。相机局部水平距离超过 `originRebaseThreshold`（默认 1024）时，位移吸到整块 chunk（`floor(local / chunkSize) * chunkSize`），一次性平移所有 Transform、调 `CameraController::offsetWorldPosition` 把平滑状态（`smoothPosition` / `focusPoint` / `lastAppliedPosition`）一起挪、重排流送 chunk 的 `chunkX/Z` 与两张 key 表，`worldOrigin += delta`。地形噪声锚点 `sampleOrigin` **保持绝对**（rebase 后新建 chunk 写 `worldOrigin + 局部中心`），`HeightField::sample()` 本来就不读它，所以查询/碰撞/接缝都不受影响。`--streaming-fly` 从圆周改成直线巡航：圆周半径恒定会让 rebase 每帧触发一次。
**可观测**：`[Origin] rebased | delta=(x, y, z) origin=(X, Y, Z) rebases=<n>`、`[Origin] stats | rebases=<n> origin=(X, Y, Z) maxLocal=<f> threshold=<f>`；自检断言「飞行距离超过两倍阈值时必须 rebase」「`maxLocal` 不得超过阈值 +128」，关闭 rebase 时这两条自动跳过。
**回归**：新增 `f2c-origin-rebase` 与 `f2c-origin-off`，当时全套 **35 项**；`--streaming-world 2 --streaming-fly 60 --origin-threshold 64` 240 帧 4 次 rebase、`chunks=35`；600 帧 13 次 rebase、`loaded=138 unloaded=103`、`chunks=35` 稳定；rebase 与不 rebase 两路截图逐像素对比 `max 14 / mean 0.9`（只差 float 光栅化）、地形几何一致；`--validation-features sync` 0/0。
## F2-d：流送半径 8 → 12（625 块）与每帧生成预算
**问题**：流送半径上限 8，地形 GPU 选择只有 64 个实体槽、命令区 4096、arena 96 槽，半径超过 3 就开始回退；单帧生成没有预算，慢帧会一次拉起整环。
**实现**：`kMaxTerrainStreamingRadius` 8 → 12（625 块）、`kMaxTerrainStreamingLoadsPerFrame` 8 → 16；`kTerrainGpuMaxEntities` 64 → 384、`kTerrainGpuCommandsPerEntity` 32 → 16、`kTerrainIndirectCapacity` 4096 → 12288；`TerrainMeshArena` 96 → 384 槽、单槽 2048 顶点 / 12288 索引（约 136 KB，合计约 51 MB）；新增 `TerrainStreamingComponent::maxUpdateMs`（默认 2 ms）与 `--streaming-budget`。
**可观测**：`[Terrain] GPU selection ready | entities=384 records=6144 commandsPerEntity=16`、`[Terrain] mesh arena ready | slots=384 vertexMB=33 indexMB=18`、`[Streaming] chunks=<n> ... budgetHits=<n>`；`--streaming-budget 0.2` 时 budgetHits 明显非零。
**回归**：新增 `f2d-streaming-radius`（半径 8 → chunks=289）与 `f2d-streaming-budget`，全套 **37 项**；半径 2/4/8/12 = 190/164/117/68 FPS（625 块瓶颈在每 chunk 的 CPU 簿记）；半径 8 飞行 600 帧 `chunks=323 loaded=599 unloaded=276`、13 次 rebase、0/0。
## F2-e：地形拾取改走高度场（不再保留每 chunk 的 CPU 顶点副本）
**问题**：每个地形 chunk 都额外保留一份全分辨率 `pickingPositions/pickingIndices`（33x33 约 38 KB、257x257 约 1.3 MB），只为编辑器 raycast 服务；这与「真正无限世界」的每 chunk 固定开销直接冲突。
**实现**：`Scene::raycastTerrain()` 不再要求 `MeshComponent`，改为沿射线在 chunk 局部空间做 XZ slab 裁剪、按 `min(cellX, cellZ)`（下限 0.25）步进采样 `HeightField::worldHeightAt()`，符号翻转后二分 16 次求交点；GPU mesh / 同步生成 / 流送三条路径都向 `Model::createFromGpuMesh()` 传空 picking 数组；`TerrainGpuMeshBuilder` 删除 `buildPickingMirror()` 与 `pickingFuture`（含 `poll/collectFinished/waitAll` 里的等待分支）；编辑器流送 chunk 拾取自检改调 `scene.raycastTerrain()`。
**可观测**：`[Editor] streamed terrain PASS | ... picking=OK`；`[Terrain] sculpt PASS`、`character controller PASS | grounded=1 maxGroundError=0` 不变。
**回归**：新增 `f2e-terrain-picking`（跑 `TinyEditor.exe --selftest 5 --empty-scene`，`Validate.ps1` 增加 `Exe = "editor"` 支持与编辑器可执行发现），全套 **38 项**；批量 vs `--no-terrain-batch` 截图仍 `max 0`；半径 8 飞行 240 帧 4 次 rebase、`chunks=322 loaded=400 unloaded=78`、0/0；`--streaming-budget 0.2` 下环未填满属预算生效，自检改为「有预算命中时不强制满环」。
**顺带**：`SceneSerializer::saveToFile()` 打开失败时带上 `errno` 与 `strerror` 文本；编辑器 streaming 自检在 TEMP 写入被拒（本机 errno 13）时回退到仓库内 `.tiny-streaming-selftest/`（已 gitignore）；`EngineConfig` 存档改存绝对坐标（局部 + `worldOrigin`），关闭 TD-22，飞行 + rebase 后不再把 ±chunk 整数倍偏移写回 `config/engine.json`。
## F2-f：地形 chunk 不再各持 Model/VBO/IBO（几何只进共享 arena）
**问题**：F2-b 的 arena 是渲染侧按可见 chunk 惰拷贝的，chunk 仍然各持一个 `Model` 与自己的 device-local VBO/IBO；半径 12 的 625 块就是 625 份缓冲对象，与「每 chunk 固定开销」的无限世界目标冲突。
**实现**：arena 所有权移到 `Scene`（`Scene::init()` 建、`getTerrainMeshArena()` 暴露），`TerrainMeshArena` 新增 `acquire(vertexCount, indexCount, slot)` 与 `copyInto(cmd, slot, srcVertex, srcIndex)`；地形网格一落地就在一次性命令缓冲里 acquire + copy + `Model::releaseGpuBuffers()`，`Model` 只留子网格/包围盒/UV，实体销毁时归还槽位。`GeometrySystem` 改为持有 `TerrainMeshArena*`，地形实体一律绑 arena 的 VBO/IBO 并用 `firstIndex + arenaIndexBase` / `vertexOffset = arenaVertexBase`；没抢到槽位的 chunk 保留自己的 buffer 走旧路径（半径 12 的 625 块 > 384 槽时不会丢几何）。
可观测：`[Terrain] batched draw | chunks=12 arenaSlots=25 commandSpan=6144`；半径 8 飞行 600 帧 `arenaSlots` 在 305~323 之间回落（槽位回收正常），`Origin` 13 次 rebase，0/0。 **回归**：新增 `f2f-arena-slots`；全套 **39 项**；`--gpu-scene` 与地形合批同时开启时发现并修掉「set 3 被地形换成 chunk 实例表、场景合批没绑回来 → Sponza 整片消失」的 bug（三角形数与 draw call 数都对，只是位置错），修后 `--gpu-scene` / `--no-terrain-batch` / `--no-gpu-scene` 三组 A/B 截图均 `max diff = 0`。
## F2-g：地形 arena 改成按实际尺寸分配（bump + 空闲区间合并）
**问题**：F2-f 的 arena 是 384 个固定槽（2048 顶点 / 12288 索引），而一个 33x33 / 3 级 LOD 的 chunk 实际只用约 1620 顶点 / 8544 索引，近三成空间放着不用，总量还被槽数卡死。
**实现**：`TerrainMeshArena` 换成两块 device-local 大缓冲（顶点 1.1M ≈ 46 MB、索引 5.9M ≈ 22.5 MB）+ 有序空闲区间表：`acquire()` 首次适配、`release()` 前后合并，`Slice` 记录 base/count；顶点分配成功但索引失败时回滚顶点区间。`TerrainComponent` 用 `arenaVertexBase/arenaIndexBase/arenaVertexCount/arenaIndexCount` 取代 `arenaSlot`，`Scene` 落地与销毁两侧直接搬 `Slice`。
**可观测**：`[Terrain] mesh arena ready | vertexMB=48 indexMB=22 vertexCapacity=1100000 indexCapacity=5900000`；`[Terrain] batched draw | chunks=12 arenaAllocs=25 commandSpan=6144`；半径 8 飞行 600 帧 `arenaAllocs` 在 305~323 回落，13 次 rebase，0/0。**回归**：39 项全 PASS；三组 A/B 截图 `max diff = 0`；编辑器自检 `picking=OK rebuild=OK unload=OK`；sync 与 resize 各 0/0。顺带修掉 `Model` 自有 VBO/IBO 缺 `TRANSFER_SRC` 导致的 `VUID-vkCmdCopyBuffer-srcBuffer-00118`。
## F2-h：chunk 坐标 int64 + 采样锚点 double（流送半径上限 24） **问题**：半径 12 的 625 块是 int32 chunk key 与 float 绝对锚点能撑的上限，再放大就会出现量化与哈希碰撞。 **实现**：`Scene::TerrainChunkCoord{int64_t x, z}` + 64 位哈希替换 `int64_t` 打包 key，`TerrainChunkComponent::chunkX/Z` 与 rebase 的 key 重排全部改宽；`TerrainConfig::sampleOriginX/Z` 与 `HeightField` 成员改 `double`（噪声本身仍按 `float` 采样，保证 CPU/GPU 逐位一致），校验上限 ±1e12；arena 容量由 `Scene::createStreamingTerrain()` 按环尺寸算（`ring*1700` 顶点 / `ring*9000` 索引）并新增 `TerrainMeshArena::reconfigure()` 原地重建（渲染侧只持 `this` 指针）；GPU selection 槽位 384 → 1024、命令区 12288 → 32768。 **可观测**：`mesh arena resized | radius=24 chunks=2401 vertexCapacity=4081700 indexCapacity=21609000`、`[Streaming] chunks=2401 loaded=2401`、`draw calls 2`、`[Validation] errors=0 warnings=0`；半径 2/12/16/24 = 190/70/36/17 FPS。 **回归**：新增 `f2h-large-radius`；全套 **40 项**；三组 A/B 截图 `max diff = 0`；编辑器自检、sync、resize 各 0/0。
## F2-i：澄清 CPU 簿记的真实成本（Release 208 FPS）+ 两处热点
**问题**：F2-h 记录「半径 24 只有 17 FPS」，据此把「每 chunk 的 CPU 簿记」列为头号债务；重新测量发现那是 Debug 构建的数字。
**实现**：新增 `--update-profile`（`Scene::update()` 五段耗时 genPoll / streaming / world / spatial / visibility，每 60 帧打印一次，关闭时零开销）；`updateVisibility` 从 `SpatialIndex::query(broadBounds)` 换成对 `MeshComponent` 的线性扫描（旧路径在小 cell 上超过 4096 个 cell 会退化成 `collectAll()`，按 cell 重复展开再 `sortUnique()`）；`updateTerrainStreaming` 加稳态早退（相机没换 chunk、环已填满、无待办时整趟跳过）。
**可观测**：Release 半径 24（2401 块）= **208 FPS**、`[Update] avg us | genPoll=0.4 streaming=0.4 world=0.06 spatial=0.06 visibility=61.5 | maxTotal=78.1`；同一份代码 Debug 的 `visibility` 是 2.08 ms；流送扫描稳态 70 µs → 0.41 µs。
**回归**：新增 `f2i-update-profile`；全套 **49 项**。

> 编辑器侧里程碑（E1 起）见 [editor/milestones](../90-editor/milestones.md)。

## B1 剩余：启动失败可读 + 窗口按工作区收敛

- 问题：[UX-04](../90-editor/ux-audit.md) 双击启动失败只有 `std::cerr`，用户什么都看不到；
  [UX-19](../90-editor/ux-audit.md) 窗口固定 1920x1080，1366x768 与 150% 缩放下状态栏被推出屏幕。
- 实现：新增 `src/Core/StartupLog.h/.cpp` 的 `tiny::writeStartupErrorLog(stage, message, directory = "")`，
  `main.cpp` 的 catch 把错误写进**可执行文件旁**的 `startup-error.log` 并在 stderr 打印路径；
  `Device::reportDeviceFault` 在驱动没有 `VK_EXT_device_fault` 时打印原因/下一步并写同一文件；
  `FirstApp::computeWindowSize` 按工作区 92% 收敛，`initWindow` 打印 `[Window] sizing | workarea=... -> WxH`。
- 可观测：`[Startup] error log selfTest PASS`、`[Window] fit selfTest PASS | 1280x720 -> 1177x662`。
- 验证：回归 54 项（新增 `b1-window-fit`）。
- 2026-11-05 补上 UX-04 的最后一段：交互式启动失败弹原生 MessageBox，`--hidden` / `--selftest` 等无人值守路径保持静默；`b1-window-fit` 增加正文断言。详见 [E-P1](#e-p1启动失败原生-messagebox2026-11-05)。

## F6-a：级联阴影（3 级 CSM + PCF）
**实现**：`src/Render/ShadowPass.h/.cpp` 三级联（2048²、2D 数组 3 层）+ 纹素吸附 + AABB 8 角点剔除 + 3x3 PCF；`GlobalUbo` 尾部追加 `shadowMatrices[3]`/`shadowCascadeSplits`/`shadowParams`，全局 set 加 binding 11（`sampler2DArray`）；`--no-shadows` 与渲染面板「阴影」分组可关。
**关键坑**：`glm::ortho` 在右手视空间 + `GLM_FORCE_DEPTH_ZERO_TO_ONE` 下把场景压到负深度，阴影图全空 —— 改手写正交矩阵（`z_ndc = (-z_view - r)/(2r)`）；单点包围球剔除会把整栋 Sponza 剔掉，改 8 角点 AABB。
**已知限制**：默认场景阴影偏淡（IBL 占主导，方向光只占约 5% 亮度；此前 README 的已知边界是「无阴影」）；室内机位 on/off 差异 `mean 15.6 / gt5 1,020,849`。
**回归**：新增 `f6-shadows`，全套 **62 项**。

## F3：方块体素世界（挖掘 / 建造 / 站立）
**问题**：引擎没有任何体素代码；roadmap 的 F3 原写 marching cubes / surface nets，但「挖矿盖房」要的是整齐方块，等值面挖出来是圆滑洞。
**实现**：`src/Voxel/`（`BlockId` + `BlockInfo` 调色板、`Chunk` 32³、`buildMesh` 面剔除 + 顶点色×面明暗，复用 `terrain::TerrainMeshData` 上传路径）；`Scene::createVoxelWorld / setVoxelBlock / raycastVoxelWorld`（chunk AABB 粗筛 + Amanatides-Woo DDA）/ `sampleVoxelGroundHeight`；边界格连带重建邻块，旧 Model 走 `retireModel` 延迟释放；材质用 `getDefaultWhiteTexture()` 避免棋盘格；`--voxel-world [r]`；编辑器运行模式（左键挖 / 右键放）+ `VoxelEditCommand` 撤销；`queryGroundAtWorld` 叠加体素分支，角色能站在方块上。
**可观测**：`[Voxel] selfTest PASS | face culling 6/10/6, boundary continuous, solid=12090`、`[Voxel] scene selfTest PASS | chunks=9 hit=(16,10,16) dugDeeper=1 restored=1 groundY=11 characterLanded=1 placedOnFace=1`；`--voxel-world 1` 截图方块地形顶面草皮、侧面土层/石层、无缝。
**已知限制**：体素世界不进存档（`buildFromDescription()` 会清掉）；无 LOD / 贴图集（贪心合并 + 逐顶点 AO 已在 F3-e 完成）；网格在主线程同步生成；透明方块没有逐面排序。
**回归**：新增 `f3-voxel`；全套 **63 项**。

## F3-d：体素世界持久化（seed + 差分）
**问题**：`captureDescription()` 只序列化地形与普通实体，`buildFromDescription()` 的 `clearEntities()` 会把体素世界连 chunk 一起清掉 —— 挖好的房子保存/重载就没了（tech-debt #19）。
**实现**：`SceneDescription` 增 `voxelWorlds`（seed/radiusChunks/baseY + 每 chunk 的 `edits` 四元组）；`captureDescription` 按 seed 重新生成参考 chunk 逐格比对出差分；`buildFromDescription` 重建世界后逐格 `set`，每块只 `rebuildVoxelChunk` 一次；`SceneSerializer` 版本 2 读写 `root["voxelWorlds"]`，老文件按空处理；编辑器自动认领世界时补 `!registry.valid(...)`。
**可观测**：`[Voxel] scene selfTest PASS … persistCapture=1 persistRestore=1`、`[Editor] voxel edit selfTest PASS | … capture + json round trip`。
**回归**：新增 `f3-voxel-persist`；全套 **65 项**。

## F3-e：贪心面合并 + 逐顶点 AO

**问题**：逐面输出的方块网格把相邻同色面拆成一堆独立四边形，`--voxel-world 1` 就有 33.8 万三角形；方块之间没有遮蔽感，阶梯处死板。

**实现**：`src/Voxel/VoxelChunk.cpp` 的 `buildMesh` 改为六方向 x 32 层切片的贪心合并（mask 记录方块 id，先扩宽再扩高，同材质矩形输出 1 个 quad）；新增 `FaceAxis` / `kFaceAxes` / `solidAt` / `vertexAo`，AO 按空气侧两边与对角取 `level = (s1 && s2) ? 0 : 3 - (s1 + s2 + cc)`、系数 `0.55 + 0.15 * level`，乘进顶点色。

**可观测**：`[Voxel] selfTest PASS | face culling + greedy 6/6/6/6, boundary continuous, solid=12090`；`--voxel-world 1` 的 triangles 338299 -> 275641（约 -18.5%），draw calls 不变。

**回归**：`f3-voxel`（engine 自检串已更新）。

## F3-f：程序化体素贴图集（方案 A）

**问题**：体素只有一张 16 色的方块色板，放大了看是纯色平面，挖矿/建造缺少材质细节。

**实现**：`MaterialSystem::fillVoxelAtlas()` 启动时程序化生成 256x256 `VK_FORMAT_R8G8B8A8_SRGB` 图集（4x4 格 x 64px，九种方块，含噪点/石纹/木纹/砖缝/玻璃边框）；顶点 `uv.x = tile * 64 + 面内跨度 u`、`uv.y = 面内跨度 v`（贪心矩形在图块内平铺），顶点色退化为 `面明暗 x AO` 灰度；`Material::shaderFlags` 写进 `metalRough.z`，`shaders/mrt.frag` 见到 `> 0.5` 就把 `fragUV` 解码成 `atlasUV`（`mod/floor` 定位图块、半纹素内缩防渗色，采样用 `textureGrad` 显式梯度；2026-11-04 修复格边界白线，见下方）。**注：2026-11-05 图集已扩为 512×512 + 32px edge-clamp gutter，见文末。**

**可观测**：`[Voxel] selfTest PASS | face culling + greedy 6/6/6/6, boundary continuous, solid=12090`（新增 UV 区间断言）；`--voxel-world 1` 画面出现草皮/土层/石层/砖墙纹理，triangles 仍 275641。

**回归**：`f3-voxel`。

## F3-g：体素异步生成（半径 8 → 12）

**问题**：`createVoxelWorld()` 在主线程逐块 `generate + buildMesh`，半径 8（289 块）起就明显卡顿，
半径上限也被 8 卡住。

**实现**：管理器和每块占位实体（只有 `VoxelChunkComponent`）先落地；生成与网格化交给 `JobSystem::submit`，
`Scene::update()` 每帧 `processVoxelChunkBuilds()` 回收、最多上传 4 块（`Model` 只在主线程建）；
`VoxelWorldComponent::building / pendingChunks` 记录进度，`Scene::waitForVoxelBuilds()` 供自检与存档重建同步。
半径上限改为 `Scene::kMaxVoxelRadius = 12`，`--voxel-world [r]` 同步。

**可观测**：`[Voxel] world created | seed=20251104 radius=12 chunks=625 baseY=0 async submitMs=70.7`（Debug）；
自检新增「作业全部回收后每块都挂 `MeshComponent`、`building` 归零」断言。

**回归**：`f3-voxel`（MustContain 增加 `async submitMs=`）。

## V-01：体素玻璃透明排序（GlassPass）

**问题**：`Glass` / `Leaves` 标成 `opaque = false`，但渲染仍按实体顺序进 G-Buffer 走延迟着色 —— 玻璃叠玻璃时只有最前面（或最后面）的一层可见，交叉墙面出现"后面先画"的暗边。

**实现**：`buildMesh` 遇到玻璃直接跳过；新增 `voxel::buildGlassMesh()` 逐面输出（只有邻格是空气才出面，玻璃-玻璃/玻璃-不透明贴面跳过），`VoxelChunkComponent` 存 `glassMesh/glassFaceCount`。新增 `src/Render/GlassPass`：Lighting 之后、Bloom 之前，只写 `hdrScene`（`LOAD`），深度 `LOAD` 但 `depthWrite = FALSE`、`compare = LESS_OR_EQUAL`，混合 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`；CPU 每帧按面质心到相机距离平方从远到近排序，索引写入按帧轮转的 host-visible 池，一次 `vkCmdDrawIndexed`。glass-only chunk 只分配材质槽、不需要 `Model`。`--voxel-glass-demo` 摆交叉玻璃墙并把相机拉近。

**可观测**：`[Voxel] glass demo placed | blocks=429`、`[Glass] sorted=879 drawcalls=1`；telemetry draw calls 106（+1）、desc binds 3（+1）；`[Validation] errors=0 warnings=0`；截图 `build/Debug/glass_demo.png`（叠放无黑边，近处玻璃后方地形可见）。

**回归**：新增 `f3-voxel-glass`；全套 **67 项** PASS；验证日志 `build/validation/20261104-162505`。

## 示例体素世界黑屏修复 + 运行模式 WASD 方向修正（2026-11-04）

**问题**：帮助 → 示例 → 体素世界（3×3 chunk）打开后视口全黑，状态栏 `绘制 0 / 三角形 0`，控制台只有 `[SceneGpu] cull stats | groups=9 visible=0 culled=0 instances=0`；运行模式进入后 `W/S/A/D` 的前后左右与视线方向不一致。

**黑屏根因**：`Scene::applyVoxelChunkMesh()` 把 `setModelBounds()` 的脏实体写进函数内局部的 `dirtySpatialEntities`，函数返回即丢弃。异步体素作业只在 `createVoxelWorld()` 末尾那次 `refreshTransformsAndVisibility()` 之后才落地，此时 transform 已不脏；`refreshSpatialIndex()` 没有该实体记录，建不出 `WorldBoundsComponent`，`updateVisibility()` 按“无包围盒”把所有 chunk 剔除。同步路径因为建模发生在刷新之前、transform 仍脏，所以此前自检与 CLI 没暴露。

**WASD 根因**：`runtimeYaw / runtimePitch` 从相机 `getEuler()` 取到的是**弧度**，注释却写成度；进入运行模式与每帧移动又各做了一次 `glm::radians()`，示例相机 `yaw = π` 被缩成约 `0.055 rad`，水平前向由 `(0,0,-1)` 翻成 `(0,0,1)`。鼠标灵敏度 `0.12` 与 `pitch clamp ±85` 也按度写，在弧度下失效。

**实现**：
- `applyVoxelChunkMesh()` 改登记 Scene 成员 `dirtySpatialEntities`，异步完成后下一帧 `refreshSpatialIndex()` 即可补出 `WorldBoundsComponent`。
- `PendingVoxelChunk` 增加 `cancelled`；`clearEntities()` / `cleanup()` 只标记取消并清空实体 id，不提前销毁作业状态（job lambda 捕获裸指针），`processVoxelChunkBuilds()` 回收时跳过，避免实体 id 复用与 UAF。
- 编辑器加载体素示例在 `createVoxelWorld()` 后 `waitForVoxelBuilds()`，再摆玻璃与相机，避免旧作业用编辑前网格覆盖玻璃。
- 运行模式统一按弧度处理：删除两处 `glm::radians()`，pitch clamp 改为 `±radians(85)`，鼠标灵敏度 `0.01 rad/px`，pitch 改为 `+=`；移动基使用 `forward=(sin yaw,0,cos yaw)` / `right=(cos yaw,0,-sin yaw)`，与 `CameraController` 和视图矩阵一致。
- 自检补齐：体素自检断言每个 chunk 都有 `WorldBoundsComponent`（`boundedChunks == chunkCount`）；运行角色自检断言进入前后 yaw/pitch 原样保留；示例自检实际加载 `voxel-world` 并断言 9 个 chunk 的 Mesh + Bounds。

**可观测**：`[Editor] runtime character selfTest PASS`、`[Editor] example presets selfTest PASS | voxel-world meshes=9 bounds=9`、`[Voxel] scene selfTest PASS | chunks=9 … bounded=9`；`f3-voxel`、`e8-editor-mode`、`e9-runtime-character` 全 PASS。

**回归**：`tools/Validate.ps1 -Incremental -SkipSetup` **67 项 ALL PASS**、stderr 全 0 字节；日志 `build/validation/20261104-173640`；DocCheck PASS。


## 体素图集格边界白线 / 远处闪烁修复（2026-11-04）

**问题**：体素地形每个方块顶面边缘出现白色细线，越远越密并连成白网格；移动视角时纹理闪烁，距离越远越明显，地形上方还有绿色辉光。程序化地形也有轻微闪烁，但轻得多。

**根因**：顶点 UV 用 `uv.x = tile * 64 + 面内跨度` 编码图块，片元用 `fract()` 还原格内 UV。`fract()` 在每个方块边界从 ≈1 跳回 0，`dFdx/dFdy` 在边界处产生整格大小的导数尖峰，硬件据此把 mip 选到最粗一级；而 256×256 图集只有 9/16 格有内容，其余 7 格是纯白，最粗 mip 平均后就是白色，于是每个方块边界出现一圈白线。距离越远，mip 越高、边界像素占比越大，白线越粗且随视角闪烁；Bloom 再把白线扩散成绿色散光。程序化地形不用 `fract()` 解码图集，所以只有普通纹理的轻微锯齿。**当时判断不是 TAA 问题**：修补前管线没有 TAA/FXAA/MSAA，但纯时域/几何锯齿不会只沿每个方块边界出现。

**实现**：
- `shaders/mrt.frag` / `shaders/glass.frag`：体素分支用 `textureGrad` + 显式梯度 `dFdx(fragUV) * 0.25 / dFdy(fragUV) * 0.25`，绕开 `fract()` 的导数不连续；普通材质同样用 `textureGrad`（梯度不缩放），与隐式 LOD 等价。
- `clamp(local, 0.02, 0.98)` 改为半纹素内缩 `0.5 / 64.0`，避免在每格边缘留下 2px 的常量色带。
- `MaterialSystem::fillVoxelAtlas()`：未使用的 7 格从纯白改为草绿色；即使高 mip 跨格混合，也不会再把整张图集平均成白色。

**可观测**：修复前 `build/voxel_shimmer.png` 全屏白色网格；修复后 `build/voxel_shimmer_halftexel.png` 白网格消失、远处不再散光；近景玻璃 `build/voxel_glass_halftexel.png` 纹理正常，`[Glass] sorted=879 drawcalls=1` 不变。`[Validation] errors=0 warnings=0`。

**回归**：`f3-voxel`、`f3-voxel-glass`；全套 `tools/Validate.ps1 -Incremental -SkipSetup` **ALL PASS**，日志 `build/validation/20261104-181700`。

**残留**：没有 TAA/FXAA/MSAA，远处几何轮廓仍有普通锯齿；若需要进一步消除运动时的边缘闪烁，应单独做后处理 AA 项，不混在本次纹理修复里。（2026-11-05 已由 TAA 单独补上。）

## TAA 时域抗锯齿（2026-11-05）

**问题**：修复体素图集白线后，远处方块边缘仍有阶梯状锯齿；移动视角时远处表面纹理（草地/土层）持续闪烁，距离越远越明显。根因是每帧只采样一个像素中心，几何边缘与高频纹理在投影缩小时产生欠采样；没有时域累积，运动时相邻帧的采样点跳变就表现为闪烁。

**实现**：新增 `src/Render/TAAPass`，插在 `GlassPass` 之后、`BloomPass` 之前，读 `hdrScene`、G-Buffer Position 与上一帧历史，写 `taaOutput` 与 ping-pong 历史。

- 抖动：`Halton(2,3)` 像素偏移只加到场景投影矩阵 `[2][0]/[2][1]`；阴影、FrameInfo 与 ImGui 使用未抖动投影。
- 重投影：`shaders/taa.frag` 用未抖动 `previousProjectionView` 把 G-Buffer 世界坐标投回上一帧 UV，不需要运动向量（当前只有相机运动）。
- 历史拒绝：3x3 当前帧颜色 AABB clamp；历史 alpha 通道存世界半径，半径差超过 `max(0.05, worldRadius * 0.0025)` 时丢弃历史。
- 双颜色附件：第二个颜色附件必须显式设置 `blendEnable=false` 与 `colorWriteMask`，否则会被静默丢弃。
- 描述符：`currentColor` 必须绑 `hdrScene`，绑成 `taaOutput` 会与颜色附件形成 feedback loop，验证层报 `VUID-VkDescriptorImageInfo-imageLayout-00344`。
- 关闭：`--no-taa` 让权重与抖动归零，但 Pass 仍执行（Bloom/Present 固定绑 `taaOutput`）；resize 与重新开启时重置历史。

**可观测**：`[TAA] initialized | 1920x1080 | history=2 | jitter=Halton(2,3)`；开启/关闭对比裁剪 `.tmp/taa_compare.png` 中，开启时远处草地与坡边的细密波纹明显收敛，关闭时网格状噪点仍在；`[Validation] errors=0 warnings=0`。

**回归**：新增 `f3v-taa`、`f3v-taa-off`；全套 **69 项** PASS；验证日志 `build/validation/20261105-094857`；DocCheck PASS（documents=65）。

**已知边界**：TAA 只有相机重投影，没有运动向量；物体自身运动、蒙皮或粒子不会被单独补偿。当前场景以静态几何 + 相机移动为主。

## 体素图集 mip 跨格渗色与摩尔纹修复（2026-11-05）

**问题**：2026-11-04 的白线修复后，远处草地/石面仍出现规则的浅色点阵（用户截图 987×571，越远越密）；移动视角时纹理闪烁，TAA 开启只能减轻时域闪烁，无法消除静态摩尔纹。此前误判为图集全局 mip 跨格渗色并试过 32px 纯基色 gutter，但高 mip 把 gutter 平均掉，地形发平。

**根因**：256×256 图集的全局 mip 在高层会把相邻格（草/土/石）平均；显式梯度只解决 `fract()` 导数尖峰，不能阻止采样点在高 mip 落到格边界。逐格 mip 单独也不够：`0.5/64` 半纹素内缩在 mip N 下只离格边 0.25 个 mip texel，线性采样仍会采到邻格。

**实现**：
- `MaterialSystem::fillVoxelAtlas()` 改为 512×512、4×4 格、每格 64px 内容 + 32px gutter、128px 间距；gutter 不再填纯基色，而是 edge-clamp 最近内容像素（`cx/cy` 夹到 0..63 后再算 hash/图案），高 mip 保留本格纹理且不渗邻格。
- `shaders/mrt.frag`：`kAtlasSize=512`、`kPitch=128`、`kPad=32`、`kTile=64`；`cellOrigin=(mod(tileId,4), floor(tileId/4))*kPitch+kPad`；`atlasUV=(cellOrigin+clamp(local,0.5/kTile,1-0.5/kTile)*kTile)/512`；显式梯度乘 `kTile/kAtlasSize`。
- `shaders/glass.frag` 同步同一组常量与映射；否则玻璃会采到错误的图集区域（此坑已实际踩到）。
- 没有采用逐格 mip / 动态 inset：实验中 `max` 轴梯度会把 inset 推到 0.5、整片抹平，`min` 轴能保细节但边界仍渗；最终保留全局 mip + edge-clamp gutter。
- 启动日志 `[Voxel] atlas=512 tile=64 pitch=128 gutter=32`，`f3-voxel` 断言该行。

**可观测**：`.tmp/gutter_edge2.png`（修复后）与 `.tmp/control_original.png`（256 无 gutter）同机位对比：baseline 草地/土层/灰色台面上有规则白点网格，修复后白点消失，近景草地纹理与玻璃图集正常；`.tmp/gutter_glass.png` 中 `[Glass] sorted=879 drawcalls=1`，玻璃透明无白块。`[Validation] errors=0 warnings=0`。

**回归**：`f3-voxel`（新增 atlas 布局断言）、`f3-voxel-glass`；全套 **69 项** PASS；验证日志 `build/validation/20261105-111952`；DocCheck PASS。

**验证注意**：全套用例按仓库基线 `config/engine.json` 跑。用户本地存的相机位姿/关闭阴影会让 `terrain-render` 与 `f6-shadows` 依赖默认相机的断言失败（不是本次代码回归）；跑前临时换基线、跑完恢复本地 config，未把用户 config 提交。

**已知边界**：512 图集 + gutter 比 256 多 4 倍显存（1MB），当前体素规模可忽略；gutter 只保护格边界，若未来图集格尺寸/间距改变，必须同步 `mrt.frag` 与 `glass.frag`，否则会重现渗色或采样错位。

## 流送地形 chunk 边界材质断层修复（2026-11-05）

**问题**：用户截图中的流送地形出现大片米色/白色云状斑块和笔直的矩形边界（`.tmp/stream_correct.png`）。斑块与 TAA 无关：关闭 TAA、Bloom、SSAO、阴影后仍存在；也不是地形纹理平铺问题。

**根因**：`shaders/mrt.frag` 用 `h = clamp((fragPosLocal.y - terrainMinY) / max(terrainHeightRange, 1e-4), 0, 1)` 决定 sand/grass/rock/snow 权重，而 `GeometrySystemGpu` 合批实例与 `GeometrySystem` 逐 chunk push 原先都写**每个 chunk 的 `BoundsComponent` 局部 min/max**。流送 chunk 各自覆盖高度场的一块，局部范围不同；同一世界高度在 chunk 边界两侧得到不同的 `h`，层权重跳变，于是出现矩形沙/雪色块。

**实现**：
- `src/Render/GeometrySystemGpu.cpp` 的 chunk 实例行：`terrainMinY = terrain->config.baseHeight`、`terrainHeightRange = std::max(terrain->config.heightScale, 1e-4f)`。
- `src/Render/GeometrySystem.cpp` 的非合批 push 使用同一组全局值；只有 `terrain == nullptr` 时才回退到旧 bounds。`TerrainMeshBuilder` 的世界 Y 也是 `baseHeight + heightScale * h`，定义一致。
- 两处都增加观测：合批日志输出 `heightCaptured/heightMin/heightRange/heightMixed`，逐 chunk 路径输出 `[Terrain] chunk shading | heightCaptured=1 ...`；`heightMixed=0` 表示所有 chunk 共用全局范围。
- `tools/Validate.ps1` 的 `f2b-terrain-batch` 与 `f2b-terrain-batch-off` 断言 `heightCaptured=1 heightMin=0 heightRange=12 heightMixed=0`。

**可观测**：同一机位 `[0,18,56]` / rotation `[0.3, π, 0]`、`--streaming-world 3 --no-bloom --no-ssao --no-taa --no-shadows`：修复前 `.tmp/stream_correct.png` 有大片沙/雪斑块与矩形边界；修复后 `.tmp/stream_fixed.png` 连续绿色、无边界，日志 `[Terrain] batched draw | chunks=19 ... heightMixed=0`。抬高与俯视机位 `.tmp/stream_fixed_top.png`、`.tmp/stream_fixed_over.png` 同样无斑块；`--no-terrain-batch` 的 `.tmp/stream_fixed_nobatch.png` 与非合批日志也一致。对比图 `.tmp/terrain_seam_compare.png`（上 BEFORE / 下 AFTER）。

**回归**：`f2b-terrain-batch`、`f2b-terrain-batch-off` 断言新增高度范围；全套 **69 项** PASS；验证日志 `build/validation/20261105-115950`；DocCheck PASS。

**附带修复**：`f3-voxel-glass` 曾偶发缺少 `[Glass] sorted=`——CLI 在初始 chunk 的异步网格落地前就摆玻璃，随后落地的旧网格把 `glassFaceCount` 覆盖回 0。`--voxel-glass-demo` 现在先 `scene->waitForVoxelBuilds()`，三次重复运行均稳定输出 `[Glass] sorted=879 drawcalls=1`。

**验证注意**：`--streaming-world 3` 的测试世界在这些机位下看起来是平的；修复前的大片白斑是材质权重伪影，不是几何起伏或纹理平铺错误，修复未改变任何高度场/网格数据。全套用例按仓库基线 `config/engine.json` 跑，用户本地相机/关闭阴影会让 `terrain-render`/`f6-shadows` 的默认相机断言失败；跑前临时换基线、跑完恢复本地 config，未提交用户 config。

**已知边界**：沙/草/岩/雪阈值仍与 `heightScale` 绑定；若未来地形使用真正的高山参数，需要重新校准阈值，而不是改回 per-chunk 归一化。

## 场景树体素 chunk 归入世界子树（2026-11-05）

**问题**：Hierarchy 中 9 个 `Voxel Chunk` 与 `Voxel World`、`Camera` 全部平铺成根节点（用户截图 241×528），树形结构没有体现 `VoxelWorldComponent` 与 `VoxelChunkComponent` 的归属关系，也无法按世界节点折叠/移动/删除整块。

**实现**：
- `Scene::createVoxelWorld()` 在创建每个 chunk 实体后调用 `setParent(entity, world)`，chunk 成为 `Voxel World` 的子节点；世界默认单位变换，挂接不改变 chunk 世界位置。
- chunk 创建完后对 `world` 的 `HierarchyComponent::children` 按 `(chunkZ, chunkX)` 稳定排序，异步完成顺序/存档重建顺序不会让 Hierarchy 行序每次不同。
- `HierarchyPanel` 的树节点统一加 `ImGuiTreeNodeFlags_DrawLinesFull`；`VoxelWorldComponent` / `TerrainStreamingComponent` 有子节点时加 `DefaultOpen`，加载后直接看到 3×3 子树。
- 示例自检增加父节点断言：9 个 chunk 的 `HierarchyComponent::parent` 必须等于世界实体，打印 `voxel-world meshes=9 bounds=9 parented=9`；`tools/Validate.ps1` 的 `e8-editor-mode` 断言该行。
- 搜索框仍按匹配项拍平（E1 行为），不强行展开树；只影响显示，不影响父子关系。

**可观测**：`.tmp/hierarchy_tree2.png` 中 `Voxel World` 默认展开，9 个 chunk 按 `-1,-1` → `1,1` 顺序缩进，带树线；`Camera`、`Skybox` 保持根节点。`--selftest 5 --empty-scene` 输出 `[Editor] example presets selfTest PASS | voxel-world meshes=9 bounds=9 parented=9`，`[Validation] errors=0 warnings=0`。

**回归**：`e8-editor-mode`（新增 `parented=9` 断言）；全套验证日志 `build/validation/20261105-125330`（69 项 ALL PASS，`CONFIG_USER_RESTORED`）；DocCheck PASS（`documents=65`）。

**已知边界**：`DeleteEntityCommand` 的组件快照尚未包含 `VoxelWorldComponent` / `VoxelChunkComponent`。在 Hierarchy 中删除整个 `Voxel World` 会级联删除 chunk 网格，但撤销只能恢复 Transform/Name/Mesh 等通用组件，体素数据不完整；当前先避免用删除世界节点来验证撤销，后续若世界需要可撤销删除，应单独补体素组件快照。

## E-P1：启动失败原生 MessageBox（2026-11-05）

**问题**：双击启动失败时，`main.cpp` 的 catch 只写 `std::cerr` 与可执行文件旁的 `startup-error.log`；没有控制台时用户完全看不到错误，必须自己知道去翻日志（UX-04 的剩余缺口）。

**实现**：
- `src/Core/StartupLog.h/.cpp` 新增 `formatStartupErrorMessage(stage, message, logPath)` 与 `showStartupErrorBox(title, message)`。正文分四段：阶段、原因、诊断日志完整路径、下一步（用 `--help` 看参数、确认驱动支持 Vulkan 1.3、提交日志）。
- Windows 走原生 `MessageBoxW(nullptr, ..., MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST)`；UTF-8 正文先按 `MB_ERR_INVALID_CHARS` 转 UTF-16，失败回退系统代码页。非 Windows 为空操作。弹窗前 stderr 打 `[Startup] showing native error dialog`；`MessageBoxW` 返回 0 时打 `[Startup] MessageBoxW failed (GetLastError=...)`。
- `main.cpp` 的 catch 仍先写 `startup-error.log`，只有交互式启动才弹窗：`if (!unattendedRun && !diagOptions.windowHidden)`。`--hidden`、`--selftest`、截图、地形测试等无人值守路径保持静默，避免 CI/回归卡在“确定”上。
- `src/Core/Application.cpp` 的 UX-04 自检新增 `[Startup] error dialog text selfTest PASS | stage + reason + log path + help hint`，断言弹窗正文确实包含阶段、原因、日志路径与 `--help`；自检不真的弹窗。`tools/Validate.ps1` 的 `b1-window-fit` 断言该行。

**可观测**：`.tmp/startup_dialog.png` 是真实弹窗（用无效 `VK_ICD_FILENAMES` 触发 `Failed to create Vulkan instance!`）：标题 `Tiny Engine 启动失败`，正文含“阶段：启动/运行”“原因：Failed to create Vulkan instance!”、`build\Debug\startup-error.log` 与三步下一步，按钮“确定”；确认后进程以失败码退出。`--selftest 5 --hidden` 输出 `[Startup] error dialog text selfTest PASS`、`[Validation] errors=0 warnings=0`。

**回归**：`b1-window-fit` 增加弹窗正文断言；全套验证日志 `build/validation/20261105-143428`（67 项引擎 + 2 项工具 = 69 项 ALL PASS，stderr 全 0）；DocCheck PASS。真实弹窗无法在无人值守测试里断言，保留无效 ICD 桌面手测。

**已知边界**：弹窗只有“确定”，没有“打开日志/复制路径”按钮；没有交互桌面时 `MessageBoxW` 可能返回 0，此时错误仍完整写在日志里。启动失败后的进程清理与退出码不变。

## 运行模式角色与骨骼动画（2026-11-05）

**问题**：运行模式只有无形的角色控制器，没有可见模型；即便挂上 Quaternius `Character.glb`，还会出现全黑/灰白棋盘格、动画不切换、进入运行模式后坠出世界、WASD 方向与相机相反四个问题。

**根因与实现**：
- **纯色材质走棋盘格**：`src/Scene/GLTFLoader.cpp` 的 `materialAlbedoFallback` 默认值不是 0，没有 `baseColorTexture` 的材质被当成"缺贴图"走棋盘格。改为 0 后保留 `baseColorFactor` 纯色。
- **动画名带前缀**：`CharacterArmature|Idle` 之类的前缀让状态机查不到 clip。导入时按最后一个 `|` 剥掉前缀，只保留 `Idle/Walk/Run/Jump/Jump_Idle/Jump_Land`。
- **蒙皮模型进静态合批**：`src/Render/GeometrySystem.cpp` / `src/Render/SceneGpuBatch.cpp` 只按"有无动画"筛选不够，蒙皮模型必须用 `Model::isDynamic()` 排除，否则顶点被静态缓存、每帧不更新。
- **出生点与 yaw**：`EditorLayer::enterRuntimeMode()` 沿相机视线 2..180m、步长 1m ray-march 找地面（`probe.y` 在 ground-6..ground+0.1）；找不到时退到相机前方 2m 并下移 1.62m。相机 yaw/pitch 本身是弧度，存进 `runtimeYaw/runtimePitch` 后不能再转一次；角色初始朝 `runtimeYaw`，WASD 跟随同一 yaw。

**动画接入**：
- `src/Scene/Model.{h,cpp}` 增加 CPU 蒙皮：骨架节点 world × inverse bind matrix，每顶点 4 joint 加权，3 个环形 host-visible 动态顶点缓冲，每帧 `vmaFlushAllocation` 后按环形索引提交。
- `Scene::createCharacterEntity()` + `attachCharacterModel()` 建 `CharacterControllerComponent` / `CharacterAnimationComponent`；`updateCharacterAnimation()` 状态机优先级 Jump_Land > Jump > Jump_Idle > Run > Walk > Idle，落地判定 `previousVerticalVelocity < -1` 触发 0.45s 落地动作。
- 第三人称相机焦点为脚底 +1.25m、后撤 4m、不低于脚底 +0.6m；中键 0.01 rad/px、pitch ±85°；Shift 跑，走 0.55、跑 1.35 倍速（相对 6m/s 基础速度）。
- `--runtime-mode` 可启动即进入运行模式；`src/Render/GeometrySystem.cpp` / `SceneGpuBatch.cpp` 跳过动态模型。
- **加载器审计重构**：`loadGLTFModel` 原本一个函数承担材质/骨架/几何/动画，圈复杂度 112 触发审计 blocker；拆成 `buildMaterialTable` / `buildSkeletonData` / `appendPrimitive` / `parseAnimations` 等匿名命名空间 helper，`loadGLTFModel` 只做编排，行为不变，`code_audit scope=diff` 0 blocker。

**可观测**：控制台依次出现 `[Character] model attached | ... skinned=1 animations=18 scale=0.5`、`[Runtime] spawn=... onGround=1`、按 W/Shift+W/Space 时 `[Runtime] clip=Walk/Run/Jump`；`--selftest 20 --empty-scene` 输出 `[Editor] runtime character selfTest PASS | ... clips=Idle/Walk/Run/Jump/Jump_Idle/Jump_Land active=Idle`。

**回归**：`e9-runtime-character`（`tools/Validate.ps1`，断言 `skinned=1`、`animations=18`、6 个 clip 与 `active=Idle`）；全套 **69 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261105-155458`；DocCheck PASS（documents=66）。

**已知边界**：单角色、CPU 蒙皮、无动画混合/事件/IK/root motion；角色只绕 Y 轴转；出生点依赖地形高度采样，悬空平台场景仍需手动摆位。

## 水面（2026-11-05）

**问题**：地形和体素世界都没有任何水体；没有海平面，也就没有反射、岸边过渡和水面动画。

**实现**：
- 新增 `src/Render/WaterPass.{h,cpp}` 与 `shaders/water.vert` / `water.frag`。Pass 插在 Glass 之后、TAA 之前：水面混合进已经着色的 HDR，水边缘能参与 TAA，太阳高光能参与 Bloom。
- 几何是相机中心径向网格：160 圈 × 192 段（30721 顶点 / 183744 索引），半径 `r_i = 1.2i + 0.15i²`，最远约 4032m；顶点只存局部 XZ，着色器加 `cameraPos.xz`，网格因此永远跟随相机而不用重建缓冲。近处环密、远处环疏。
- 顶点位移 4 个大波（波长 23 / 14 / 9 / 5.5m）；像素着色器解析求梯度得到法线，再叠加 3 个近距 ripple（波长 1.7 / 1.1 / 0.75m，60..220m 内淡出，避免远处闪烁）。
- 着色：Fresnel `0.02 + 0.98·(1-NdotV)^5`；反射直接采样全局 Set0 binding1 的等距柱状环境图，并用 `textureGrad` 折回经度接缝；从 G-Buffer `inPosition` 取水后不透明几何位置算视线厚度，`depthFactor = 1 - exp(-thickness·0.22)` 控制浅水/深水；岸边薄水加波峰生成泡沫；太阳高光 `pow 900` + `pow 120`。
- Pass 合同：`LOAD` HDR + 深度，`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`，`depthTest = TRUE`、`depthWrite = FALSE`、`LESS_OR_EQUAL`，关闭背面剔除，只绑 Set0，push constant 32 字节（C1 后为 48 字节，见下节）。
- 只有 registry 中存在 `TerrainComponent` 或 `VoxelChunkComponent` 才注册 Water，避免 Sponza/空场景出现全球水面。`EngineSettings` 增加 `waterEnabled/waterLevel/waterWaveStrength/waterWaveSpeed`；编辑器 Render 面板与运行时 ImGui 都有“水面”分组；`--no-water` 直接不注册，打印一次 `[Water] disabled`；`config/engine.json` 持久化 `water` 节点。

**可观测**：启动打印 `[Water] initialized | rings=160 segments=192 vertices=30721 indices=183744 radius=4032m`；首帧打印 `[Water] first draw | level=... waveStrength=... waveSpeed=... cameraY=...`；`--no-water` 打印 `[Water] disabled`。

**回归**：`f3v-water`（断言 initialized/first draw + `[Validation] errors=0 warnings=0`）与 `f3v-water-off`（断言 `[Water] disabled`）；全套 **69 项引擎 + 2 项工具 = 71 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261105-162911`；DocCheck PASS（documents=67）。

**已知边界**：水面是无限水平面，不跟随地形起伏；本阶段反射只来自环境图，没有 SSR，近处物体不会出现在倒影里（C1 后改为半分辨率平面反射，见下节）；没有折射和水下后处理，镜头进入水下后不会切换色调；没有交互（涟漪/尾迹）。

## 远水 A+B：footprint 波浪滤波 + 距离雾（2026-11-05）

**问题**：站在 6.4m 视高看 100m~4km 外的水面，Fresnel 在掠射角接近 1，水面几乎只剩环境图天空反射；4 个大波和 3 个 ripple 没有按像素足迹过滤，远处波峰在 1km 外不足 1 像素，被 Fresnel 和 `pow(NdotH, 900)` 高光放大成条带/闪烁；同时全场景没有空气透视，远水与地平线是硬切边。

**A：波浪 footprint 滤波**（`shaders/water.frag`）：
- 每个波用 `dFdx(p)/dFdy(p)` 求世界 XZ 像素足迹，`resolve = 1 - smoothstep(0.5, 1.5, k*footprint)`；未解析波不再参与法线。
- 淡出的斜率能量累加成 `slopeVariance`，转成 `roughness = clamp(sqrt(variance)*7 + smoothstep(120,3000,distance)*0.30, 0.02, 0.65)`。
- 环境反射的 `textureGrad` 导数乘 `1 + roughness*12`；太阳高光指数从 900 按 `roughNorm` 展宽到 24、峰值 3.0 -> 0.55，宽层 120 -> 8、0.10 -> 0.18（specular AA）。
- 近处保持原来的锐利反射/窄高光；远处变成略模糊的反射和宽高光，不再出现随机条带与点状闪烁。

**B：全场景距离雾**：
- `GlobalUbo` 尾部新增 `fogParams=(start, density, 0, enabled)`，`Application` 每帧上传；默认 `enabled=true`、`start=80m`、`density=0.00045`（1km 约 34%、4km 约 83%）。
- 雾色取环境图地平线方向 `normalize(vec3(-V.x, 0.035, -V.z))` 的 LOD3，和天空同源；`lighting.frag` 对不透明几何混合，天空在法线为空时提前返回、不二次染色。
- `water.frag` 用真实视线距离 `waterDistance` 做指数雾；`glass.frag` 通过 32 字节 push constant 拿到同一组参数，只淡出玻璃颜色，alpha 不变，避免远处玻璃变成实心雾墙。
- 编辑器 Render 面板新增“距离雾”分组，运行时 ImGui 新增 `Atmosphere / Fog`；`config/engine.json` 持久化 `fog` 节点；没有 `--no-fog` 命令行开关。

**可观测/证据**：`build/water_after.png` 为近景湖面；把相机转向开阔水面后，同机位 `build/farwater_fog.png` 与 `build/farwater_nofog.png` 几乎逐像素一致（mean 0.047）——因为远水 Fresnel 已接近天空，而雾色正是环境图地平线色，这是设计上的无缝淡出。临时把密度调到 0.01 拍 `build/water_fogheavy.png`，与默认雾图全画面 mean 13.5、底部水体约 +9 级，证明 Water/Lighting/Glass 的雾参数确实生效；默认密度在 100m 水体只约 0.6 级，近景几乎不可见。

**回归**：全套 **69 项引擎 + 2 项工具 = 71 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261105-172117`；DocCheck PASS（documents=67）。

**已知边界**：本阶段远水反射仍只来自环境图（2026-11-05 的 C1 已补半分辨率平面反射，见下节）；雾是均匀指数雾，没有高度衰减、体积光/太阳散射或时间变化；footprint 滤波只修法线与高光，不改变几何，外圈网格 132m 段长的几何欠采样仍在。

## C1 半分辨率平面反射（2026-11-05）

**问题**：A+B 把远水的波浪条带和硬切边压下去了，但远水仍然只反射环境图里的天空。视高 6.4m、距离 1km 时 Fresnel 已到 0.97，水面近似一块灰蓝镜面；近处能看到水色、透底和波纹，远处却看不到对岸山体和石墙，缺少“这是水”的空间线索。

**实现**：
- 新增 `src/Render/ReflectionPass.{h,cpp}`。半分辨率（1920x1080 -> 960x540）重建一套 G-Buffer：Position / Normal / Albedo / Pbr 四张颜色 + 深度 + 反射色；深度沿用主 G-Buffer 格式，反射色沿用 HDR 格式。每帧在飞 2 槽。
- 反射相机是水面平面镜像：`mirror[1][1] = -1`、`mirror[3][1] = 2 * waterLevel`，`reflectionView = mainView * mirror`；投影用**未抖动**的主投影，避免 TAA jitter 进入反射；反射相机位置 `(x, 2w - y, z)`。反射 UBO 复制主 UBO 后只替换 `projectionView` / `cameraPos`。
- 渲染图新增两个 pass：`PlanarReflection.GBuffer`（CLEAR 4 色 + 深度，写反射 UBO 后调用 `GeometrySystem` 重画不透明几何）与 `PlanarReflection.Lighting`（读 4 色 + IBL 三件套 + 环境图，CLEAR 后写反射色）。`GeometrySystem::render` 增加视图覆盖参数，反射时不画网格、不建 CPU terrain 状态；`SimplePushConstantData` 末尾增加 `clipY/clipMode`，`mrt.frag` 丢弃水面以下的片元，避免水下地形进入倒影。
- `WaterPass` 从 Set0 binding12 采样反射色：`screenUV + N.xz * (0.0015 + roughness * 0.006)` 做轻微扰动；push constant 从 32 字节扩到 48 字节，末尾 `params3.x` 是平面反射开关。关闭时 binding12 回退环境图，着色器走原来的天空反射分支。
- 开关 `water.planarReflection`（默认 `true`），编辑器 Render 面板与运行时 ImGui 都有 `Planar Reflection`；`config/engine.json` 持久化 `water.planarReflection`。运行时切换会重建全局描述符，避免 binding12 指向未转换布局的纹理。
- 只有“水面开启 + 平面反射开启 + registry 有 Terrain/VoxelChunk”同时成立才注册反射 pass；没有可反射的不透明几何时不付这份代价。

**可观测**：启动打印 `[Reflection] planar reflection initialized | extent=960x540`；首帧打印 `[Reflection] first draw | level=2 mirrorY=-6.8105 extent=960x540`；Water 首绘日志末尾带 `planarReflection=1`；关闭时为 `0`。

**证据**：同机位 `build/reflection_on.png` 能看到绿色山体与石墙倒影，`build/reflection_off.png` 只剩纯天空反射。早期关闭路径曾出现 `[Validation] errors=40 warnings=0`，原因是 binding12 仍指向从未转换布局的反射纹理；回退环境图并在开关变化时重建描述符后恢复 `0/0`。

**回归**：新增 `f3v-water-reflection`（`--selftest 20 --voxel-world 1`，断言 initialized / first draw / `planarReflection=1` / `[Validation] errors=0 warnings=0`）；全套 **70 项引擎 + 2 项工具 = 72 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261105-180749`；DocCheck PASS（documents=67）。

**已知边界**：反射只有半分辨率，边缘有轻微软/锯齿；倒影只含不透明几何，玻璃、水面本身、粒子与编辑器 gizmo 不进倒影；没有 SSR、没有折射、没有独立于主视角的时域累积；反射相机使用主相机的阴影级联，远处倒影的阴影精度有限；`--no-water` 时反射 pass 不注册。

## V-04：体素共享 arena + 间接合批（2026-11-06）

**问题**：每个体素 chunk 都建一份独立 `Model`，半径 12 的 625 块就是 625 份顶点/索引缓冲和 625 次绑定，与 V-05 的无限世界目标直接冲突；同时 625 个 chunk 各占一个实体材质槽，首次跑半径 12 直接触发 `DescriptorManager: material slots exhausted! capacity = 256`。

**实现**：
- `Scene::applyVoxelChunkMesh()` 仍先建临时 `Model` 完成上传，但随后 `TerrainMeshArena::acquire()` 取切片、`copyInto()` 拷进与地形共用的 arena，再 `releaseGpuBuffers()`；`Model` 只保留 CPU 几何供拾取/编辑。切片基址与数量写进 `VoxelChunkComponent::arenaVertexBase/arenaIndexBase/arenaVertexCount/arenaIndexCount`（默认 `0xFFFFFFFF`）。
- 重建、删除、清空 chunk 前先 `TerrainMeshArena::release()` 旧切片；全空 chunk 也会归还切片并移除 `MeshComponent`，避免编辑把 arena 一点点吃满。
- `GeometrySystem` 每帧预扫描可见体素 chunk：每块一行 224 B 的 `SceneGpuBatch::InstanceRecord` 写进按帧轮转的 host-visible 实例表（上限 1024），每个子网格一条 `VkDrawIndexedIndirectCommand`（上限 2048）；`firstIndex = 子网格基址 + arenaIndexBase`、`vertexOffset = arenaVertexBase`、`firstInstance = 实例行号`，整批一条 `vkCmdDrawIndexedIndirect`。
- 反射视图不复用主视角命令，退回逐块 arena draw；`SceneGpuBatch` 的 CPU 批跳过 arena-backed 体素；`ShadowPass` 也改走 arena 切片。
- 材质槽池 `DescriptorManager::init(..., 256)` 提到 **1024**（bindless 4 个纹理数组只多约 3k 描述符，低于常见 sampled-image 上限），625 块世界不再耗尽槽位。

**实测（Debug / RTX 2070）**：
- 半径 1：`[Voxel] world build complete | chunks=9 arena=9 vertices=10364 indices=15546`；`[Voxel] arena batch | chunks=9 commands=9 ... stride=224 mode=indirect`。
- 半径 12：`chunks=625 arena=625 vertices=725888 indices=1088832`，arena 容量 1100000/5900000 未溢出；主视角可见 535 块，`[Voxel] arena batch | chunks=535 commands=535 vertices=619748 indices=929622`，draw calls 106；Validation 0/0。200 帧 Debug 用时 8.63 s（23.17 FPS），只作功能记录，不作为性能结论（该配置 TAA 关闭）。
- 玻璃：`--voxel-glass-demo --selftest 40` 仍为 `[Glass] sorted=879 drawcalls=1`，Validation 0/0。
- 编辑器 `--selftest 5 --empty-scene`：体素世界 `meshes=9 bounds=9 parented=9`，两次世界 `arena=9`，Validation 0/0。

**回归**：`f3-voxel` 增加 `arenaRebuilt=1`；新增 `f3-voxel-arena-625`（`--selftest 200 --voxel-world 12`，断言 `chunks=625 arena=625`、`[Voxel] arena batch`、Validation 0/0）。全套 **73 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-102034`；DocCheck 通过。

**测试稳定性**：`terrain-render` 在用户相机 `[0,42,90]` 下暴露既有 flaky（`git stash` 掉 V-04 源码后仍复现）：CPU/GPU 最近点距离差 1 ULP，正好落在 LOD 阈值两侧，旧断言按整数逐位比较就失败。现改为 distance/extent 在 1e-4 相对误差内时允许 `lodA/lodB/stableLod` 差 1 级；调试契约统一为 8 word：`visible/lodA/lodB/blend/commandCount/distance/extent/stableLod`。这不是 V-04 数据错误，也没有放宽画面验收。

**已知边界**：arena 仍是两块不可增长的大缓冲，超出容量时该 chunk 退回自有 buffer；体素批次上限 1024 chunk / 2048 命令，超限时打印一次 `[Voxel] arena batch capacity exceeded` 并退回逐块 arena draw；反射仍是逐块 draw，没有做镜像视图的间接批；625 块落地仍受每帧最多 4 块的限制（V-02）。

## F6-01：水面近场 SSR（2026-11-06）

**问题**：C1 的平面反射解决了“远水只反射天空”，但倒影只来自镜像相机重画的不透明几何：屏幕外物体、被主视角遮挡的细节和近场薄几何没有屏幕空间校验；同时平面反射是另一套完整 G-Buffer + Lighting，近场精度受半分辨率镜像相机限制。F6-01 要给水面补一层近场屏幕空间反射，并保留平面反射/IBL 作为回退。

**实现**：
- 新增 `src/Render/SSRPass.{h,cpp}` 与 `shaders/ssr.frag`：半分辨率（1920x1080 -> 960x540）`ssrColor`，每帧在飞 2 槽；Pass 读 `hdrScene`、G-Buffer 世界位置与全局 UBO，输出 RGBA16F，alpha 是命中置信度。
- 片元用解析平面 y = waterLevel 求视线与水面交点，再沿 `reflect(viewDir, up)` 做固定 48 步 DDA；每步把射线点用未抖动投影投回屏幕，采样 G-Buffer 世界坐标，命中条件为“在射线上、沿射线距离落在 [0, t]、点到射线垂直距离小于厚度”（默认厚度 0.5m，按步长自适应放大）；命中后采样 HDR 颜色，并按距离淡出置信度。屏幕外、超过 `ssrMaxDistance`（默认 200m）无命中时 alpha=0。
- `WaterPass` 的 Set0 binding13 绑定 SSR 结果；`water.frag` 用与平面反射相同的扰动 `reflectionUV` 采样，权重为 `ssr.a * (1 - smoothstep(0.20, 0.55, roughness)) * (1 - smoothstep(120, 220, distanceToCamera))`，再与平面反射/环境图混合。粗糙水面和远水仍交给平面反射/IBL。
- 注册条件与水面一致：`ssrEnabled && waterEnabled && (Terrain || VoxelChunk)`，Pass 排在 `PlanarReflection.Lighting` 之后、`Glass` 之前；关闭时 binding13 回退环境图，避免指向未转换布局的纹理。
- 开关：`ssr.enabled`（默认 true）、`ssr.maxDistance`（默认 200m）、`--no-ssr`；编辑器和运行时 ImGui 的 Water 分组都有 Screen Space Reflection / SSR Max Distance。
- 旧配置兼容：旧版本只在 `planned.ssrEnabled` 写占位 false，读取时若没有顶层 `ssr` 节点，仅在旧值为 true 时继承，否则保留新默认 true，避免升级后被静默关闭。

**实测（Debug / RTX 2070）**：
- 体素世界：`[SSR] initialized | extent=960x540 steps=48`、`[SSR] first draw | extent=960x540 waterLevel=2 maxDistance=200 planarReflection=1`、Water 日志末尾 `ssr=1`；Validation 0/0。
- 默认配置（平面反射开）下 SSR 开/关截图差异很小（925 像素），因为平面反射已经覆盖大部分反射；关平面反射 + 关波浪后，同机位开/关差异 **91918 像素**，水面出现浮空岛底面的深色倒置轮廓，无 speckle 噪点。证据图 `build/Debug/ssr_vstill.png`（开）、`build/Debug/ssr_vstill_off.png`（关）、`build/Debug/ssr_vstill_crop.png`（上下对比）。
- 流送地形：可见水面基本在 220m 外，SSR 距离权重已降到 0，画面与关闭一致；这不是 SSR 失效，是近场限定。
- 已知视觉边界：浮空岛底面本来无光照，SSR 倒影呈深色是几何结果；SSR 是半分辨率、单帧、无时域累积的屏幕空间方法，水面动画和薄几何会有轻微抖动/漏光。

**回归**：新增 `f3v-ssr`（断言 initialized / first draw / Water first draw / `ssr=1` / 0/0）与 `f3v-ssr-off`（`--no-ssr`，断言 `[SSR] disabled` / `ssr=0` / 0/0）。全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-105750`；DocCheck PASS。

## R-06：水面法线贴图替换高频正弦波（2026-11-06）

**问题**：水面在默认视角下出现规则的**黑白菱形格**，关掉 `waveStrength` 后完全消失。根因不是棋盘格占位材质，也不是 SSR/平面反射：`shaders/water.frag` 原来除了 4 个大波，还在片元里叠了 3 个高频正弦 ripple（波长 1.7 / 1.1 / 0.75m），多组解析法线在 Fresnel 和 `pow(NdotH, 900)` 高光下发生规则干涉，远处采样不足时进一步走样成网格。

**实现**：
- 新增 1024x1024 无缝水波法线 `Resources/Texture/Water/water_normal.png`（1,903,139 bytes，CC0）：白噪声 FFT 乘 `k^-1.60` 风浪频谱、30 度主风向、方向性 0.55，逆变换得高度场，再求梯度编码为 `0.5 + 0.5 * normalize(-gx, -gy, 1)`。
- 纹理用 `Texture::tileableMips=true`：CPU 2x2 box filter 按纹理尺寸取模生成完整 mip 链，整链一次 `vkCmdCopyBufferToImage`，平铺边缘不会因 clamp 偏色。
- Set0 扩到 15 个 binding，新增 binding14 水面法线；`Application` 加载为 `R8G8B8A8_UNORM`，`rebuildGlobalDescriptors()` 每个 frame slot 写入，析构时在 `environmentTex.reset()` 后 `waterNormalTex.reset()`。
- `shaders/water.frag` 删除 `RIPPLES` 与 `filteredRippleGradient`，改为两层滚动法线：18m 周期权重 0.55、流速 (0.020, 0.012)；42m 周期权重 0.35、流速 (-0.013, 0.017)。用 `textureGrad` 接收世界 XZ 像素足迹，150..700m 以外按 `detailFade = 1 - smoothstep(150, 700, distanceToCamera)` 淡出；LOD 超过 1 后用 `textureQueryLod` 估算 `slopeVariance = smoothstep(1, 6, lod)^2 * 0.008`，交给粗糙度展宽高光。
- 首轮运行 exit 3：`waterNormalTex` 未在析构中释放，VMA 报 `Assertion failed: m_pMetadata->IsEmpty() && "Some allocations were not freed before destruction of this memory block!"`（`3rdParty/VulkanMemoryAllocator/vk_mem_alloc.h:10975`）。补 `reset()` 后 exit 0。

**实测（Debug / RTX 2070）**：
- 同机位截图 `build/Debug/water_check.png`（修复前）与 `build/Debug/water_normal_test.png`（修复后）对比，规则黑白菱形格消失；提亮 4 倍的近景裁切 `.tmp/water_after_bright.png` 可见连续波纹，未见平铺接缝。
- 启动日志新增 `[Texture] ...water_normal.png (1024x1024)` 与 `[Tiny] Water normal map loaded | 1024x1024 UNORM wrapped mips.`；水面首绘仍为 `planarReflection=1 ssr=1`，Validation 0/0。

**回归**：`f3v-water` 新增 `[Tiny] Water normal map loaded |` 断言；全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-113028`；DocCheck PASS。

**已知边界**：水面细节来自单张重复法线贴图，18m/42m 两层滚动只能降低重复感，极近处仍可能看出周期；没有流场/交互涟漪/泡沫贴图。修复后水面不再有原先由正弦网格产生的高频闪点，整体比修复前更暗、更连续，这是去掉干涉伪影后的预期结果。

## F5-01：元数据驱动 ECS（2026-11-06）

**问题**：编辑器里的特性和特效没有统一挂载入口。Inspector 只绘制硬编码组件，新增一个效果要同时改 Scene、命令栈、序列化和删除/复制快照；`Scene::update()` 也没有明确的系统阶段，效果逻辑无处安放。

**实现**：
- `src/Scene/ComponentTypes.h`：`PropertyType`（Bool/Int/Float/Vec3/Color/String）、`PropertyValue`、`NamedProperty`、`ComponentRecord`；record 是纯数据，可比较、可存盘、可进命令栈。
- `src/Scene/ComponentRegistry.h/.cpp`：`ComponentDescriptor`/`PropertyDescriptor` 做类型擦除；`makeComponentDescriptor<T>` 生成 addDefault/has/remove/read/write/defaults，Scene 构造时注册内置组件。
- `src/Scene/SystemScheduler.h/.cpp`：Simulation / PostTransform 两阶段，按 order 稳定排序；`Scene::update()` 在 `updateWorldTransforms()` 前跑 Simulation、之后跑 PostTransform，profiling 与非 profiling 两条路径都接入。
- `src/Scene/FeatureComponents.*`：`effects.spin`（axis/degreesPerSecond/enabled）与 `effects.orbit`（center/radius/degreesPerSecond/angle/enabled）作为“新特性一键挂载”的样板。
- Scene 公开 API：`addComponent` / `removeComponent` / `hasComponent` / `readComponent` / `defaultComponentRecord` / `applyComponentRecord` / `captureComponentRecords`。
- Inspector：`drawComponents()` 列出已挂组件，按属性类型绘制 Checkbox/DragInt/DragFloat/DragFloat3/ColorEdit4/InputText；改动走 `SetComponentRecordCommand`，右键移除，`+ Add Component` 按分类添加默认 record。
- `SceneSerializer` v3：`components` 作为可选数组；未知组件/属性或坏值跳过，旧 v2 文件无 components 仍可读；roundTrip 逐实体比较组件。
- 撤销/删除/复制：`SetComponentRecordCommand` 覆盖 add/remove/modify 并支持拖拽合并；`DeleteEntityCommand` 快照 components；`DuplicateEntityCommand` 复制已注册 record。

**测试**：
- `[Scene] ECS component selfTest PASS | components=2 | systems=2`：注册、增删、默认值、读写、自洽比较、Spin 系统旋转 45°/0.5s。
- 编辑器 `selfTestWithScene`：给 B07 角色挂 `effects.spin` 90°，走 capture → JSON → load → build 后仍为 90°；再用 `SetComponentRecordCommand` 改 180°，验证 apply/undo/redo/undo。stderr 0。
- `tools/Validate.ps1` 的 `selftest-default` 增加 `[Scene] ECS component selfTest PASS` 断言。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-120327`；DocCheck PASS（documents=67 lines=11595）。

**边界**：组件/系统仍是 C++ 编译期注册，没有脚本、热重载、属性数组/结构体；系统在编辑模式也随 `Scene::update()` 推进；属性只有 6 种基础类型。

## F5-02：预制体资产（2026-11-06）

**问题**：编辑器只能整场景保存/打开，做好的实体组合无法复用；每个场景都要手工重建同一套层级、网格、材质与元数据组件。场景描述已经是纯数据，但缺少“只捕获一棵子树”和“追加到现有场景”的入口。

**实现**：
- `Scene::captureDescription(entt::entity root)`：按 `HierarchyComponent::children` DFS 收子树，父先子后；跳过体素/地形 chunk；子树不写 `voxelWorlds`。无参版本保持整场景捕获。
- `Scene::instantiatePrefab()`：不清场景，追加一组实体；校验单根、父下标、环与运行期实体；逐实体恢复 Transform/层级/Name、角色、网格与材质、元数据组件，最后把根挂到 parent。失败按 created 逆序销毁，不触碰已有场景。
- 抽出 `Scene::applyMeshAndMaterialFromDescription()` 供场景加载与预制体实例化共用；同时修掉“角色带 `meshPath` 时被 `else if` 吞掉、网格不加载”的旧路径。
- `src/Editor/Prefab.*`：`validate` / `save` / `load`，复用 v3 `SceneSerializer`；保存前先拒绝子树里的 `VoxelWorldComponent`，避免实例化时体素世界静默丢失。
- `InstantiatePrefabCommand`：构造时先实例化（adopted），首次 `apply()` 只接管；undo 逆序销毁，redo 重新实例化并产生新 ID。
- 菜单：文件 → 保存选中为预制体...；创建 → 实例化预制体...；播放中文档只读时拒绝。文件过滤器 `*.prefab.json`。

**测试**：
- 编辑器自检 `[Editor] prefab selfTest PASS | save/load/instantiate/undo/redo`：cube 父子树 + `effects.spin` 90°，覆盖子树捕获、单根下标、存读、父子关系、组件值、mesh 计数、undo 后消失、redo 换新 ID 恢复。
- `tools/Validate.ps1` 的 `e1-batch-commands` 增加 `[Editor] prefab selfTest PASS` 断言，不新增用例编号。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-124323`；DocCheck PASS（documents=68 lines=11711）。

**边界**：单根子树；没有嵌套预制体、变体/参数覆盖、实例与源资产联动；不保存体素世界、地形、相机与流送状态；每次实例化重新分配材质槽位；实例化根固定在场景根下；redo 后实体 ID 变化。说明见 [prefabs](../90-editor/prefabs.md)。

## F5-03：内置效果/运动组件第一批（2026-11-06）

**问题**：F5-01 只注册了 Spin/Orbit 两个示例，`+ Add Component` 菜单只有两项；常见效果（浮动、脉冲、摆动、朝向、跟随）仍要写代码或手工逐帧改 Transform。

**实现**：
- `src/Scene/FeatureComponents.h` 新增 `BobComponent` / `PulseComponent` / `SwayComponent` / `LookAtComponent` / `FollowComponent`；运行期 `time`、`lastOffset`、`lastFactor`、`lastAngle` 不暴露给 Inspector。
- `src/Scene/FeatureComponents.cpp` 新增 5 个系统：Bob 120、Sway 130、Pulse 140、Follow 150、LookAt 160；Bob/Sway/Pulse 用“本帧 - 上帧”增量写 Transform，保留用户手动平移/缩放，LookAt/Follow 直接按目标计算。
- LookAt 用 `glm::lookAt` 矩阵转四元数，`turnSpeed>0` 时 slerp，视线与 up 平行时换 up 兜底；Follow 按 `speed*dt` 步进，停在 `stopDistance`。
- 属性 clamp：Bob amp 0..100 / freq 0..100 / phase ±720；Pulse amp -0.95..10；Sway amp 0..180；LookAt turnSpeed 0..3600；Follow speed/stop 0..1000。
- 分类：Spin/Orbit/Bob/Sway/Pulse 为 Effects，LookAt/Follow 为 Motion；组件 id 统一 `effects.*`。

**测试**：
- `[Scene] ECS component selfTest PASS | components=7 | systems=7`：注册表/调度器数量、Spin 45°/0.5s、Bob 0.25s 到 0.25 再回 0、Pulse 0.25s 到 1.15 再回 1、Sway 中段旋转且整周期回正、LookAt target=(10,0,0) 时局部 -Z 朝 +X、Follow speed=2 一秒 x=2、再四秒停在 9.9。
- 编辑器 `TinyEditor.exe --selftest 5 --empty-scene` 通过，`[Validation] errors=0 warnings=0`。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-142427`；DocCheck PASS（documents=68 lines=11782）。

**边界**：效果只在实体本地空间；LookAt/Follow 的 target/up 是父空间坐标，顶层即世界；没有碰撞/寻路/曲线/缓动，Follow 直线移动会穿墙；系统在编辑模式也推进，暂停不冻结；属性仍只有 6 种基础类型，不能做曲线或数组。

## F5-04：原生组件接入统一 Add Component（2026-11-06）

**问题**：F5-01/F5-03 的 `+ Add Component` 只覆盖 7 个 `effects.*`；Character、Render Tag 等已有原生组件只能在代码里挂载，Inspector 没有入口。同时原生组件已有专用序列化字段，若直接进注册表会被写入 v3 `components`，并在加载时被“文件里没有就移除”的逻辑误删。

**实现**：
- `ComponentDescriptor` 增加 `addable` / `removable` / `drawProperties` / `serialized` / `required` / `canRemove` / `unavailableReason`；`makeComponentDescriptor<T>` 改用 `ComponentOptions`。默认值保持旧行为，7 个 `effects.*` 不受影响。
- 新增 `src/Scene/NativeComponents.*`，注册 9 个 `native.*` 描述：Name/Transform/Hierarchy 为必需只读；Character Controller（8 个 Float）与 Render Tag（skybox/grid）可增删；Camera/Terrain/Terrain Streaming/Voxel World 为创建菜单托管，菜单置灰并显示原因。Camera 是空 tag，描述符手写。
- `Scene::addComponent` / `removeComponent` 检查 `addable` / `removable` / `required` / `canRemove`；Render Tag 在地形实体上不可移除。
- `captureComponentRecords` 只收集 `serialized` 的组件；场景加载的“移除缺失组件”和 apply 循环跳过 `serialized=false`，文件里伪造的 `native.*` 记录打印警告后忽略。原生组件继续走专用字段，不重复写 v3。
- Inspector：固定面板跳过通用属性绘制；Character 的 8 个参数改为可编辑并走 `SetComponentRecordCommand`；Character/Render Tag 面板底部提供移除按钮；Add 菜单对不可添加项置灰并显示 tooltip。
- `DeleteEntityCommand` 增加 `CharacterControllerComponent` 显式快照，删除撤销不会丢角色参数。

**测试**：
- `[Scene] ECS component selfTest PASS | components=16 | systems=7`：7 个 `effects.*` 的完整周期，加上 native Character 的 add/read/apply/remove（radius 0.35→0.42）、Render Tag 增删、必需组件拒删、`captureComponentRecords` 不含 `native.*`。
- `[Editor] prefab selfTest PASS`：capture 的实体 `isCharacter=true` 且 `components.size()==1`（只有 `effects.spin`），读回 radius≈0.42，实例化后 Character 记录仍在，undo/redo 通过。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-150438`；DocCheck PASS（documents=68 lines=11864）。

**边界**：只接入 Character Controller 与 Render Tag 两个可添加原生组件；Mesh/Material 及 Terrain/Voxel/Streaming/Camera 仍由固定面板或创建菜单管理；原生组件不进 v3 `components`，跨版本仍依赖专用字段。

## R-08：水面暗反射补天空底色（2026-11-06）

**问题**：用户标注的远水区域整体发暗、偏绿，像一块没有细节的暗色平板（水面区域 mean RGB≈(100,117,117)，天空≈(133,143,154)）。先怀疑平面反射没有写天空 alpha，做了“反射 G-Buffer 天空 alpha=0 + 水面按 alpha 混合”的修复；但在 streaming 高机位、voxel 低机位和“反射跳过 skybox”三组对照下，开/关都只有 UI 区域差异（最多 2537 像素，水面 0 像素），说明反射 G-Buffer 里天空并不是黑的，这条路径是 no-op，已完整回退。

真正来源是三个观感参数叠加：
- 天空处 G-Buffer 距离≈10000m，`depthFactor≈1`，`body` 取 `deepColor=(0.008,0.035,0.055)`，本身接近黑色；
- 远水 `alpha=mix(0.18,0.92,depthFactor)` 再叠加 Fresnel，几乎不透明，背后亮天空透不出来；
- 平面反射在该角度主要采到对岸深色地形，且会完整覆盖环境图反射，于是暗地形铺满水面。

**实现（`shaders/water.frag`）**：
- 保存平面反射/SSR 之前的环境图反射 `environmentReflection`，之后按反射亮度只提亮暗部：`luma = dot(reflection, vec3(0.2126,0.7152,0.0722))`、`dark = 1 - smoothstep(0.04, 0.22, luma)`、`reflection = mix(reflection, environmentReflection, 0.34 * dark)`。亮的水面/天空倒影保持原对比度，避免整片水变成均匀乳白蓝。
- 深水色 `(0.008,0.035,0.055)`→`(0.016,0.058,0.075)`，浅水色 `(0.045,0.150,0.160)`→`(0.050,0.160,0.167)`；`0.55+0.45*sunDiffuse` 的方向光调制不变。
- 远水 alpha 上限 `0.92`→`0.89`，保留少量背景透出。

**实测（Debug / RTX 2070，体素世界，临时低机位 [16,8,140]、pitch 0.16、yaw 180，waveStrength=0、waveSpeed=0、平面反射/SSR 开）**：
- 修改前 `build/voxel_water_before2.png`，修改后 `build/voxel_water_v2.png`；同机位裁切对比 `build/water_tune_compare_v2.png`（上=修改前，下=修改后）。
- 水面区域（y600..1000、x100..1800）mean RGB `(62.5,78.5,94.2)`→`(66.4,84.5,99.6)`，luma `76.2`→`81.8`；全画面 diff>2 共 **840553** 像素，diff>8 共 543093。
- 岛体倒影仍可见，但暗部被抬起；没有新增噪点/条带，Validation 0/0。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-164734`；DocCheck 通过。

**边界**：这是观感调参，不是物理吸收/散射模型；只提升暗反射，不改变反射几何与 SSR/平面反射的取舍；仍无折射/水下后处理。用户原始截图机位未保存，本条用体素低机位复现同类“远水暗色平板”，不代表该截图逐像素一致。

## R-09：低渲染倍率水面平铺亮暗格（2026-11-06）

**问题**：用户 m16832 在 1536x864 窗口（3D 视口约 710x570）看到水面有规则的斜向亮暗格/菱形条带，移动视角时更明显；同一相机在 1920 全分辨率渲染却几乎看不到。

**根因**：`Resources/Texture/Water/water_normal.png` 是 18m 平铺的低频 FFT 法线，低频脊线在每个平铺块里重复。低渲染倍率（`--render-scale 0.37`，约等于用户 710px 宽视口）下，每个 18m 块只占约 43px，重复的脊线被放大成规则亮暗格；不是 SSR、平面反射或 TAA 的缺陷。对照：`waveStrength=0` 时网格完全消失，说明来源在水波法线/波浪，不在反射。

**实现**：把法线贴图换成**高通 FFT 法线**，清掉 K<32 的频段，只保留 18m 平铺里约 0.56m 以下的细纹；低频形状交给 4 个解析大波。`shaders/water.frag`、两层 18m/42m、`textureGrad`、`tileableMips` 全部保持 HEAD 不变，所以这是单文件资产替换。新增 `tools/GenerateWaterNormal.py`（1024²、cutoff=32、exponent=1.9、slope_rms=0.22、seed=44，默认输出该资产；重新生成与仓库内文件逐字节一致）。

**实测（Debug / RTX 2070，体素世界，相机 [16,8,140]、pitch 0.16、yaw 180，waveStrength=0.83，平面反射/SSR 开，TAA 关）**：
- `--render-scale 0.37`：修改前 `build/grid_before_lowres.png` 有明显斜向亮暗格；修改后 `build/grid_hp32_head_lowres.png` 为细碎自然波纹，无规则格。
- 全 1920：`build/grid_hp32_head_full.png` 正常，和原 HEAD 水面观感一致。
- 对比图 `build/water_grid_fix_compare.png`（上=HEAD 低频法线，下=高通法线）。
- 排除项：三层 18/23/37m + 旋转、各向同性法线、高通 K<8/16 在 0.37x 下仍残留重复亮斑；只有 K>=32 的高通彻底消除。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-173309`；DocCheck PASS（documents=68 lines=11930）。

**边界**：这是针对“18m 法线低频在低渲染倍率下重复”的资产级修复，不是通用反走样；极近处仍可能看出 18m 周期；高通后太阳细碎高光仍在，但不再按平铺成块；大波轮廓、反射与 Fresnel 不变。

## R-10：高角度水面泡沫规则菱格（2026-11-06）

**问题**：用户 m17600 从高角度俯视水面时仍能看到规则的亮暗菱形/棋盘格，低角度近处的水面在 R-09 之后已经正常。红框里是水面上成排的白色小菱形簇，不是反射里的物体，也不是用户手绘的红色标注本身。

**根因（逐项禁用确认）**：在同一机位分别关掉 SSAO、TAA、平面反射、SSR、环境反射、太阳高光，并把水面 alpha 强制为 1，菱形格都还在；只注释掉泡沫混合 `color = mix(color, vec3(0.82,0.90,0.94), foam*0.55)`，水面立刻完全平滑。旧泡沫有两个问题：
- `crest`（波峰项）没有乘 `shore`，白浪会铺满整个水面，不只在岸边；
- 噪声是 `sin(p.x * 1.7 + t * 0.6) * sin(p.y * 2.1 - t * 0.45)` 的乘积，俯视时形成规则菱形棋盘格。

旧代码（`shaders/water.frag` 约 229-236 行）：

```glsl
const float shore = 1.0 - smoothstep(0.0, 1.6, thickness);
const float crest = smoothstep(0.08, 0.26, bigWaveHeight(p, t, strength));
const float foamPattern = 0.55 + 0.45 * sin(p.x * 1.7 + t * 0.6)
                                      * sin(p.y * 2.1 - t * 0.45);
const float foam = clamp((shore * 0.90 + crest * 0.25) * foamPattern, 0.0, 1.0);
```

**实现**：
- `shore = 1 - smoothstep(0, 1.8, thickness)`，泡沫只在水陆交界 1.8m 内出现；
- `crest = smoothstep(0.10, 0.30, bigWaveHeight)`，波峰项也乘 `shore`，深水不再有泡沫；
- 噪声改采已有的 `waterNormalMap` 低频（`textureLod(waterNormalMap, p/7 + t*(0.011,-0.008), 1.5).x`），`foamMask = smoothstep(0.34, 0.74, foamNoise)`；非周期纹理打散泡沫，不新增资产；
- `foam = clamp(shore * foamMask * (0.75 + 0.25 * crest), 0, 1)`，颜色混合量与 alpha 抬高逻辑不变。

**实测（Debug / RTX 2070，1920x1080，--streaming-world 1 --selftest 40 --screenshot-frame 30 --hidden）**：
- 相机 `[-11.84165, 59.89929, 60.03920]`、rotation `[0.66149, -6.29172, 0]`，`waveStrength=0.83`、`waveSpeed=0.95`，平面反射/SSR 开，TAA/Bloom 关；
- 修复前 `build/highgrid_C_before_samecamera.png`：水面铺满约 4x4 白点的规则菱形簇；
- 修复后 `build/highgrid_C_after_samecamera.png`：同机位/参数，菱形簇消失，只剩连续波纹与云影反射；
- 关平面反射/SSR 的对照 `build/highgrid_C_none.png` 也平滑，`[Validation] errors=0 warnings=0`；
- 对比图 `build/foam_grid_compare.png`（上=修复前，下=修复后，已裁掉左侧 UI）；
- 低角度相机 y=14、pitch=0.30 的检查图 `build/foam_lowshore.png`：水面同样没有菱形格；该机位岸线白边很弱，因为泡沫只在 1.8m 岸带内且受噪声门控，不保证每条岸线都有明显白沫。

**回归**：全套 **75 项** ALL PASS，stderr 全 0，验证日志 `build/validation/20261106-180307`；DocCheck PASS。

**边界**：泡沫仍是观感模型，不是白浪频谱/破碎波模拟；没有独立的泡沫贴图，只借用 18m 水波法线的低频；泡沫只覆盖岸边，湖心/深海不会出现白浪。用户原始截图机位来自当时的 `config/engine.json`，本条用同一相机复现高角度水面，不代表该截图逐像素一致。
