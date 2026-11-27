---
id: roadmap/terrain-plan
title: 程序化地形实施方案（F2 / D-18）
type: roadmap
scope: 地形方案的三条原始取舍、阶段状态、风险对策
read-when: 想知道地形为什么用顶点色/同步生成/单块起步，或要复核阶段 7 还剩什么
size: ~150 行
updated: 2026-11-27
---

# 程序化地形实施方案（F2 / D-18）

> **本文现在只回答「当初为什么这么定」。** 阶段 1~7 除「无限世界 / 多 chunk GPU 合批」外全部落地；
> 落地后的数据模型（`TerrainConfig`）、噪声与高度场、网格规则、渲染与 LOD、编辑器、序列化、验证命令
> 都在 [terrain](../30-systems/terrain.md) 与 [terrain-materials](../30-systems/terrain-materials.md)。
> 2026-11-27 删掉原 §2、§4~§9、§14（约 430 行）——那几节写的是「打算怎么写」，与 terrain.md 的
> 「现在是什么样」逐条重复，且已经过期（仍写 129² 起步、仍假设地形挂普通 `MaterialComponent`）。

## 1. 结论

这份规范方向可行，但不能按它建议的类名/目录照搬：本工程已有 `Model`/`GeometrySystem`/`SceneDescription`/
`CommandStack`/`SpatialIndex`，地形接在它们上面，不另长一套。三条根上的结论：

- **不另建 RenderQueue**：提交入口是 `Scene::visibleEntities` + `GeometrySystem`，地形作为普通实体进入同一条路径。
- **不另建渲染路径**：着色差异用 `terrainMode` 分支表达，而不是加一条管线。
- **规模化不塞进 MVP**：chunk/LOD/异步/材质分层推到阶段 6、7，先把「创建 → 参数 → 生成 → 视口显示 → 保存/加载 → 删除/撤销」打通。

## 2. 三个取舍（决策依据，事后回看）

### 取舍 A：MVP 不着色纹理，用顶点色

`GeometrySystem` 只要实体没有 albedo 贴图就强制画棋盘格，地形挂普通 `MaterialComponent` 会变成黑白格。当时三条路：

1. 生成 albedo 纹理并更新 bindless 槽位 —— 要处理纹理所有权、槽位更新、延迟释放，MVP 风险最大；
2. 新增地形专用材质/管线 —— 违背「不另建渲染路径」；
3. **顶点色 + `terrainMode` 分支** —— 复用同一 G-Buffer/光照，零新描述符。

选 3。事后：阶段 6 已换成专用 Set 2 的 4 层纹理分层，顶点色作为程序化回退保留，这条取舍没有返工成本。

### 取舍 B：MVP 同步生成，不引入异步任务状态机

把生成放 worker 后 UI 要每帧轮询结果，还要处理「旧任务晚到覆盖新地形」、对象销毁、GPU 上传必须在主线程等问题。MVP 用显式 Generate + 129²（上限 257²），耗时可控。

事后：阶段 7 已改成异步（不可变 config 快照 + request serial + 主线程上传 + 旧任务丢弃），默认分辨率提到 257²。**没变的一条是「Vulkan 资源只在主/渲染线程创建」**——异步只覆盖 CPU 生成，上传仍在主线程（257² 主线程 poll 约 1ms）。`JobSystem::CancelToken` 至今未接线，地形用 `submit` 出去的任务里自己的 `cancelRequested` 轮询（tech-debt #4）。

### 取舍 C：单块地形，不做 chunk/LOD

`SpatialIndex` 每帧重建、`GeometrySystem` 逐实体提交；单块地形只有 1 个 draw call，正好先验证端到端。chunk/LOD 要全局坐标采样、邻接缝合、增量索引，属于规模化阶段。

事后：已推翻——B-01 独立 chunk 实体 + 流送、B-02 拓扑级 stitching、B-03 GPU 选层全部落地，单块路径保留为 fallback。

## 3. 阶段状态

| 阶段 | 交付 | 状态 | 记录 |
|---|---|---|---|
| 0 | 审计与方案（现状表、取舍、风险） | 完成 | 本文 |
| 1 | `TerrainConfig` / `TerrainNoise` / `TerrainHeightField` + `terrain::selfTest()` | 完成 | D-18 |
| 2 | `TerrainMeshBuilder` + 顶点/索引/法线断言 | 完成 | D-18 |
| 3 | `createTerrainEntity`、`RenderTag::isTerrain`、`terrainMode` 分支 | 完成 | D-19 |
| 4 | Create 菜单、Inspector、Generate/Randomize/Reset、撤销 | 完成 | [panels](../90-editor/panels.md) |
| 5 | `SceneDescription` / JSON / roundTrip / `--capture` | 完成 | D-20 |
| 6 | 4 层 Set 2 材质分层、Surface Style、外部 `setN` PNG、高度/坡度混合 | 完成 | B-08 |
| 7 | 异步生成、chunk + LOD、GPU 选层与 culling、GPU 网格组装、流送、雕刻 | 完成（无限世界除外） | B-01~B-07 |
| 7 剩余 | 无限世界 / 多 chunk GPU 合批 | 未开始 | [backlog](../70-status/backlog.md) V-02/V-03/V-05 |

## 4. 风险与对策

| 风险 | 影响 | 对策（现状） |
|---|---|---|
| `Model` 析构立即 `vmaDestroyBuffer` | 重新生成释放 GPU 在用的旧缓冲 | 命令持有新旧 Model；Scene 另有 4 帧延迟回收队列 |
| 材质没有 albedo 走棋盘格 | 地形显示成黑白格 | `terrainMode` + 顶点色，不依赖 `MaterialComponent` |
| 删除实体没快照地形参数 | 撤销丢掉整块地形 | `DeleteEntityCommand` snapshot 内含 `TerrainComponent` |
| 生成在主线程 | 高分辨率卡 UI | 已异步；同步加载仍阻塞（[known-limits](../70-status/known-limits.md)） |
| push constant 布局错位 | 材质/地形颜色全乱 | `offsetof`/`sizeof` 静态断言 + 与 GLSL 同步 |
| 绕过 `Scene::destroyEntity` 销毁 | Model/空间索引残留 | 生产路径统一（M1-A2 门禁在 Validate.ps1 里） |
| 旧场景文件 | 加载失败 | `terrain` 是可选字段，缺省不生成；scene version 保持 1 |
| 撤销命令长期持有旧 Model | 显存增长 | 命令历史上限 256，延迟回收只覆盖在途 GPU 使用 |

## 5. 验收

逐阶段验收清单已并入 [test-matrix](../50-validation/test-matrix.md)：`--selftest` 的 `[Terrain] selfTest PASS`、地形相关用例与 `[Validation] errors=0 warnings=0`。2026-11-27 删掉原 §13 的 10 条勾选清单——它与 test-matrix 的用例表是同一批断言，两处各维护一遍必然漂移。
