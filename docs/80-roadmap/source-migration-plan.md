---
id: roadmap/source-migration-plan
title: Source 迁移改造计划（M1）
type: roadmap
scope: 空
read-when: 想推进 Source 引擎迁移改造、或确认某项改造到底做完了没有
size: ~131 行
updated: 2026-11-20
---

# Source 迁移改造计划（M1）

来源是 `D:\HL\HalfLife_Leak\leak\docs\source-migration\index.json`（29 篇，原索引按 P0/P1/P2
排序）。那份索引衡量的是「Source 里什么值得抄」，不是本仓库的优先级；实际顺序按「本仓库缺
什么、阻塞哪条 roadmap」重排，并做了三处现场修正（见 §3）。

总量估算：单人约 7 周；A+B 解 F4、C1+C2 解 F5、D3+D4 解 F6 的尾项、D1 是 F7 的前置。

## 1. 状态总表

| 项 | 内容 | 状态 | 提交 |
|---|---|---|---|
| A1 | 实体句柄契约 `EntityHandle` | 契约 + **接线完成**（`Selection`/`EditorState` 三个字段/`Gizmo::dragEntity` 改句柄，`pruneDestroyed` 每帧剪掉已销毁选择；命令栈仍存快照式 `entt::entity`） | `3810761` |
| A2 | 实体销毁唯一出口（静态门禁） | 完成（`Validate.ps1` 的 M1-A2 门禁） | `f2c0df4` |
| A3 | 射线契约 `TraceRequest/TraceResult` | 值类型 + `Scene::traceRay()` **已接线**（地形/体素/模型三条后端按 fraction 取最近，编辑器拾取改走它） | `f2c0df4` |
| B1 | glTF 节点树不再烘焙 | 完成（几何存节点局部空间） | `4feb575` |
| B2 | `.tpack` 资产容器 + 全资源接入 | 完成（glTF/OBJ/SPIR-V/场景 JSON 全走包） | `348dd35` `15585de` |
| B3 | 材质 JSON 只存非默认字段 | 完成（25 默认材质 4390B→540B，-88%） | `93fd114` |
| B4 | 地形雕刻差分落盘 | 完成（手动地形） | `9d2c46d` |
| B4b | 流送 chunk 雕刻落盘 | **未做**（见 §2） | — |
| B5 | 纹理显存预算与三级降级 | 完成（434.7MB→65.3MB，-85%） | `9d6c8f4` |
| C1a | 固定步长模拟时钟 | 完成（`SimulationClock` + `--tick-test`） | `118a758` |
| C1b | 编辑器两条模拟路径接入 | 完成（`stepSimulation` + `simulationDrivenExternally`） | `3e33071` |
| C1c | 渲染插值（TRS + slerp） | 完成（按 tick 门控，含地形闪烁修复） | `c451f07` `90e5dcb` |
| C2 | JobSystem work stealing + 依赖 + 取消 | 完成（5.9–6.2× 加速；偶发挂死已修）。取消能力在 `JobSystem` 内部（`CancelToken`），消费端接线未做 | `ab1cda3` `d0fa6c8` |
| C3 | 粒子 SoA | **判定不做**（实测 0.025ms/帧） | `d0fa6c8` |
| C4 | 音频子系统（混音器 + XAudio2） | 完成 | `9075e5a` |
| C4b | 3D 音源接入场景 | 完成（距离衰减/声像/低通） | `e1265b6` |
| D1a | 角色扫掠盒求解器 | 完成（8 组几何自检） | `d5dfc8b` |
| D1b | 角色控制器接入求解器 | 完成（地形/体素/模型三类碰撞源） | `a386c3f` |
| D2 | 角色步进纯函数 + 录制/回放 | 完成（60/144Hz 抖动帧逐 tick 哈希一致） | `e5fd09b` |
| D3 | 第一人称手持物独立通道 | 完成（独立近远平面 + 独立深度） | `5446ef4` |
| D3b | 3D 几何天空（SkyCamera） | 完成（锁相机 + `skyGeometryPipeline`） | `508ab6e` |
| D4 | shader specialization constants | **已评估：不接**（决策 85；`Pipeline.cpp:105/:113` 显式 `pSpecializationInfo = nullptr` 属有意为之，运行时可调标量走 push constant） | — |
| D5 | GPU LOD 选级 | 地形已完成（`shaders/terrain_lod.comp` + CPU parity 镜像）；**模型没有 LOD 链**，故模型侧无从选级 | — |

