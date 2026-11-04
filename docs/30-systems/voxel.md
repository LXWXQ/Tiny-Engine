---
id: systems/voxel
title: 方块体素世界
type: system
scope: src/Voxel/, Scene 体素接口, 编辑器 Voxel Brush, shaders/mrt.frag（顶点色路径）
read-when: 改体素数据模型、网格生成规则、挖放/重建、体素拾取或地面查询
size: ~120 行
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
| `VoxelChunkComponent` | 同上 | 一块：world 句柄、chunkX/Z、baseY、`shared_ptr<Chunk>`、revision、faceCount |

坐标换算：`chunkX = worldX >> 5`，`localX = worldX & 31`（负数用 `toLocal()` 取正模）。
`Chunk::generate()` 用同一套值噪声（`splitmix64` + 双线性 `valueNoise`）按世界列算高度，
所以相邻 chunk 的边界天然连续，不需要缝合、也没有 LOD。

## 2. 网格

`voxel::buildMesh(const Chunk&, terrain::TerrainMeshData&, MeshStats*)`：

- 6 个面各一组 `FaceDef`，**邻格 `opaque` 就整面跳过**（面剔除）；
- 每面 4 顶点 6 索引，顶点色 = 方块色 x 面明暗（+Y 1.00、-Y 0.55、+X 0.82、-X 0.74、+Z 0.68、-Z 0.90）；
- 顶点布局直接复用 `terrain::TerrainVertex`（44 字节，与 `Vertex` 同布局）→ `terrain::TerrainMeshData` → `Model::Builder` 的上传路径，无需新缓冲格式；
- 不采样贴图：材质用 `MaterialSystem::getDefaultWhiteTexture()` 作 albedo（**必须给白图**，否则 `albedoFallback` 会画棋盘格），metallic 0 / roughness 0.9，颜色全部来自顶点。

## 3. 场景接口

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

## 4. 编辑器：Voxel Brush

- 切到**运行模式**（工具栏「运行」或 `F8`）后，鼠标直接操作世界；
- `左键` 挖掉命中方块，`右键` 在命中面放置当前方块（块型在 Inspector 的 `Voxel Brush` 段选）；
- 编辑模式下左键只做选中，不会改方块 —— 想挖/放先切到运行模式；
- 每次改动都是一条 `VoxelEditCommand(before, after)`，`Ctrl+Z` 逐格撤销；
- 没有体素世界时面板提示「创建 -> 体素世界」；启动参数 `--voxel-world [r]` 也会自动被笔刷认领。

## 5. 放置位置与命中法线

- `Scene::raycastVoxelWorld` 返回的 `outNormal` 必须是**轴对齐**的（只有一个分量 ±1）：
  右键放置用的是 `hitBlock + hitNormal`，斜射时如果直接返回 `-step`，会得到
  `(-1,-1,-1)` 这类对角偏移，方块就落在鼠标旁边的斜角上（用户报的位置不一致）；
- DDA 每次跨轴都重设 `faceNormal`，进入 chunk 的那一面记在 `entryAxis` 上；
  射线起点本来就在方块内部时（贴脸挖）用视线主轴反向兜底，保证仍是相邻格；
- 自检里有一条斜射断言：`components == 1 && magnitude == 1 && manhattan == 1`。

## 6. 已知限制

- **体素世界不进存档**：`captureDescription()` / `buildFromDescription()` 只处理地形与普通实体，
  重建场景（打开新场景、流送序列化自检）会把体素世界清掉（见 tech-debt）。
- 立方体网格没有贪心合并（相邻同色面仍是独立四边形），大世界顶点数偏高；
  chunk 网格生成目前在主线程同步完成（一块 32³ 约 1 ms 量级）。
- 没有 LOD、没有 AO、没有贴图集；颜色只有一张 16 色调色板。
- 水/透明方块只做了 `opaque = false`（玻璃、树叶），排序仍是按实体，没有逐面透明排序。

## 7. 自检

| 打印 | 覆盖 |
|---|---|
| `[Voxel] selfTest PASS \| face culling 6/10/6, boundary continuous, solid=<n>` | 空块不出面、单块 6 面、相邻块共享面剔除、挖掉后恢复、边界列高度与 `surfaceHeight` 一致 |
| `[Voxel] scene selfTest PASS \| chunks=9 hit=.. dugDeeper=1 restored=1 groundY=.. characterLanded=1 placedOnFace=1` | 世界创建、DDA 命中、挖深、放回、地面高度、角色落地、面放置 |

相关：[scene-ecs](scene-ecs.md)、[terrain](terrain.md)、[编辑器面板](../90-editor/panels.md)。
