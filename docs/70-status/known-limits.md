---
id: status/known-limits
title: 已知边界
type: status
scope: 空
read-when: 判断某个需求现在能不能做
size: ~100 行
updated: 2026-11-05
---

# 已知边界

## 1. 平台与硬件

- 仅 **Windows x64**。无 Linux / macOS / Android。
- 需要 **Vulkan 1.3** 且支持 `dynamicRendering`、`synchronization2`、`timelineSemaphore`、
  `descriptorIndexing`、`fragmentStoresAndAtomics`、`maintenance4`。
- 未做多 GPU / 设备选择 UI。

## 2. 渲染特性缺失

无：SSR、折射、水下后处理、自动曝光、超分、通用动画/骨骼系统、粒子（`ExternalInstance` 入口已就位，无生产方）、
点光源阴影（方向光 CSM 已做，只支持单方向光、级联固定 3 级）、体积光、通用透明排序（仅体素玻璃有独立 pass）、通用 GPU LOD/网格组装。
后处理抗锯齿已有 TAA，但只有相机重投影、没有运动向量，物体自身运动不会被单独补偿。
骨骼动画目前只服务运行模式的单角色（CPU 蒙皮，见 [character](../30-systems/character.md)），不是通用动画系统。
地形单 chunk 子集已接 GPU culling、indirect count 与 GPU 顶点/索引组装；体素走独立 `Model` 路径（无合批、无 LOD）。
体素世界半径上限 12（`Scene::kMaxVoxelRadius`）；玻璃（`Glass`）走独立 `GlassPass` 按面排序混合，树叶（`Leaves`）仍在不透明 G-Buffer（图集无 alpha 通道）。水面（`Water`）是独立的水平前向混合 Pass，只在有地形/体素块时注册，反射只来自全局环境图，无折射/SSR/水下后处理。

## 3. 架构限制

| 限制 | 后果 | 详见 |
|---|---|---|
| 跨队列资源走 `CONCURRENT` sharing | 未实现 ownership transfer；带宽不是最优 | [weak-points](../10-architecture/weak-points.md) #1 |
| glTF 几何烘焙到世界空间 | Sponza 内部节点不是独立 entity，不能局部动 | 决策 13 |
| SpatialIndex 仅 streamed chunk 有 XZ region bucket | 非 chunk 实体仍走统一 3D 网格；大世界仍需分块剔除 | [scene-ecs](../30-systems/scene-ecs.md) §3 |
| 线性渲染图 | 无自动重排/别名化/死代码剔除 | 决策 2 |
| 材质槽位只增不减 | 反复换场景会吃满 256 槽 | [tech-debt](tech-debt.md) #3 |
| 单场景单相机 | 无多视口 / 分屏 | — |

## 4. 可观测性限制

- 回归最长只跑 120 帧，**没有小时级稳定性测试**。
- 交互式关窗路径（用户点 X）没有单独回归——历史 VMA 崩溃就藏在析构路径上。
- 截图类测试依赖"窗口在前台"，脚本做了检查但那是脚本级保障。
- 没有自动化视觉金标准（`Screenshot.ps1` 有 diff 能力，但没有纳入默认回归的金标准图）。

## 5. 内容与资产

- `3rdParty/` 与 `Resources/` 的绝大部分 **不入库**，需 `SetupDependencies.ps1` + 手动准备；例外是运行模式角色 `Resources/Models/Character/`（CC0，随仓库提供）。
- 当前场景固定为 Sponza + 一个测试球 + 天空盒，没有场景描述格式。
- 材质只支持 4 张 PBR 贴图（albedo / normal / metallic / roughness），
  无法线贴图实际采样、无 emissive / AO 的独立通道（emissive 贴在 albedo 里）。

## 6. 工程化限制

- 无 CI（全部靠本地 `Validate.ps1`）。
- 无 Release 构建的性能基线（所有数字都是 Debug）。
- 无单元测试框架，测试都以"进程内自检 + stdout 标记"的形式存在。

## 编辑器（D-8）

- **单选**：`Selection` 接口按集合设计，但 UI 只做单选。
- **材质参数不可撤销**：Inspector 里改 metallic/roughness/baseColor 不进命令栈。
  它们是外观调参，撤销需求弱；真正需要的是"材质库 + 引用的撤销"，那是另一个量级。
- **播放模式没有逻辑**：`PlayState` 状态机与快照/恢复已就位，但没有脚本/物理推进点。
  以后加逻辑时在 `EditorLayer::updatePlayState()` 里接。
- **无 docking**：ImGui 1.92.6 是非 docking 分支，布局是手写的固定位置。
  换成 docking 分支后 `drawPanels()` 里的布局分支可以整段删掉。
- **实体 ID 不稳定**：删除后撤销，实体 ID 会变。任何跨帧缓存 `entt::entity`
  的地方（选中态除外，它会被显式更新）都不能假设 ID 稳定。

## 角色与动画（E9）

- **单角色**：运行模式只有一个角色；没有 NPC / 多角色/多骨架管理。
- **CPU 蒙皮**：每帧在 CPU 上重算全部顶点并写动态缓冲；角色顶点数量上来后必须改成 GPU skinning。
- **无动画混合**：Idle/Walk/Run/Jump 之间硬切，没有 cross-fade、分层、遮罩；切 clip 时 time 归零，快速来回按键会有轻微跳变。
- **无动画事件 / IK / root motion**：不能从动画里触发逻辑或提取位移；位移完全由 `CharacterControllerComponent` 决定。
- **状态机阈值固定**：Run 需要水平速度 > 5，Walk > 0.2；默认移速 6 × 运行倍率 1.35 才进 Run，改移速/倍率时要同步检查阈值。
- **出生点依赖地面查询**：视线 2..180m 内找不到可站立面时，退化为相机前方 2m + 重力；相机完全朝天或脚下是空洞时角色会先下落。
- **资源固定**：`Resources/Models/Character/Character.glb`（Quaternius Platformer Game Pack，CC0）；替换模型时必须保留 Idle/Walk/Run/Jump/Jump_Idle/Jump_Land 这 6 个 clip 短名，或改 `CharacterAnimationComponent` 的默认名。