## 2. 未完成项

| 项 | 现状（代码位置） | 缺口 | 量级 |
|---|---|---|---|
| ~~A1 接线~~ | 已完成：`Selection`/选中态/`Gizmo::dragEntity`/`TraceResult.entity` 均改用 `EntityHandle`，编辑器自检新增“选中→销毁→prune”用例 | — | — |
| ~~A3 接线~~ | 已完成：`Scene::traceRay()` 落地（地形法线用高度场梯度、体素用整数面法线、模型暂不给法线），编辑器 `pickEntity` 已改走它 | — | — |
| B4b | `TerrainComponent::sculptEdits` 只服务手动地形（`Components.h` 注释写明「流送 chunk 不走这里」） | chunk 差分按 `(chunkX, chunkZ)` 存进 `TerrainStreamingComponent`，并在 `processTerrainGeneration` 的 GPU 高度→GPU 网格两阶段之间回放 | 2–3 天 |
| C2 收尾 | `CancelToken` 在 JobSystem 之外 0 处使用 | 地形/体素异步改用 `CancelToken`，替掉各自的 `cancelRequested` | 0.5 天 |
| D4 | `src/RHI/Pipeline.cpp` 仍写死 `pSpecializationInfo = nullptr` | 用 spec constants 合并 shader 变体（现在 48 个手写变体） | 1–2 天 |
| D5 | 只有 CPU 选级 `updateTerrainLodSelection`（`GeometrySystem.cpp:914`） | compute 选级 / GPU 驱动剔除 | 3–5 天 |

「剩下先记着」的其它尾巴（都不阻塞 roadmap）：

- `RenderTagComponent::isSkyGeometry` 此前不进场景 JSON；`SkyCameraComponent` /
  `ViewModelComponent` 走通用 `components` 记录，参数本来就能存。
- 手持物只有程序化立方体（`Device` 没有 command pool 访问器，加载 glTF 需要它）。
- 3D 天空还没有真实远景资产，观感是一条地平线。
- 音频：无遮挡/混响、无多普勒、无总线、无流式解码，片段槽固定 32。
- 角色：模型只用世界 AABB、角色互不阻挡、玻璃算实心、蒙皮骨骼仍按 tick 更新。
- 粒子：模拟侧 0.025ms/帧不是瓶颈，真正该量的是 `ParticlePass` 的实例写入与排序。

## 3. 相对原索引的三处现场修正

1. **`core/handles` 说 1–2 天，实际不是**：本仓库 `entt::entity` 出现 518 处、
   `getRegistry()` 在 `Scene.cpp` 之外 47 处。原文档假设「实体表是引擎私有的」，
   这里不是，所以只能分级落地（A1 只建契约，改持有者留作后续）。
2. **`20-geometry/01` 说的「22 处零散 raycast」不成立**：实测 4 个入口、3 个文件
   19 行。收益不在削减调用点，而在统一输出契约（法线/命中实体/fraction 的语义）。
3. **`30-assets` 的 `tpack` 虽然标 P1，却是 F4 的阻塞项**，所以提到 B 阶段最前；
   且必须先修 glTF 节点层级，否则包里的资产装进来仍是烘焙过的顶点。

另外 `CMakeLists.txt` 是 C++17，文档里 `TraceRay` 的 `operator==(...) = default`
（C++20）不能照抄，手写比较。

## 4. 明确不做

