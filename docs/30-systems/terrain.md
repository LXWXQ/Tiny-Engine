---
id: systems/terrain
title: 程序化地形
type: system
scope: src/Terrain/, src/Render/TerrainGpuMeshBuilder.*, Scene 地形生成接口, shaders/terrain_mesh.comp（材质与 Set2 见 terrain-materials）
read-when: 改地形生成算法、网格规则、顶点色、创建/重建流程、地形存读或地形自检
size: ~590 行
updated: 2026-11-13
---

# 程序化地形

## 1. 范围与边界

当前实现同时支持**单实体地形**与 **B-01 独立 chunk 实体流送**；两者共用 GPU
高度场、拓扑级边界缝合、可选裙边回退和四层 PBR 材质。B-04 起单 chunk 网格由
GPU compute 组装，多 chunk 仍保留 CPU builder 作为兼容/回退路径：

- 默认 `257 x 257`，上限 `257 x 257`；Inspector 仍可降到低分辨率；
- 创建/Generate 是显式操作，不在每帧或每次拖参数时重建；高度场通过
  8 槽 GPU compute 异步提交/轮询生成并读回，CPU 网格构建提交给 JobSystem，
  主线程只轮询 fence、做 Model 上传与 ECS 安装；
- 单个 Model 内含 `chunkGridX * chunkGridZ * lodLevelCount` 个连续索引子网格；
  每个 chunk/LOD 带局部 AABB。B-03 起由 pre-graphics compute pass 做 world AABB
  视锥剔除、15% 滞回选层和 0.24s 抖动式双 LOD 过渡，并直接写 indirect 命令；
- B-02 起每个 chunk/LOD 的外边界固定使用全高度场分辨率，内部保持 LOD 粗
  网格；四条过渡带把外边界与内部粗网格焊成一张无洞索引网格。相邻 chunk
  无论 LOD 组合如何都共享同一组边界顶点，因此不依赖裙边；
- `skirtsEnabled` 默认 true，保留向下 `heightScale * 0.08` 裙边作为保守回退；
  关闭后仍应无裂缝，`--terrain-test` 与流送 smoke 均以 skirts=off 运行；
- GPU selection 为每个 terrain entity 预留 32 条 indirect command，compute
  用 atomic count 追加稳定/过渡命令；graphics 侧每实体一次
  `vkCmdDrawIndexedIndirectCount`，draw count 不读回 CPU；
- 过渡命令的 blend/mode 编码进 `firstInstance`，再由 `mrt.vert` 解成 flat
  varying；这样 indirect 合批不再牺牲逐 chunk cross-fade；
- B-04 起 `chunkGridX/Z == 1` 的 terrain 由 `terrain_mesh.comp` 直接写
  device-local 顶点/索引 buffer；`Model::createFromGpuMesh()` 接管 VMA
  allocation，渲染不再经过 CPU staging copy。F2-e 起 CPU 侧不再保留 picking
  mirror，拾取直接走 HeightField；
- 复用现有 `Model` / `Scene` / `GeometrySystem` / G-Buffer，不新增 RenderPass 或
  RenderQueue；阶段 6 后续新增**专用 terrain descriptor sets（最多 4 套）**，不占 bindless
  材质槽；
- 阶段 6 增加 height curve / island mask / ridge sharpness / detail frequency，
  以及按高度坡度混合的沙地/草地/岩石/雪 albedo + normal + height + roughness；
- set 0 纹理由 `TerrainTextureGenerator` 启动时确定性生成；set 1~3 可按需加载外部资产；
- 提供 Natural / Arid / Alpine / Lush 四种 Surface Style；
- 支持 `Resources/Texture/Terrain/` 与 `setN/` 的 16 张 PNG 按需加载，缺任一张则该 set 回退程序化纹理；
- 手动创建的地形仍是单实体、单 Model；`createStreamingTerrain()` 可创建
  manager + 多个独立 chunk 实体，每个 chunk 拥有自己的 Model/Mesh/Bounds，
  在相机周围有限半径内加载/卸载；
- GPU compute 已负责高度场噪声与单 chunk 顶点/索引组装；多 chunk 手动地形仍由
  CPU builder 生成。LOD 选择/剔除走 GPU selection；F2-h 起流送半径上限 24（`--streaming-world`
  最大 2401 块），chunk 坐标是 `int64`、采样锚点是 `double`，噪声本身仍是 `float`（见 §7 精度）。
- F2-a 起有可持续运行的载体：`--streaming-world [radius]` 在构造期建立 manager，
  半径 r 时 (2r+1)^2 个 chunk 实体；`--streaming-fly [speed]` 让相机沿 +X 直线
  巡航（F2-c 起，`--streaming-fly-radius` 控制侧向摆动幅度），使加载/卸载持续发生。
- F2-d 起半径上限 8 → 12（625 块）：地形 GPU 选择槽位 64 → 384（只受同屏可见
  chunk 数约束）、命令区 4096 → 12288，arena 96 → 384 槽（2048 顶点 / 12288 索引，
  约 136 KB/槽、合计约 51 MB）；新增每帧流送时间预算 `maxUpdateMs`（默认 2 ms，
  `--streaming-budget` 可调），慢帧不再一次生成整环。
- F2-b 起同一帧的地形 chunk 由**一条** indirect 提交：每个 chunk 的顶点/索引放进
  `TerrainMeshArena` 里自己的切片（F2-g 起按实际尺寸分配，见下），间接命令里带
  `firstIndex`（arena 索引基址）、`vertexOffset`（arena 顶点基址）与
  `firstInstance`（该 chunk 在 set 3 实例表里的行号）。chunk 的世界矩阵不再烘进顶点，
  而是在顶点着色器里按 `gl_InstanceIndex` 从实例表取 —— 同一份网格被移动时不需要重拷。
  命令区固定为 16384 条（1024 实体 x 16 命令，F2-h 起），绘制前用 `vkCmdFillBuffer` 清零，
  `indexCount == 0` 的条目就是空绘制。`--no-terrain-batch` 回退到逐 chunk 绑定 + 逐 chunk draw。
