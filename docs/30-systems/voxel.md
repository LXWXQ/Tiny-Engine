---
id: systems/voxel
title: 方块体素世界
type: system
scope: src/Voxel/, Scene 体素接口, 编辑器 Voxel Brush, src/Render/GlassPass, shaders/glass.*, shaders/mrt.frag（顶点色路径）
read-when: 改体素数据模型、网格生成规则、挖放/重建、体素拾取或地面查询
size: ~135 行
updated: 2026-11-04
---

# 方块体素世界

七日杀式的挖掘与建造：世界由 32x32x32 的方块 chunk 组成，挖掉一格是一个整齐的坑，
放一格是一块实心方块。实现走**块状立方体网格**，不是 marching cubes / surface nets ——
等值面挖出来是圆滑洞，与「挖矿盖房」的期待不符，而且改一格要重算周围十几格顶点。

## 1. 数据模型

| 类型 | 位置 | 说明 |
|---|---|---|
| `voxel::BlockId` | `src/Voxel/VoxelTypes.h` | Air / Grass / Dirt / Stone / Sand / Wood / Leaves / Plank / Brick / Glass |
| `voxel::BlockInfo` | 同上 | 名称、顶点色（r/g/b）、`solid`、`opaque` |
| `voxel::Chunk` | `src/Voxel/VoxelChunk.h` | `kSize = 32`、`kBlockCount = 32768`、`get/set/fill/generate/solidCount` |
| `VoxelWorldComponent` | `src/Scene/Components.h` | 世界管理器：seed、半径、baseY、blockScale |
| `VoxelChunkComponent` | 同上 | 一块：world 句柄、chunkX/Z、baseY、`shared_ptr<Chunk>`、revision、faceCount、`glassFaceCount` / `glassMesh`（V-01） |

坐标换算：`chunkX = worldX >> 5`，`localX = worldX & 31`（负数用 `toLocal()` 取正模）。
`Chunk::generate()` 用同一套值噪声（`splitmix64` + 双线性 `valueNoise`）按世界列算高度，
所以相邻 chunk 的边界天然连续，不需要缝合、也没有 LOD。

## 2. 网格

`voxel::buildMesh(const Chunk&, terrain::TerrainMeshData&, MeshStats*)`：

- 六个方向各做一次**贪心合并**：按 32 层切片建 mask（记录该格方块 id，邻格 `opaque` 的格不写），先在平面 u 方向扩展宽度、再在 v 方向扩展高度，同材质的矩形只输出 1 个 quad（4 顶点 6 索引）；
- 每面 4 顶点 6 索引，顶点色 = 方块色 x 面明暗（+Y 1.00、-Y 0.55、+X 0.82、-X 0.74、+Z 0.68、-Z 0.90）x 顶点 AO；AO 看空气侧两条边与对角（`level = (s1 && s2) ? 0 : 3 - (s1 + s2 + cc)`，系数 `0.55 + 0.15 * level`），矩形四个角各取所在方块的 AO；
- 顶点布局直接复用 `terrain::TerrainVertex`（44 字节，与 `Vertex` 同布局）→ `terrain::TerrainMeshData` → `Model::Builder` 的上传路径，无需新缓冲格式；
- **贴图集（F3-f，方案 A）**：`MaterialSystem::fillVoxelAtlas()` 在启动时程序化生成 256x256 的 `VK_FORMAT_R8G8B8A8_SRGB` 图集（4x4 格、每格 64px，格内 = `BlockId - 1`），九种方块各有噪点/石纹/木纹/砖缝/玻璃边框；
- **UV 编码**：顶点 `uv.x = tile * 64 + 面内跨度 u`、`uv.y = 面内跨度 v`，跨度取贪心矩形的宽/高，因此一片大矩形在图块内平铺而不是拉伸；顶点色只保留 `面明暗 x AO` 的灰度；
- **片元解码**：`materialFactors.factors[matId * 2u + 1u].z > 0.5`（材质 `shaderFlags = 1.0`）时，`tileId = floor(uv.x / 64)`、`local = fract(vec2(uv.x - tileId * 64, uv.y))`、`atlasUV = (vec2(mod(tileId, 4), floor(tileId / 4)) + clamp(local, 0.02, 0.98)) * 0.25`（夹 0.02~0.98 防图块间渗色）；
- 体素材质因此必须同时设 `albedoMap = getVoxelAtlasTexture()` 与 `shaderFlags = 1.0f`；普通模型仍走 `texture(albedoMaps[matId], fragUV)`。
- **玻璃走独立网格（V-01）**：`buildMesh` 遇到 `isGlass` 直接跳过，玻璃不进贪心不透明网格（否则会先写进 G-Buffer 深度，`GlassPass` 里同一层玻璃被自己挡掉）；`buildGlassMesh` 逐面输出（不做贪心），只有邻格是空气才出面 —— 玻璃-玻璃、玻璃-不透明方块的贴面直接跳过，避免叠暗；UV/AO 编码与不透明网格一致，结果挂在 `VoxelChunkComponent::glassMesh / glassFaceCount`。