| 项 | 理由 |
|---|---|
| `core/symbols` 符号驻留 | 渲染侧已经强类型（`FrameInfo`/`EngineSettings`），热路径没有字符串比较 |
| `core/cvar` 标志位 + 编译期消除 | 现有 `EngineSettings` 够用，收益 < 迁移成本 |
| `core/containers` | `TerrainMeshArena` 的 bump + 空闲区间合并已优于 Source 的定长 arena；只保留「显式 reserve」当 review 清单 |
| `assets/searchpaths` | 全仓库 `filesystem::exists` 只有 6 处 |
| C3 粒子 SoA | 上限 16384 下模拟 0.025ms/帧（Release，1.5ns/粒子）；触发条件是 >10 万粒子、>0.1ms/帧、或要逐粒子碰撞/软粒子 |
| A0 的 11 条反模式 | VPC / 引擎-游戏 DLL ABI / VGUI2 / Havok 壳 / 手工 barrier 一律不抄 |

## 5. 验收数据（可复现）

稳定基线：`--selftest 5` 的 Sponza bounds
`min(-15.3676, -1.01154, -9.46246) max(14.3993, 11.4355, 8.84341) radius 18.5479`
（顶点 192496 / 索引 786801 / 子网格 103），改造全程逐字一致。

| 项 | 数据 |
|---|---|
| B3 材质 JSON | 25 个默认材质 4390B → 540B（-88%）；`__EngineDefault` 151B → 27B |
| B5 纹理预算 | 101 张 / 434.7MB → `--texture-budget 64` 时 65.3MB（-85%），74 张降级，画面 bounds 不变 |
| C2 JobSystem | 串行 9.10ms → 并行 1.54ms（5.9×）；取消 < 500ms |
| C3 粒子 | Release 0.0242–0.0250 ms/帧（1.5ns/粒子），1ms 预算 ≈ 65 万粒子 |
| C4 音频 | `--audio-play` played=1 peak=0.169706 active=1；100 事件 → 64 声部、stolen=36 |
| D2 回放 | 3 段 × 60/144Hz，逐 tick 哈希零 mismatch |
| 全量回归 | `Validate.ps1 -Incremental` 96 项 ALL PASS，16 个 `m1-*` 用例 |

复现：`tools\Validate.ps1 -Incremental`；单项见
[cli-reference](../50-validation/cli-reference.md)，自检清单见
[test-matrix](../50-validation/test-matrix.md)。

## 6. 阶段划分与顺序理由

| 阶段 | 内容 | 出口条件 |
|---|---|---|
| A 契约 | 句柄、销毁出口、射线契约 | 两条契约自检进 `Validate`，生产代码开始使用 |
| B 数据 | glTF 层级、`.tpack`、材质表、雕刻落盘、纹理预算 | F4 的阻塞项清零，资产能全部从包内读 |
| C 系统 | 固定 tick、JobSystem、音频 | 模拟可重放、任务可偷可取消、引擎发声 |
| D 玩法 | 角色碰撞、回放、手持物、天空、（D4/D5 未做） | 角色不穿墙、能上台阶、画面不抖 |

顺序理由：先契约后数据，是因为句柄与射线一旦定型，B/C/D 的代码就不会再写一遍；数据在系统
之前，是因为 `.tpack` 与节点层级会改变资产的装载方式；系统在玩法之前，是因为角色碰撞与回放
都依赖固定 tick。**每阶段都以「全量回归 96 项不红 + 基线 bounds 逐字一致」为硬门槛**。

## 7. 与 roadmap 的关系

- F4（资产与场景管线）：A+B 已解掉「glTF 烘焙」这个阻塞，剩异步加载/引用计数/材质流送。
- F5（运行时架构）：C1+C2 已解掉 JobSystem 与固定 tick，剩序列化热重载。
- F6（渲染特性）：D3/D3b 补了手持物与天空；SSR、时间超分、切线空间法线仍未做（自动曝光已于决策 42 落地，默认关），
  D4/D5 是它们的加速项。
- F7（物理/动画/工具）：D1 已经是前置（角色碰撞不依赖 Jolt），Jolt 本身仍未引入。