- F2-f 起 arena 归 `Scene` 所有（`Scene::getTerrainMeshArena()`），地形网格生成完成、
  `Model` 接管完 VMA allocation 后立刻在一次性命令缓冲里 `acquire()` + `copyInto()` + 
  `releaseGpuBuffers()`：chunk 只剩子网格/包围盒/UV 这些元数据，VBO/IBO 全部落在共享 arena。
  实体销毁时把切片交还（`Scene::destroyEntity()`）。
- F2-g 起 arena 改成**按实际尺寸分配**：两块 device-local 大缓冲（顶点 1.1M ≈ 46 MB、
  索引 5.9M ≈ 22.5 MB），维护有序空闲区间表做首次适配 + 前后合并；`acquire()` 返回
  `Slice{vertexOffset, vertexCount, indexOffset, indexCount}`，`copyInto()` 按切片拷贝，
  `release()` 归还并合并。单块超过剩余容量时该 chunk **保留自己的 buffer**，
  渲染时照旧 `Model::bind()`，所以不会丢几何，只是少了合批；`--no-terrain-batch` 同样只绑 arena。
- F2-h 起 **chunk 坐标换成 `int64`**（`TerrainChunkCoord{x, z}` + 64 位哈希，`TerrainChunkComponent::chunkX/Z`
  同步改宽），采样锚点 `TerrainConfig::sampleOriginX/Z` 与 `HeightField` 的对应成员改成 `double`，
  校验范围放宽到 ±1e12；rebase 只平移 Transform 与 chunk 整数索引/key，锚点保持绝对。
  arena 容量不再写死：`Scene::createStreamingTerrain()` 按环尺寸算 `ring*1700` 顶点 / `ring*9000` 索引，
  容量不足时用 `TerrainMeshArena::reconfigure()` **原地重建**（渲染侧只持 `this` 指针，不能换对象），
  半径 12 仍是 1.1M/5.9M，半径 24 是 4.08M/21.6M（约 171 MB + 82 MB）。

## 2. 文件与职责

| 文件 | 职责 |
|---|---|
| `src/Terrain/TerrainConfig.h` | 纯数据参数 + `validate()`；不依赖 Vulkan/EnTT |
| `src/Terrain/TerrainNoise.*` | 无全局状态 2D Perlin + fBm |
| `src/Terrain/TerrainHeightField.*` | 规则网格高度场、世界坐标、双线性采样 |
| `src/Terrain/TerrainMeshBuilder.*` | 顶点/索引/法线/UV/顶点色，输出 chunk 索引区间与局部 AABB；仍不依赖 Vulkan |
| `src/Terrain/TerrainTextureGenerator.*` | 确定性、可平铺的 4 层 albedo/normal/height/roughness CPU 纹素生成 |
| `src/Terrain/TerrainSelfTest.*` | CPU 算法、网格与纹理缓冲不变量的进程内回归 |
| `src/Render/TerrainComputeGenerator.*` | 8 槽异步 GPU 高度场 pipeline、storage buffer、submit/poll/release 与 CPU 参考自检 |
| `shaders/terrain_height.comp` | 与 `TerrainNoise.cpp` 对齐的 GPU Perlin/fBm/ridge/detail/mask/height-curve 计算 |
| `src/Render/TerrainGpuMeshBuilder.*` | 单 chunk 的 GPU 顶点/索引组装、device-local output buffer、parity selfTest |
| `shaders/terrain_mesh.comp` | B-02 拓扑 + 可选裙边的 GPU mesh 组装；11-float tight Vertex 与 uint index 输出 |
| `src/Render/TerrainMaterial.*` | 优先加载 16 张外部 PNG，否则生成程序化纹理并更新专用 descriptor set |
| `src/Render/SubMeshCulling.*` | CPU 参考 frustum-AABB、世界 bounds、LOD/滞回/过渡状态机；也供 parity selfTest |
| `src/Render/GeometrySystem.*` | GPU selection 资源生命周期、per-entity indirect-count draw、CPU fallback、地形 Set2 绑定 |
| `src/Render/GeometrySystemGpu.cpp` | chunk record 上传、GPU selection dispatch、GPU/CPU selection parity、过渡相机/LOD 自检 |
| `shaders/terrain_lod.comp` | GPU frustum cull、LOD 滞回/过渡状态、indirect command/count/stat 输出 |
| `src/Core/JobSystem.*` | 地形 CPU 构建的 worker 队列；Scene 每帧轮询完成状态 |
| `src/Core/DescriptorManager.*` | set 2 terrain layout/set 的创建与更新 |
| `src/Scene/Scene.*` | GPU mesh / CPU Builder -> `Model` -> ECS；失败保留旧网格 |
| `src/Scene/Components.h` | `TerrainComponent`、`TerrainStreamingComponent`、`TerrainChunkComponent`、`RenderTagComponent::isTerrain` |
| `src/Editor/Commands.*` | 创建 / 改参数 / 重建三个可撤销命令 |
| `src/Editor/panels/InspectorPanel.*` | Terrain 参数分组、Generate/Reset/状态 |
| `src/Editor/SceneSerializer.cpp` | 只存参数，读盘时重新生成 |

CPU 模块不 include `Scene/Model.h` 或任何 Vulkan 头。同步入口
`Scene::buildTerrainModel()` 仍在当前线程完成 CPU + staging 上传；异步入口只把
`TerrainMeshData` 的生成放到 worker，GPU 上传必须回到主线程。

## 3. TerrainConfig