## 3. 玻璃与透明排序（V-01）

玻璃不进 G-Buffer：延迟着色的 G-Buffer 只能存一个表面，玻璃叠玻璃时没有地方放第二层。它在 `Lighting` 之后、`Bloom` 之前由 `src/Render/GlassPass` 前向混合：

- **管线**：`kHdrSceneFormat` 单颜色附件，`LOAD` 载入 Lighting 结果；深度 `LOAD`、`depthWrite = FALSE`、`compare = LESS_OR_EQUAL`（不透明几何仍挡住玻璃，玻璃自己不写深度）；`blendEnable = TRUE`，`SRC_ALPHA / ONE_MINUS_SRC_ALPHA`，`srcAlpha = ONE / dstAlpha = ZERO`。
- **排序**：CPU 每帧按面质心到相机的距离平方**从远到近**排序（`writeSortedIndices()`），索引写进按帧轮转的 host-visible 索引池，一个 pass 一次 `vkCmdDrawIndexed`。
- **几何来源**：`GlassPass` 直接读 `VoxelChunkComponent::glassMesh`，用实体世界矩阵变换（位置 `world->matrix`、法线 `normalMatrix`），所以 glass-only chunk 不需要 `Model` / `MeshComponent`，但要有材质槽位。
- **着色**：`glass.frag` 复用图集解码（`clamp(local, 0.02, 0.98)` 防渗色），颜色乘玻璃蓝 `vec3(0.72, 0.88, 0.95)` 与顶点 AO；法线按视线双面翻转；alpha = `0.45 + fresnel * 0.35 + spec * 0.5`（上限 0.95），边缘更不透明。
- **日志**：`[Glass] sorted=<面数> drawcalls=1`（面数变化时打印）；没有玻璃时 pass 早退，不产生 draw/bind。`--voxel-glass-demo` 在 `--voxel-world` 里摆交叉玻璃墙并把相机拉近，供目测与回归。
- **范围**：只有 `Glass` 走这条路；`Leaves` 留在 G-Buffer（图集没有 alpha 通道，做混合会变成半透明色块）。

## 4. 场景接口

| 接口 | 行为 |
|---|---|
| `Scene::createVoxelWorld(seed, radiusChunks, baseY)` | 建管理器 + `(2r+1)^2` 个 chunk 实体，半径夹到 1..8 |
| `Scene::setVoxelBlock(world, x, y, z, id)` | 写入并重建本块；格子在 chunk 边界（local 0/31）时连邻块（含对角）一起重建 |
| `Scene::getVoxelBlock(world, x, y, z)` | 越界/无块返回 Air |
| `Scene::raycastVoxelWorld(...)` | chunk AABB 粗筛 + Amanatides-Woo DDA，返回命中方块、命中面法线与距离 |
| `Scene::sampleVoxelGroundHeight(x, z, outY)` | 该列自顶向下第一个 `solid` 方块的 `y + 1` |
| `Scene::queryGroundAtWorld()` | 在地形分支之后叠加体素分支，取更高者 —— 角色因此能站在方块上 |

重建走 `Scene::rebuildVoxelChunk()`：旧 `Model` 先 `retireModel()`（延迟 4 帧释放）再换指针，
避免 GPU 还在用的时候把缓冲销毁掉。

## 5. 编辑器：Voxel Brush

- 切到**运行模式**（工具栏「运行」或 `F8`）后，鼠标直接操作世界；
- `左键` 挖掉命中方块，`右键` 在命中面放置当前方块（块型在 Inspector 的 `Voxel Brush` 段选）；
- 编辑模式下左键只做选中，不会改方块 —— 想挖/放先切到运行模式；
- 每次改动都是一条 `VoxelEditCommand(before, after)`，`Ctrl+Z` 逐格撤销；
- 没有体素世界时面板提示「创建 -> 体素世界」；启动参数 `--voxel-world [r]` 也会自动被笔刷认领。

