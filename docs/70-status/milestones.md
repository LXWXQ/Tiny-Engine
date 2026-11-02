---
id: status/milestones
title: 里程碑与实测基线
type: status
scope: 空
read-when: 对照历史进度、或需要"已知良好基线数字"
size: ~520 行
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
| 回归 | 27 项全 PASS，stderr 全 0 字节 | `tools/Validate.ps1` |
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

**实现**：
- `Model` 为可批模型保留一份完整 CPU 顶点/索引（`getCpuVertices()/getCpuIndices()`）；地形模型（含 `isTerrainChunk` 子网格）不留，它们本来不走 GPU 批；
- `SceneGpuBatch::ensureMergedMesh()`：按模型集合算 FNV 签名（指针 + 顶点数 + 索引数），变了才重建；顶点/索引分别拼进一份 device-local 缓冲，索引保持原样，每个模型记 `baseVertex`；
- 绘制时把 `baseVertex` 填进间接命令的 `vertexOffset`（等价 `vkCmdDrawIndexed` 语义），所有实例共用 `ranges[0]`/`counts[0]`，一条 `vkCmdDrawIndexedIndirectCount` 画完；
- 重建走自建 `uploadPool` + staging + `cmdBeginSingleTime/cmdEndSingleTime`，屏障 TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ|INDEX_READ；
- 回退：任一可批模型没有 CPU 几何 -> `ensureMergedMesh` 返回 false，退回 F1-a 每模型路径；`--no-merged-buffers` 可强制回退。

**可观测**：`[SceneGpu] merged buffers built | vertices=192496 indices=786801 vertexMB=8 indexMB=3`；相机朝向 Sponza 时遥测 `draw calls 2 / triangles 270459 / vertices 811377`（合并前 104）。

**回归**：`Validate.ps1` 新增 `f1b-merged-buffers` 与 `f1b-merged-off`，全套 24 项 ALL PASS；三路像素比对（合并 / 每模型 / CPU）相机 `[0,6,45]` 下 `max diff = 0`。

**性能**：300 帧两轮 cpu 185.2/209.6、merged 209.6/227.6、per-model 218.9/219.6 —— 同模式轮间波动 24 FPS，仍以噪声为主；确定收益是 draw call 数 104 -> 2，不是本机 FPS。

## F1-c：按组实例化 + 材质排序 + Hi-Z 遮挡剔除
**实现**：分组键改成 `(模型, 子网格)`（实例连续，`instanceCount = N`，一条命令画完所有实例）；去掉 counters 缓冲（组自己那 5 个 uint 直接写，被剔除写 0）；组按并集世界 AABB 剔除；合并模式按 `materialSlot` 排序、非合并按 `(model, materialSlot)`；包围盒缓存改用 `Model::getGeometryId()`（600 帧/64 条淘汰）；Hi-Z 金字塔从 `GBuffer.Position` 反算线性视深度（`R32_SFLOAT`、`floor(log2(max(w,h)))+1` 级、布局恒 GENERAL、手动屏障），`HiZBuild` 挂在 `Geometry` 之后写另一槽（错帧一帧），剔除时投影成屏幕矩形、按矩形像素选级、4 角 `textureLod` 取最大值，`nearest > farthest + 0.05` 判遮挡；`--no-hiz` 可关。
**可观测与回归**：`[SceneGpu] cull stats | groups=103 visible=103 culled=0 instances=103 hiz=1 hizLevels=11 hizEnabled=1 hizTests=103 hizCulled=0`（每 60 帧一行）；新增 `f1c-hiz-off`，全套 25 项 ALL PASS、stderr 0 字节；`--resize-test` / `--render-scale 0.5` 验证层 0/0（顺带修掉 G-Buffer 重建后遗留旧 `VkImageView` 导致描述符写入崩溃、录制期 `vkUpdateDescriptorSets` 让命令缓冲失效两个 bug）。

**Hi-Z 有效性**：当前 `hizCulled = 0` 是正确结果 —— Sponza 的 103 个子网格各自覆盖整栋建筑一大块，组级保守剔除几乎不命中；把 `kOcclusionBias` 临时改成 -100000 可验证全链路（`hizTests=103 hizCulled=103`、`visible=0`）。收益等大量小实例（粒子/植被/体素 chunk）。
## F1-d：稳定帧快速路径 + 同步校验 0 错误
**问题**：从 F1-a 起每帧都重新摊平实例（世界矩阵 + 每组并集 AABB），静态场景实测 439µs/帧；`--validation-features sync` 跑出 180 条同步危害，全部来自 Hi-Z 的跨帧读写与屏障。
**实现**：结构签名（FNV-1a：entity、`sourceRevision`、材质槽、材质指针、`getGeometryId()`、子网格数、局部 AABB 六个 float、`gpuMergedBuffers`）；签名命中就跳过 `buildInstances()` 与 `instances/groups` 上传，只写 240 字节 `frameData` 再 dispatch；每槽位记 `(signature, capacityGeneration)`，容量重建后两槽都重传。Hi-Z 起始屏障改成 `GENERAL→GENERAL` + `srcAccess = SHADER_READ`（等上一帧剔除的采样），逐级屏障给下一级 `SHADER_READ`。
**可观测**：`[SceneGpu] cull stats | ... stable=60/60 rebuilds=0 flattenUs=9`（重建帧 439）；`--validation-features sync` → `[Validation] errors=0 warnings=0`。
**回归**：新增 `f1d-stable-path`（要求 `stable=`/`rebuilds=`/`flattenUs=`）与 `f1d-sync-validation`（sync 0 错误），全套 **27 项 ALL PASS**、stderr 0 字节。