| 字段 | 默认 | 校验 |
|---|---:|---|
| `seed` | 12345 | uint32，任意值 |
| `resolutionX/Z` | 257/257 | `[2, 257]` |
| `sizeX/Z` | 100/100 | finite, `(0, 10000]` |
| `baseHeight` | 0 | finite, `[-10000, 10000]` |
| `heightScale` | 25 | finite, `[0, 1000]` |
| `noiseFrequency` | 0.01 | finite, `[0.0001, 10]` |
| `octaveCount` | 6 | `[1, 8]` |
| `persistence` | 0.5 | finite, `[0.05, 0.95]` |
| `lacunarity` | 2.0 | finite, `[1.1, 4.0]` |
| `ridgeWeight` | 0 | finite, `[0, 1]` |
| `detailWeight` | 0.25 | finite, `[0, 1]` |
| `edgeFalloff` | 0 | finite, `[0, 1]` |
| `heightCurve` | 1.0 | finite, `[0.25, 4]`；大于 1 压低谷地、锐化峰顶 |
| `islandWeight` | 0 | finite, `[0, 1]`；可选径向岛屿遮罩 |
| `islandRadius` | 0.8 | finite, `[0.1, 1]`；遮罩内部平台半径 |
| `ridgeSharpness` | 1.0 | finite, `[0.25, 4]`；ridge 指数 |
| `detailFrequency` | 2.37 | finite, `[1, 8]`；fBm detail 层频率倍率 |
| `materialDetailStrength` | 0.65 | finite, `[0, 1]`；纹理层的法线强度/宏观明暗变化 |
| `uvTiling` | 1 | finite, `[0.01, 100]`；世界空间纹理重复倍率，shader-only |
| `sampleOriginX/Z` | 0 / 0 | finite `[-1e7, 1e7]`；噪声采样域世界中心，流送 chunk 用 |
| `localColorRange` | true | 顶点色是否按局部 min/max 拉伸；流送 chunk 必须 false |
| `skirtsEnabled` | true | 是否追加向下裙边；几何参数，B-02 缝合后可关闭做 A/B |
| `chunkGridX` / `chunkGridZ` | 2 / 2 | `[1, 4]`；单个 Model 内的逻辑 chunk 数，几何参数 |
| `lodLevelCount` | 3 | `[1, 3]`；每个 chunk 预生成的 LOD 层级，几何参数 |
| `lodDistanceScale` | 1.0 | finite `[0.25, 4]`；LOD 切换距离倍率，渲染参数，无需 Generate |
| `surfaceStyle` | Natural | `[0, 3]`；Natural/Arid/Alpine/Lush 材质变体，shader-only |

`validate()` 在生成入口被调用，错误文本直接进 `TerrainComponent::lastError`。
顶点数和索引数在 `size_t` 中计算，并确认能放进 32 位索引。

## 4. 噪声与高度场

- `seed` 只参与整数 hash；绝不调用 `rand()`，同 seed/坐标逐元素可复现。
- 生产生成路径优先用 `TerrainComputeGenerator` 在 GPU 上计算 normalized
  heightfield；compute shader 与 CPU `TerrainNoise.cpp` 使用同一整数 mixer、
  梯度表、octave seed、ridge/detail 调制和 island/edge mask。GPU 输出经
  host-visible readback 填入 `HeightField`，`generatedOnGpu` 标记来源。
- 异步 Generate/流送使用 8 个独立 slot：`submit()` 只记录并提交 compute，
  `poll()` 在后续帧检查 fence；完成的请求才进入 CPU mesh job。覆盖/卸载请求
  标记 abandoned，fence 完成后回收 slot。`generate()` 保留给同步场景加载，
  内部是 submit+poll 的阻塞封装。
- GPU 不可用或 compute 失败时自动回退 CPU `HeightField::build()`；两种路径
  都产出相同布局的归一化高度网格。
- 每 octave 用整数 mixer 派生本地 seed；`ridgeWeight` 在 Perlin 与
  `pow(1 - |n|, ridgeSharpness)` 之间插值；`detailWeight` 只从第二 octave
  起加一层高频扰动，`detailFrequency` 控制该层频率。
- fBm 先按权重和归一化，再映射到 `[0, 1]`；随后应用可选的 `heightCurve`。
- `islandWeight > 0` 时，径向遮罩在 `islandRadius` 内保持原高度，向外平滑
  衰减到边界 0；权重为 0 时完全不生效。
- 世界坐标：

```text
x = -sizeX/2 + i * sizeX/(resX-1)
z = -sizeZ/2 + j * sizeZ/(resZ-1)
y = baseHeight + heightScale * h(i,j)
```

`edgeFalloff > 0` 时才启用方形边缘 mask；它与可选的径向 `islandWeight`
相互独立：默认 0，不会偷偷把边界压平。最终高度始终 clamp 到 `[0, 1]`。

## 5. 网格规则

- 每个 chunk 在每个 LOD 下独立生成顶点/索引切片，但都存放在同一个 Model
  的顶点/索引缓冲里；`TerrainSubMesh` 记录 `firstIndex/indexCount`、
  `surfaceIndexCount`、chunk 坐标、LOD 和局部 AABB。
- LOD0 的 chunk 内部也是完整网格；LOD1/LOD2 的内部按 2/4 个 cell 的步长
  重新采样高度场。
- **边界固定全分辨率**：每个 LOD 的 chunk 外环逐 cell 采样高度场，四条边
  与相邻 chunk 的边界完全重合，包括位置、法线、UV 和顶点色（关闭
  `localColorRange` 时）。
- **过渡环带**：外环与向内缩 `step` 的粗网格内环之间按弧长参数配对三角形；
  内环再以粗网格填充。外侧/内侧共享索引，四角焊在同一顶点上，不产生
  T-junction 或洞。`stitchPolylines()` 中按归一化段参数推进，任一时刻只
  添加覆盖当前四边形的三角形。
- `skirtsEnabled=true` 时额外生成一圈向下 `heightScale * 0.08` 的裙边；
  裙边索引不计入 `surfaceIndexCount`，拾取只用 LOD0 surface。关闭裙边时
  `surfaceIndexCount == indexCount`。
- 绕序固定为 `(a,b,c)` / `(b,d,c)`，从 `+Y` 看逆时针；过渡环带也通过
  `windingFacesPositiveY` 断言，不允许负 Y 面积三角形。
- 当前管线 `cullMode = NONE`；surface 仍保留背面剔除正确的 +Y CCW 绕序。
- 法线对世界坐标高度做中心差分，粗 LOD 也在相同世界尺度上采样：