## 6. 放置位置与命中法线

- `Scene::raycastVoxelWorld` 返回的 `outNormal` 必须是**轴对齐**的（只有一个分量 ±1）：
  右键放置用的是 `hitBlock + hitNormal`，斜射时如果直接返回 `-step`，会得到
  `(-1,-1,-1)` 这类对角偏移，方块就落在鼠标旁边的斜角上（用户报的位置不一致）；
- DDA 每次跨轴都重设 `faceNormal`，进入 chunk 的那一面记在 `entryAxis` 上；
  射线起点本来就在方块内部时（贴脸挖）用视线主轴反向兜底，保证仍是相邻格；
- 自检里有一条斜射断言：`components == 1 && magnitude == 1 && manhattan == 1`。

## 7. 持久化：seed + 差分

场景 JSON 只记 `seed / radiusChunks / baseY` 与**被改过的格子**：
`voxelWorlds: [{ seed, radiusChunks, baseY, chunks: [{ chunkX, chunkZ, edits: [[x, y, z, block], ...] }] }]`。
载入时先按 seed 重建世界，再逐格应用 edits，每个受影响 chunk 只重建一次网格。
一个 32³ chunk 有 32768 格，全量存盘会爆；实际改动通常只有几百格，所以存差分。
自检：`persistCapture/persistRestore`（挖一格只产生一条 Air 记录，放回后清零）与编辑器侧的 `SceneSerializer::roundTrip`。

## 8. 异步构建（F3-g）

- `Scene::createVoxelWorld()` 只做「管理器 + 每块占位实体」：chunk 实体带 `VoxelChunkComponent`，
  但**先不挂 `MeshComponent`**，所以不会阻塞主循环；耗时就在建实体（半径 12 / 625 块约几十毫秒）。
- 真正的工作（`Chunk::generate` + `voxel::buildMesh`）交给 `JobSystem::submit`，结果留在
  `PendingVoxelChunk` 里；`Scene::update()` 每帧调用 `processVoxelChunkBuilds()` 回收，**每帧最多上传 4 块**
  （建 `Model` 必须在主线程：Vulkan 上传不是线程安全的）。
- `VoxelWorldComponent::building / pendingChunks` 记录进度：全部回收后 `building = false`；
  `Scene::waitForVoxelBuilds()` 供自检 / 存档重建同步等待。
- 半径上限从 8 提到 **12**（`Scene::kMaxVoxelRadius`），`--voxel-world [r]` 同步；日志
  `[Voxel] world created | seed= radius= chunks= baseY= async submitMs=`。

## 9. 已知限制

- **体素世界不进存档**：`captureDescription()` / `buildFromDescription()` 只处理地形与普通实体，
  重建场景（打开新场景、流送序列化自检）会把体素世界清掉（见 tech-debt）。
- chunk 网格生成仍在主线程同步完成（一块 32³ 约 1 ms 量级），没有并行 meshing；
  chunk 网格生成目前在主线程同步完成（一块 32³ 约 1 ms 量级）。
- 没有 LOD；AO 是逐顶点环境光遮蔽（不是烘焙贴图）。
- 只有玻璃（`Glass`）走透明 pass 并按面排序；树叶（`Leaves`）仍留 G-Buffer（图集没有 alpha 通道，做混合会是半透明色块）。

## 10. 自检

| 打印 | 覆盖 |
|---|---|
| `[Voxel] selfTest PASS \| face culling + greedy 6/6/6/6, boundary continuous, solid=<n>` | 空块不出面、单块 6 面、相邻两块贪心合并成 6 面、2×2 平板仍是 6 面、挖掉后恢复、边界列高度与 `surfaceHeight` 一致 |
| `[Voxel] scene selfTest PASS \| chunks=9 hit=.. dugDeeper=1 restored=1 groundY=.. characterLanded=1 placedOnFace=1` | 世界创建、DDA 命中、挖深、放回、地面高度、角色落地、面放置 |
| `[Glass] sorted=<n> drawcalls=1` | 玻璃网格拆分、按面质心排序后一次 draw；`--voxel-glass-demo` 叠放场景（`f3-voxel-glass`） |

相关：[scene-ecs](scene-ecs.md)、[terrain](terrain.md)、[编辑器面板](../90-editor/panels.md)。