## 视口交互（D-10）

- **只有单轴手柄**：没有平面手柄（XY/YZ/XZ）与屏幕空间手柄。
- **旋转以四元数为准，Inspector 显示欧拉角**：万向节附近欧拉数值可能跳变，
  但物体矩阵不会死锁；保存/加载仍走欧拉角（版本 1 格式），极端姿态保存后会
  有等价但不同的欧拉表示。父节点带非均匀缩放时局部轴可能不严格正交。
- **拾取已精确到三角形**，但 Sponza 整栋仍是一个实体，所以点任意墙面都选中它
  —— 要选到内部构件需要先把 glTF 节点拆成独立实体（需改 `GLTFLoader` 保留节点树）。
- **旋转绕物体原点而非包围盒中心**：手柄视觉中心在 OBB 中心，
  但 `orientation` 是绕自身原点的旋转，两者在物体原点偏离几何中心时有偏差。
- **选中框是 OBB**：局部 AABB × 世界矩阵，跟随旋转/缩放；拾取粗筛仍用世界 AABB，
  所以点选范围会比 OBB 略大（对细长/旋转物体尤其明显）。

## 配置与相机

- **退出会自动保存相机姿态**（`exit also saves`）。在编辑器里移动相机后正常关闭，
  下次启动会回到那个位置。这是刻意行为（引擎要记住视角），但如果想回到默认机位，
  需要手动改 `config/engine.json` 的 `camera` 段或删除该文件。
- **yaw 不做归一化**：长时间转视角后 `rotation.y` 会累积成很大的数值
  （实测出现过 -1101 rad）。功能上无影响（三角函数是周期性的），
  但配置文件里看着很怪。
## 程序化地形（D-18 ~ D-25）

- 手动地形是单实体；B-01 manager 支持相机中心有限半径的独立 chunk 实体流送，
  每 chunk 有自己的 Model。异步 Generate/流送使用 8 槽 GPU 高度场请求，
  主线程只 poll；同步场景加载仍阻塞。选择/删除/撤销以 manager 为单位，
  runtime chunk 不单独编辑；流送半径有限。LOD 边界已用全分辨率外环 +
  粗内部过渡带做拓扑级缝合， skirts 只是可关闭的保守回退。地形 LOD/可见性
  由 GPU selection pass 生成 indirect count；容量 64 entities / 1024 records。
- B-04 起单 chunk 顶点/索引由 `terrain_mesh.comp` 直接写 device-local buffer，
  Model 接管 VMA allocation；多 chunk 手动地形仍走 CPU builder。B-05 已把单
  chunk GPU mesh 改为 4 槽异步 submit/poll，CPU picking mirror 也在 worker 线程
  生成；默认 257² 异步重建主线程 poll 约 1ms，但同步场景加载仍阻塞。
- 场景加载仍同步；重复 Generate 会触发 Scene 级协作取消，但取消粒度是顶点行，不是硬中断。
- 地形材质最多 4 个 Set2 材质集，每个 set 固定 12 张纹理（height/roughness
  已打包）。set 0 启动时加载根目录，set 1~3 按需从 `setN/` 加载；16 张 PNG
  不齐就整套回退程序化纹理，仓库不附带美术资产。
- `TerrainConfig::materialSetIndex` 随场景 JSON 保存；渲染按相邻地形实体的
  material set 变化重绑 Set2，但未按材质集排序/合批，交替材质会增加
  descriptor switch。
- 地形片元当前约 16 次纹理采样/像素（8 albedo + 4 normal + 4 packed height/roughness）；
  后续可把四层 albedo 收进 texture array 或降低双尺度采样。
- 地形 Normal.w=0，不参与 SSAO；局部接触阴影留待后续。
- 地形高度场可由 GPU compute 生成；单 chunk 网格组装也在 GPU，但多 chunk
  仍由 CPU builder 生成。CPU 侧保留全分辨率 picking mirror 和 HeightField
  collision 源。B-07 已接入 kinematic 角色控制器与运行期雕刻：角色用局部包围盒
  + 双线性高度采样，笔刷只重建相交 chunk；但雕刻只改运行期 HeightField/Model，
  不写场景 JSON，Play 停止或保存重载会丢失；世界空间高度查询每次遍历地形实体。
- Transform/Bounds 字段已私有，setter / `markDirty()` 通过 owner callback
  自动通知 Scene，干净帧不做 Mesh/Bounds 扫描；`setPose()` 用于命令快照。
  `SpatialIndex` 增加 streaming chunk 的 XZ region bucket，但非 chunk 实体
  仍走统一网格；生产销毁统一走 `Scene::destroyEntity()`，独立命令栈自检
  的局部 registry 是唯一例外。地形实体只贡献一个总 AABB，chunk 剔除发生在
  GeometrySystem 子网格级。
- Scene 有 4 帧旧 Model 延迟回收队列；撤销历史仍会额外持有新旧 Model，
  历史上限 256 条，会占用显存。