```text
dhdx = (h[i+1,j] - h[i-1,j]) / (2*dx)
dhdz = (h[i,j+1] - h[i,j-1]) / (2*dz)
normal = normalize(vec3(-dWorldHeight/dx, 1, -dWorldHeight/dz))
```

- UV 按世界尺寸和 `uvTiling` 生成，不绑定分辨率。
- 顶点色按**当前高度场局部范围**归一化后的高度和坡度混合
  沙地 / 草地 / 岩石 / 雪；颜色先做 sRGB -> linear。局部归一化是为了让默认
  Perlin 的窄幅高度范围也能出现沙地和雪线，而不是整片草地。阈值仍是 MVP 常量。
  这条是 CPU 顶点色路径；片元着色器的沙/草/岩/雪层权重用全局范围，见 §7.1，
  两条路径不能混用。

### 5.1 GPU 网格组装与异步上传（B-04/B-05）

`TerrainGpuMeshBuilder` 只对 `chunkGridX/Z == 1` 生效，服务流送 chunk 与单
chunk 手动地形；多 chunk 继续走 CPU `TerrainMeshBuilder`。

- compute 用 push constant 分两次 dispatch：一次写顶点、一次写索引；顶点 SSBO
  使用 11 个连续 float（position3/color3/normal3/uv2），匹配 C++ `Vertex` 的
  44-byte tight 布局，避免 `std430` 的 `vec3` 16-byte 对齐；
- 索引顺序与 CPU builder 对齐：内部粗网格四边形 -> 上/右/下/左 transition
  band -> 可选裙边环；LOD0 完整，LOD1/2 内部按 2/4 cell 步长；法线、顶点色、
  UV、局部/绝对色域、chunk 外环与 skirt 深度均由 GPU 复算；
- 输出 buffer 同时带 storage、vertex/index 与 transfer-src usage；自检只读回
  比较，生产路径直接作为 `Model` 的 VBO/IBO；`createFromGpuMesh()` 接管 VMA
  allocation 并统一销毁；
- B-05 起使用 4 个异步 slot：`submit()` 准备 buffer、写 height/params、录制
  compute 并 `vkQueueSubmit`，不在主线程等 fence；`poll()` 用
  `vkGetFenceStatus`，`release()/collectFinished()` 回收放弃请求，`waitAll()`
  用于取消/关闭；
- F2-e 起不再生成全分辨率 picking mirror（原先每 chunk 约 38 KB 的
  `pickingPositions/pickingIndices`，由 `std::async` 与 GPU assembly 并行）；
  地形 raycast 改为沿射线步进采样 `HeightField::worldHeightAt()` 再二分细化；
- 构建失败、设备无 compute 或多 chunk 配置回退 CPU builder + staging；
  `TerrainComponent::meshGeneratedOnGpu` 记录实际来源；
- `createTerrainEntity()`/场景加载仍用阻塞 `build()`；运行时 Generate、重建与
  流送走异步 submit/poll，slot 满时保持 pending，不回退同步组装。

## 6. ECS 与失败语义

`TerrainComponent` 保存 `config`、`generatedConfig`、`revision`、顶点/索引数、
最近生成耗时、错误、`generated`、`generating`，以及生成该网格时使用的
`shared_ptr<terrain::HeightField>` CPU 碰撞源。它**不保存任何 GPU 句柄**；网格仍在
`MeshComponent::model`，包围盒由 `Model` 边界写入 `BoundsComponent`。
碰撞高度场不参与序列化，旧网格被替换时随组件原子更新；撤销/重做 `TerrainComponent`
快照会一并恢复对应高度场。

`Scene::createTerrainEntity()` 先完整生成并上传，成功后才创建实体，避免半成品。
`Scene::regenerateTerrain()` 同样先构建到临时 `Model`：

- 成功：替换 `MeshComponent`、刷新包围盒、`generatedConfig = config`、`revision++`；
- 失败：只写 `lastError`，旧 `MeshComponent` 与 `generatedConfig` 保持不变。

运行时 `Scene::requestRegenerateTerrain()` 走异步流水线：

- 先提交 8 槽 GPU 高度场；完成后单 chunk 提交 4 槽 GPU mesh builder，多
  chunk/失败回退 CPU `JobSystem`；
- 立即置 `TerrainComponent::generating = true`，旧 `MeshComponent` 继续渲染；
- `Scene::update()` 在主线程 poll 高度场/mesh 请求，完成后安装 `Model` 并替换
  `MeshComponent`；slot 满时任务保持 pending；
- 每次请求分配全局递增 serial；新请求置位旧任务 `cancelRequested`，结果与
  回调静默丢弃；回调在主线程执行，编辑器据此构造 `RegenerateTerrainCommand`；
- 被取代/实体已销毁的任务移入 `retiringTerrainBuilds`，等 `JobHandle` 完成才由
  `drainRetiringTerrainBuilds()` 回收（worker 用裸指针写任务字段，不能提前释放）；
- `waitForTerrainGeneration()` 只给自检/诊断；`clearEntities()` 丢弃待完成
  回调，`cleanup()` 先等 worker 结束再销毁 Scene/JobSystem 依赖。

### 6.1 独立 chunk 实体与流送基础

`Scene::createStreamingTerrain(chunkConfig, loadRadius, maxLoadsPerFrame, name)`
创建：

```text
manager entity
  Transform + Hierarchy + Name + TerrainStreamingComponent
chunk entities (runtime)
  Transform + TerrainComponent + MeshComponent + Bounds
  + RenderTag(isTerrain) + TerrainChunkComponent
```

规则：

- 每个 chunk 的 `chunkGridX/Z` 强制为 1，`lodLevelCount` 保留模板值；
- chunk 中心 `((x+0.5)*sizeX, 0, (z+0.5)*sizeZ)`，同时写入
  `config.sampleOriginX/Z`，因此独立生成的噪声高度在边界连续；
- `localColorRange=false`，避免各 chunk 按自己的 min/max 拉伸顶点色产生色带；
- 相机所在 chunk 为中心，Chebyshev 半径内按距离排序加载；
- `maxLoadsPerFrame` 控制每帧新请求数（上限 16），`maxUpdateMs` 是同一帧里的
  生成时间预算（默认 2 ms），先到者生效；卸载半径 = loadRadius + 1；
- 加载中的 chunk 先有 TerrainComponent、没有 MeshComponent；GPU 高度场完成后
  优先走 `TerrainGpuMeshBuilder` 组装并安装 Mesh/Bounds；GPU mesh 不可用或
  构建失败时再提交 CPU JobSystem fallback；
- manager 只序列化模板参数，`TerrainChunkComponent` 实体不逐项保存；加载场景
  后围绕相机重新建立 chunk 集。

当前流送使用 8 槽异步 GPU 高度场请求；单 chunk GPU mesh 组装使用 4 槽
异步 submit/poll，主线程只做提交与轮询，完成后再安装 Model。`createTerrainEntity()`
与场景加载仍是同步路径。F2-h 起半径上限 24（2401 块）。**性能数字必须注明构建类型**：Debug 下半径 24 只有 17 FPS（没优化时 `updateVisibility` 一段就占 12.9 ms），Release 下同一场景 **208 FPS**，`--update-profile` 给出 `Scene::update()` 全程约 62 µs/帧（genPoll 0.4 / streaming 0.4 / world 0.06 / spatial 0.06 / visibility 61.5 µs），CPU 只占帧时间约 1%。F2-i 把可见性从 `SpatialIndex::query(broadBounds)` 换成对 `MeshComponent` 的线性扫描（旧路径在小 cell 上超过 4096 个 cell 会退化成 `collectAll()`，按 cell 重复展开再排序去重），并给流送扫描加了稳态早退（相机没换 chunk、环已填满、无待办时整趟跳过，70 µs → 0.4 µs）。

编辑器语义：

- Viewport 射线先命中 runtime chunk，再通过
  `Scene::resolveTerrainChunkOwner()` 解析到 manager，避免选中会在流送中消失的实体；
- Hierarchy 隐藏 `TerrainChunkComponent`，Inspector 对 manager 显示半径、预算、
  mapped chunks、in-flight GPU 请求与 update ms；
- 删除 manager 会级联销毁 chunk；`DeleteEntityCommand` 恢复 manager 后重新
  adopt 并流送 chunk，redo 删除重建后的新实体 ID；
- `terrainWorld` 已覆盖真实 JSON 文件 save/load smoke；
- Debug/本机跨 4 chunk 移动样例 `maxStreamingUpdateMs=0.6123`（非稳定性能门槛）。

### 6.2 CPU 碰撞查询

`Scene` 提供三个地形查询入口，供编辑器、角色控制或后续物理桥接使用：

| API | 复杂度 | 说明 |
|---|---|---|
| `sampleTerrainHeightLocal()` | O(1) | 在高度场局部 XZ 上双线性采样；越界返回 false |
| `sampleTerrainHeightWorld()` | O(LOD0 三角形) | 从世界空间包围盒上方垂直向下 raycast，支持实体旋转/缩放 |
| `raycastTerrain()` | O(LOD0 三角形) | 任意世界空间射线；返回最近命中点和世界距离 |

世界空间结果与渲染使用的 LOD0 表面一致；`sampleTerrainHeightWorld()` 是正确性优先的
精确路径，连续大量采样时应优先把位置变换到局部空间并使用 O(1) 查询。

B-07 的 `sculptTerrain()` 通过 XZ region 查询只重建相交 chunk；角色控制器用
`sampleTerrainGroundHeightWorld()` 做局部包围盒 + 双线性贴地，`raycastTerrainWorld()`
供编辑器笔刷取最近地形命中。

地形实体没有 `MaterialComponent`，也不占用 bindless 材质槽。
`RenderTagComponent::isTerrain = true`，`GeometrySystem` 不打印 fallback 警告；
地形材质统一使用启动时创建的 set 2 纹理集。

## 7. 渲染

### 7.1 LOD 运行时与 GPU selection

B-03 起 GPU selection 在 `GeometryPass` 之前的无附件
`TerrainGpuSelection` pass 中执行。它仍录制在 graphics command buffer 上，
因此 dispatch 位于 dynamic rendering 之外，也不跨 compute queue。

每帧 CPU 只上传 chunk record：

```text
worldMatrix, local bounds, distanceScale, lodCount
commandBase, stateIndex, reset, entitySlot
lod0/lod1/lod2 firstIndex+indexCount
```

容量为 64 terrain entities、1024 chunk records。compute 对每条 record：

1. 用 world AABB + view-projection 六平面做 frustum cull；
2. 计算相机到 AABB 的最近距离与世界 chunk extent；
3. 在 per-frame-slot SSBO 中维护 `stableLod/from/to/elapsed`；
4. 执行与 `SubMeshCulling` 相同的 LOD 阈值和 15% 滞回；
5. 换层后产生 0.24s from/to 过渡；
6. 用 atomic count 向该 entity 的固定 32-command 区域追加一或两条命令。

Graphics 侧对每个 terrain entity 发一次：

```cpp
vkCmdDrawIndexedIndirectCount(
    cmd, indirectBuffer, entityCommandRegion,
    countBuffer, entitySlot * sizeof(uint32_t),
    /*maxDrawCount=*/32, sizeof(VkDrawIndexedIndirectCommand));
```

draw count 完全由 GPU 写、GPU 读，不回读 CPU。

过渡命令把 `blend` 量化为 16 bit、mode 放进 2 bit，再写入
`VkDrawIndexedIndirectCommand::firstInstance` 的 marker payload。
`mrt.vert` 用 `gl_InstanceIndex` 解码并通过 flat varying 传给 `mrt.frag`；
fragment 仍用同一像素的 Bayer dither 生成互补覆盖：

```text
outgoing: dither >= blend 时 discard
incoming: dither <  blend 时 discard
```

因此稳定帧与过渡帧都可以走 indirect，不再需要过渡直绘分支。

同步与回退：

- compute 写完后用 `SHADER_WRITE -> INDIRECT_COMMAND_READ` barrier 交给 draw；
- 无 `drawIndirectCount` 时自动回退现有 CPU 选层/命令构建路径；
- per-frame 独立 state/count/stats/debug buffer 避免两帧在飞互相覆盖；
- GPU stats（draws、culled chunks、LOD 分布、blend/switch）供 telemetry
  读取上一完成帧；UI 计数允许一帧延迟；
- `--terrain-test` 同时维护 CPU parity mirror，下一帧比较
  visible/lodA/lodB/blend/commandCount/stable/to；不一致会直接让自检失败。
  它还会临时切换 `lodDistanceScale`，强制检查 GPU 过渡输出。

`GeometrySystem` 在 push constant 中写 `terrainMode = 1`、地形全局 Y 范围
（`terrainMinY` / `terrainHeightRange`）、`terrainDetailStrength`、
`terrainUvTiling`、`terrainMaterialStyle`，以及 CPU fallback 的
`terrainLodBlend` / `terrainLodBlendMode`。GPU-selected indirect 命令会优先
从 `firstInstance` payload 覆盖后两者的 CPU fallback 值。`mrt.vert/.frag` 的
push block 同步声明这些字段；`SimplePushConstantData` 用 `offsetof` / `sizeof`
静态断言钉住布局，总大小仍为 208 字节。

地形全局 Y 范围取 `TerrainConfig::baseHeight` 与
`std::max(TerrainConfig::heightScale, 1e-4f)`，**不能**取每个 chunk 的
`BoundsComponent` 局部 min/max。流送 chunk 各自覆盖高度场的一块，局部范围
不同；`mrt.frag` 用 `(fragPosLocal.y - terrainMinY) / terrainHeightRange`
归一化高度后决定沙/草/岩/雪权重，按 chunk 归一化会让同一世界高度在边界两侧
得到不同的 `h`，表现为矩形沙/雪色块。合批实例表（`GeometrySystemGpu`）与
逐 chunk push（`GeometrySystem`）使用同一组全局值；`TerrainMeshBuilder` 的
世界 Y 也是 `baseHeight + heightScale * h`。

`GeometrySystemGpu` 的 `[Terrain] batched draw |` 日志与 `GeometrySystem` 的
`[Terrain] chunk shading |` 日志都会输出 `heightMin` / `heightRange` /
`heightMixed`，用于确认所有 chunk 共用全局范围（`heightMixed=0`）。

### 7.2 GPU 网格与 Model 生命周期

单 chunk 的 v0 路径是：

```text
HeightField readback
  -> TerrainGpuMeshBuilder::build()
  -> terrain_mesh.comp (vertices pass + indices pass)
  -> device-local VBO / IBO
  -> Model::createFromGpuMesh()
  -> MeshComponent / BoundsComponent / GPU selection records
```

`Model::createFromGpuMesh()` 是 B-04 新增的接管入口：调用方把 VMA
`VkBuffer + VmaAllocation` 所有权交给 `Model`，后者不再执行 staging copy，
析构时用同一个 allocator 销毁。失败路径由 `Scene` 显式销毁尚未接管的
buffer，避免 half-built terrain。

CPU 侧仍保留：
- `TerrainComponent::cpuHeightField`：collision、高度采样，以及**地形 raycast**
  （`Scene::raycastTerrain()` 沿射线步进 + 二分，命中点由 `worldHeightAt()` 决定）；
- `SubMesh::firstIndex/indexCount/terrainSurfaceIndexCount/bounds`：GPU selection
  和 indirect draw 所需元数据。

地形 Model 不再持有 CPU 顶点/索引（F2-e）：GPU mesh 组装、同步生成、流送三条
路径都传空的 picking 数组，`Model::getPickingPositions()` 对地形返回空，任何
依赖网格拾取的地形代码都应改用 `Scene::raycastTerrain()`。

地形材质（Set2 的 12 个 sampler、四套材质集的加载/回退、四层 PBR 混合与
Surface Style）单独写在 [terrain-materials](terrain-materials.md)，这里只保留
网格、LOD 与渲染顺序相关内容。

**SSAO 例外**：大面积平滑地形上的 16-tap SSAO 会产生低频干涉条纹。
`outNormal.w` 因此定义为 SSAO policy mask：地形写 `0`，普通几何写 `1`，
`ssao.frag` 对 `w < 0.5` 的像素输出无遮蔽（1.0）。这比让 SSAO 去压暗
地形更干净，也避免把 kernel 噪声烘进最终画面。

## 8. 编辑器与命令

- `Create -> Terrain` 菜单和 Hierarchy 的 `Create Terrain` 按钮共用
  `createTerrainThroughCommand()`，落点在相机前方 `sizeZ/2 + 10`、世界 Y=0。
- Inspector 的 Terrain 分组可改全部参数：参数修改走
  `SetTerrainConfigCommand`，只改 `config`，不重建网格。`Surface Detail`
  只影响片元材质，立即生效；`Chunk Grid X/Z`、`LOD Levels` 等几何相关字段
  变化才显示 Generate 提示。`LOD Distance` 只改运行时选层距离。
- `High Detail 257` 一键把分辨率设为上限，仍需点击 Generate 重建网格。
- `Generate` 先提交异步 GPU 高度场请求，完成后再把 CPU 网格构建提交给
  JobSystem；生成期间按钮仍可再次点击，用当前参数 supersede 旧任务并触发
  协作取消。UI 显示
  `Generating on JobSystem... press Generate to replace.`。worker 完成后主线程
  上传，成功回调才构造 `RegenerateTerrainCommand`；撤销/重做持有新旧
  `MeshComponent` 的 `shared_ptr<Model>`，旧显存不会在仍可撤销时被释放。
- 生成期间继续渲染旧网格；若用户在等待时改参数，完成后
  `generatedConfig` 仍是任务快照，`config` 保留新值并显示 Generate 提示。
- `Randomize` 只改 seed；`Reset` 只恢复默认参数；两者都不自动 Generate。
  参数与 `generatedConfig` 不同时显示 "Parameters changed, press Generate"。
- `DeleteEntityCommand` 的 snapshot 包含 `TerrainComponent`，删除地形可完整撤销。

## 9. 序列化

场景 JSON 的实体可选字段：

```json
"terrain": {
  "version": 5,
  "seed": 12345,
  "resolutionX": 257,
  "resolutionZ": 257,
  "sizeX": 100,
  "sizeZ": 100,
  "sampleOriginX": 0,
  "sampleOriginZ": 0,
  "baseHeight": 0,
  "heightScale": 25,
  "noiseFrequency": 0.01,
  "octaveCount": 6,
  "persistence": 0.5,
  "lacunarity": 2.0,
  "ridgeWeight": 0.0,
  "detailWeight": 0.25,
  "edgeFalloff": 0.0,
  "heightCurve": 1.0,
  "islandWeight": 0.0,
  "islandRadius": 0.8,
  "ridgeSharpness": 1.0,
  "detailFrequency": 2.37,
  "materialDetailStrength": 0.65,
  "uvTiling": 1.0,
  "localColorRange": true,
  "skirtsEnabled": true,
  "chunkGridX": 2,
  "chunkGridZ": 2,
  "lodLevelCount": 3,
  "lodDistanceScale": 1.0,
  "surfaceStyle": 0
}
```

流送 manager 使用独立的可选对象：

```json
"terrainWorld": {
  "chunkConfig": { "version": 5, "sizeX": 80, "sizeZ": 80, "resolutionX": 65,
                   "resolutionZ": 65, "lodLevelCount": 3,
                   "sampleOriginX": 0, "sampleOriginZ": 0,
                   "localColorRange": false, "skirtsEnabled": false },
  "loadRadius": 1,
  "unloadPadding": 1,
  "maxLoadsPerFrame": 2
}
```

不保存顶点、索引、包围盒、chunk 实体或 GPU/纹理句柄。加载时
`Scene::buildFromDescription()` 重建 manager，再由流送逻辑围绕相机加载 chunk。
`terrain.version` 当前为 5；旧 v1~v4 地形缺少的新字段由 `TerrainConfig`
默认值补齐，旧场景没有 `terrain`/`terrainWorld` 字段时按普通实体加载。

被笔刷改过的地形还会写一个可选对象，只存改过的格子，不写整块高度场：

```json
"terrainSculpt": {
  "indices": [ 1234, 1235, 2001 ],
  "heights": [ 0.512, 0.498, 0.63 ]
}
```

`indices` 是高度场的行优先下标（升序），`heights` 是归一化高度
（世界高度 = `baseHeight + heightScale * h`）。运行时形态是
`TerrainComponent::sculptEdits`（`std::map<uint32_t, float>`）：每次笔刷只把
"笔刷前 → 笔刷后"变化的格子并进去，保存时展开成这两个数组。加载时先
`applySculptDelta()` 盖到重建出的高度场上，再
`buildTerrainModelFromHeightField()` 重建网格，所以网格与碰撞对得上。
257² 的整块高度场是 264KB，一次半径 4m 的笔刷通常只动几百格，
因此存档里只出现差分；没有雕刻的地形不写这个键。

当前限制：只有手动地形（`terrain`）的雕刻会落盘。流送 chunk
（`terrainWorld`）由 manager 按 `chunkConfig` 重新生成，chunk 上的雕刻
仍不保存，属于 tech-debt #10 的后续项。

## 10. 验证

```powershell
# CPU + 网格 + 渲染路径
.\\build\\Debug\\TinyEngine.exe --selftest 20 --empty-scene --terrain-test

# 只看地形截图
.\\build\\Debug\\TinyEngine.exe --selftest 30 --empty-scene --terrain-test `
  --screenshot terrain.png --screenshot-frame 20

# 雕刻差分与存档往返（不需要 Vulkan / 窗口）
.\\build\\Debug\\TinyEngine.exe --terrain-sculpt-test
```

稳定标记：

- `[Terrain] sculptDeltaSelfTest PASS | cases=5 ascending=ok apply=ok guards=ok`：
  差分只收改动格且升序，尺寸不一致、越界、长度不等、非递增、NaN 全部拒绝；
- `[Terrain] sculptJsonSelfTest PASS | cases=2 round-trip=ok empty-omitted=ok`：
  `terrainSculpt` 往返逐格相等，没有雕刻时不写这个键；
- `[Terrain] LOD stitching PASS | lodPairs=9 edges=18 skirts=off boundary=full-res`：
  LOD0/1/2 的 3×3 邻接组合全部通过边界顶点位置、法线、UV、色断言；
- `[Terrain] selfTest PASS`：同/不同 seed、非法参数、顶点/索引与法线、+Y 绕序、
  chunk/LOD 区间、LOD 递减、局部 AABB、18 条邻接边与 16 张源纹理断言全部通过；
- `[Terrain] demo entity created`：`--terrain-test` 真正走完 Model 上传；
- `[Terrain] GPU mesh assembly ready | maxLods=3 maxResolution=257 mode=singleChunk async slots=4`：
  单 chunk mesh builder 资源与 4 个异步 slot 就绪；
- `[Terrain] GPU mesh assembly selfTest PASS | vertices=6691 indices=37824 ...`：
  GPU 顶点/索引与 CPU `TerrainMeshBuilder` 逐项一致；
- `[Terrain] async GPU upload PASS | resolution=257x257 chunks=1 ...
  gpuMesh=1`：257² 单 chunk 异步重建主线程 poll 时长低于同步创建 baseline；
- `[Terrain] streaming chunk test PASS | chunks=9 independentModels=9 gpuMesh=9`：
  流送 chunk 全部通过 GPU mesh 路径；
- `[Terrain] spatial region query PASS | chunks=9 regions=5x5`：Scene 通过
  XZ region 索引查询当前 manager 的 chunk，不扫描 chunk map；
- `[Terrain] GPU selection ready | entities=64 records=1024 commandsPerEntity=32
  drawIndirectCount=1`：compute selection 资源就绪；
- `[Terrain] GPU selection selfTest PASS | matches=3 records=4` 与
  `transition PASS | records=4`：GPU visible/LOD/commandCount 与 CPU mirror 一致，
  临时切换 `lodDistanceScale` 的 from/to 过渡命令也通过 parity；
- `[Terrain] GPU compute heightfield selfTest PASS`：GPU/CPU 高度场参考最大误差
  < 5e-4；`--terrain-test` 的同步创建与异步重建都断言 `generatedOnGpu`；
- `[Terrain] build task retirement PASS | cpuBuildInFlight=1 tracked=1 drained=1`：
  CPU 网格构建仍在 worker 上时销毁实体，任务进退役队列并最终 drain 干净；
- `[Terrain] async generation PASS`：异步 GPU 高度场请求 + JobSystem 网格提交、
  主线程轮询上传、revision 递增，默认 2×2 = 4 个逻辑 chunk、3 个 LOD；
- `[Terrain] sculpt PASS | chunks=9 rebuilt=1` / `character controller PASS | grounded=1`：
  笔刷只重建相交 chunk；角色贴地/跳跃/往返行走稳定；`character serialization PASS` 覆盖组件往返。
- `[Terrain] material sets PASS | resident=4 max=4 external=1 fallback=3 serialized=1`：
  4 个 Set2 材质集、外部 16 PNG 加载、程序化回退和 `materialSetIndex` 序列化。
- `[Scene] ECS/spatial selfTest PASS`：SpatialIndex 增量 upsert/remove、SubMeshCuller AABB、LOD 滞回用例；
- 渲染路径稳定帧/过渡帧只通过 `vkCmdDrawIndexedIndirectCount` 提交，`Terrain indirect groups: 1`；
- `[Validation] errors=0 warnings=0` + stderr 0 字节：地形 G-Buffer 渲染路径。

编辑器模式还会在 `selfTestWithScene()` 覆盖 `CreateTerrainCommand` 的
undo/redo 快照复用、`RegenerateTerrainCommand` 的新旧网格切换、碰撞高度场/
越界查询拒绝、局部与世界高度采样、地形三角形 raycast、连续提交两个异步请求、
只安装最新结果的 stale-discard 断言，以及 9 个独立 chunk 的接缝高度、
移动卸载/重载、manager 描述 round-trip、真实 JSON 文件 load、chunk→manager
picking 与 delete/undo/redo。
## 11. 当前限制

- 手动地形是单实体；流送 chunk 的几何归 `TerrainMeshArena`，GPU mesh 组装仍只支持单 chunk，
  多 chunk 与 fallback 走 CPU builder；
- LOD 边界做拓扑级全分辨率缝合，skirts 是可关闭的保守回退；换层用 0.24s 抖动 cross-fade，
  运动模糊下可能看到短暂噪声；不同 LOD 的粗内部法线/着色仍有正常层级差异；
- GPU selection 当前容量为 384 terrain entities / 6144 chunk records（F2-d 起），每 entity 16 条命令；
  超过容量的实体跳过，不混入 CPU 命令。无 `drawIndirectCount`
  设备回退 CPU 命令构建；GPU stats 是上一已完成帧，UI 允许一帧延迟；
- 异步 Generate/流送已改用 8 槽 GPU 高度场请求，GPU mesh 组装也已是 4 槽
  异步 submit/poll；同步场景加载仍走阻塞 `generate()`，全分辨率 CPU picking
  mirror 已在 F2-e 取消；F2-f 起流送 chunk 的 `Model` 只剩元数据，VBO/IBO 归 `TerrainMeshArena`；
- B-07 已接入轻量 kinematic 角色控制器与地形雕刻：角色使用局部包围盒 + 双线性高度采样，笔刷复用 region 查询只重建相交 chunk；但雕刻只修改运行期 HeightField/Model，不写入场景 JSON；
- 场景加载仍走同步 `createTerrainEntity`（编辑器创建已异步，见下节）；supersede 用协作取消，
  粒度是每个顶点行，不是硬中断；
- 地形默认 set0 的 albedo/normal/height/roughness 是启动时生成的程序化纹理；set1~3 可按需加载外部 PNG，但仓库不附带美术资产；
- 地形不参与 SSAO；大面积平滑地形使用 AO-neutral 策略，局部接触阴影留给后续；
- Set2 的纹理数量/加载回退见 [terrain-materials](terrain-materials.md)；
- `SpatialIndex` 用 Transform/Bounds dirty 队列（setter 自动入队，干净帧不扫描）；
  B-06 起 streaming chunk 另有 XZ region bucket；生产销毁统一走 `Scene::destroyEntity()`，
  地形实体总 AABB 仍用于视锥粗筛，chunk 剔除发生在 GeometrySystem 子网格级；
- Scene 把被替换的旧 Model 放进 4 帧延迟回收队列；撤销历史额外持有新旧 Model，上限 256 条。

## 异步创建地形（TD-24）

`Scene::createTerrainEntityAsync(config, name, spawn, onComplete)`：主线程只做「校验配置 → 申请材质槽位 →
建实体（Transform/Name/RenderTag/TerrainComponent, `generating = true`）→ 提交后台作业」，
高度场与网格复用 `requestRegenerateTerrain` 的既有管线，完成时主线程补 `MeshComponent`、包围盒、
dirty 跟踪与可见性。257² 默认档 Debug 实测：同步 `createTerrainEntity` **105.9 ms**、异步提交 **0.28 ms**。

编辑器侧 `CreateTerrainCommand(deferBuild = true)` 走异步，`EditorState::pendingTerrainBuild` 每帧 poll，
完成/失败各播报一条 Toast。`generating` 期间 `raycastTerrain*` 与地形 GPU 选择都会跳过该实体
（没有高度场、没有 MeshComponent），所以"创建后立刻点地面"可能落空 —— 等 Toast 出现即可。
