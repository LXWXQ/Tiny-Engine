---
id: systems/character
title: 运行时角色与骨骼动画
type: system
scope: src/Scene/Model.*, src/Scene/GLTFLoader.cpp, src/Scene/Scene.cpp, src/Editor/EditorLayer.cpp, Resources/Models/Character
read-when: 改角色移动/出生点/动画状态机/蒙皮，或排查角色全黑、不动、悬空、动画不切换
size: ~150 行
updated: 2026-11-05
---

# 运行时角色与骨骼动画

运行模式（`F8`）里的第三人称角色：一个 kinematic 胶囊负责移动与贴地，一个 glTF 蒙皮模型负责外观，动画状态机按速度/离地状态切换 Idle / Walk / Run / Jump / Jump_Idle / Jump_Land。

本文只讲角色这一条路径；通用 glTF 静态模型、场景序列化和地形采样分别见
[scene-ecs](scene-ecs.md)、[serialization](../90-editor/serialization.md)、[terrain](terrain.md)。

## 1. 边界

- **单角色**：同一时刻只有一个运行模式角色，没有 NPC/多角色管理。
- **CPU 蒙皮**：不改 `Vertex` 布局，也不新增 skinning shader；每帧把顶点变换后写入 host-visible 动态缓冲。一个角色约几千顶点，Debug 下可接受。
- **无动画混合**：状态切换是硬切（`currentClip` 变化时把 `time` 归零），没有 cross-fade、分层、遮罩或动画事件。
- **无 root motion / IK / 动画编辑器**：位移由控制器决定，动画只改骨骼姿势；不能从动画里提取位移。
- **模型来源固定**：`Resources/Models/Character/Character.glb`，Quaternius Platformer Game Pack，CC0；细节与 SHA256 见同目录 `README.md`。

## 2. 资产与导入

`GLTFLoader` 对带 `skin` 的图元走单独分支：

- 蒙皮顶点**保留 mesh local**，不乘 node world；普通顶点仍然烘到世界空间。这是为了不让骨架里的 scale 算两遍。
- `JOINTS_0` / `WEIGHTS_0` 按 `skinJointSlots` 映射到全局 joint 槽位（多个 skin 可以共享一个 `jointNodes` 数组）。
- 骨架节点保存 `translation/rotation/scale/parent`；动画通道按 node index 采样。
- 动画名读取 glTF animation 的 `name`；导入时把最后一个 `|` 之前的前缀剥掉（例如 `CharacterArmature|Walk` → `Walk`），状态机只认短名。
- 没有 `baseColorTexture` 的材质把 `materialAlbedoFallback` 置 0，保留 `baseColorFactor` 纯色；否则会落进棋盘格 fallback，角色会变成灰白格子。

日志：

```text
[GLTF] 蒙皮: joints=29 animations=18 skinVertices=...
[Character] model attached | .../Character.glb | skinned=1 animations=18 scale=0.5
```

## 3. 模型侧：骨架、clip 与动态缓冲

`Model` 在构造时额外保存：

- `bindSkeletonNodes`：绑定姿态的骨架节点；
- `jointNodes` / `inverseBindMatrices`：参与蒙皮的 node 与 IBM；
- `animations`：`AnimationClip{name, duration, channels}`，channel 带 STEP/LINEAR 采样；
- `cpuVertices` / `skinJoints` / `skinWeights`：CPU 蒙皮输入；
- 3 个环形 host-visible 动态顶点缓冲（`std::array<DynamicVertexBuffer, 3>`）。

`applyAnimation(clipIndex, time)` 的步骤：

1. 从绑定姿态复制一份 `posed`；
2. 按 clip 采样每个 channel：translation/scale 线性插值，rotation 用 slerp；STEP 通道不插值；
3. 递归算骨架 world 矩阵，乘 IBM 得到每个 joint 的 `jointWorld * inverseBind`；
4. 每个顶点对 4 个 joint 加权（位置和法线都变换），写进当前环形缓冲，再 `vmaFlushAllocation`；
5. 缓冲索引循环前进，避免 GPU 还在读上一帧的缓冲。

注意：`isSkinned()` 为真时 `isDynamic()` 也为真，渲染合批必须跳过它
（详见 §6 的坑）。

## 4. 场景侧：角色实体与控制

`Scene::createCharacterEntity(name, spawnPosition)` 只建 Transform + Name + `CharacterControllerComponent`，此时实体没有 Mesh，**逻辑上**是不可见胶囊；外观由 `attachCharacterModel()` 再挂。

`Scene::attachCharacterModel(entity, glbPath, uniformScale)`：

- 加载 glTF，必须是蒙皮模型（`isSkinned()`），否则返回 false；
- 替换 `MeshComponent`，旧模型走 `retireModel()`；
- 设置均匀缩放（运行模式传 0.5，把 3.55m 绑定姿态缩到约 1.78m）；
- 挂 `CharacterAnimationComponent`，初始 clip = `Idle` 并 apply 到第 0 秒；
- 更新 Bounds / 空间索引，打印 `[Character] model attached`。

`Scene::updateCharacterController(entity, worldMoveDirection, jump, dt, out, speedMultiplier)` 每帧：

- 用 `sampleTerrainGroundHeightWorld`（地形 + 体素都走这条查询）找脚下地面；
- 处理水平移动、重力、跳跃、step up、坡度限制和 ground snap；
- 角色只绕 Y 轴转向：`setEuler(0, atan2(move.x, move.z), 0)`，前向为 +Z，与 glTF 模型默认朝向一致；
- 把水平速度与 `justLanded` 交给 `updateCharacterAnimation()`。

控制器参数默认值在 `CharacterControllerComponent`：半径 0.35、身高 1.8、移速 6、跳跃初速 6、重力 18、最大坡度 50°、step 0.5、snap 0.4。运行模式把 `speedMultiplier` 在 0.55（走）和 1.35（Shift 跑）之间切。

## 5. 动画状态机

`updateCharacterAnimation()` 的优先级（从高到低）：

| 条件 | clip | 循环 |
|---|---|---|
| `landTimer > 0` | Jump_Land | 否，播完停在末帧 |
| 离地且 `verticalVelocity > 0.2` | Jump | 否 |
| 离地且下落 | Jump_Idle | 是 |
| 着地且水平速度 > 5 | Run | 是 |
| 着地且水平速度 > 0.2 | Walk | 是 |
| 其他 | Idle | 是 |

- 落地沿 `justLanded`（上一帧未着地、这一帧着地、下落速度 < -1）触发，`landTimer = 0.45s`；
- clip 名找不到时保持当前 clip，不会崩；
- `time` 按 `playbackSpeed` 前进；循环 clip 取模，一次性 clip 夹到 duration；
- 模型每帧 apply 一次，CPU 顶点在下一帧绘制前已就绪。

运行模式日志只在 clip 名变化时打印一次：

```text
[Runtime] clip=Idle feet=0,12.3,4.1 grounded=1
[Runtime] clip=Run feet=0,12.3,12.8 grounded=1
```

## 6. 编辑器侧：进入/退出与相机

`EditorLayer::enterRuntimeMode()`：

1. 保存相机位置/欧拉角，直接把弧度存进 `runtimeYaw/runtimePitch`（**不能再按度转一次**）；
2. 从相机沿视线 ray-march（2..180m，步长 1m），在每个 probe 点查地面；命中条件 `probe.y ∈ [ground-6, ground+0.1]`，取第一个命中点作为出生点；
3. 找不到就退化为相机前方 2m、脚底 = 相机 y - 1.62，并让重力接管；
4. 建角色实体，初始朝向 = `runtimeYaw`；
5. 挂 `Character.glb`，缩放 0.5；失败则保留空控制器并提示。

`applyRuntimeCameraPose()`：焦点 = 脚底 + 1.25m，相机在焦点后方 4m（沿视线反方向），且不低于脚底 + 0.6m；pitch 夹在 ±85°。

退出时销毁角色实体，并把相机还原到进入前保存的位姿；编辑模式的选中状态不受影响。

### 操作

| 输入 | 行为 |
|---|---|
| `WASD` | 前后左右走（方向跟随 yaw） |
| `Shift` | 跑（`speedMultiplier=1.35`） |
| `Space` | 跳 |
| 中键拖动 | 转视角（0.01 rad/px，pitch ±85°） |
| 左键 / 右键 | 挖方块 / 放方块（或地形笔刷，和编辑模式同一份代码） |
| `F8` | 回编辑模式 |

`--runtime-mode` 让 `TinyEditor.exe` 启动后直接进入运行模式，便于截图/演练；它只影响编辑器层，不改变命令行里其他渲染开关。

## 7. 验证与观测量

- `TinyEditor.exe --selftest 5 --empty-scene` 的 e9 自检：
  - 进入运行模式后角色实体存在、相机跟随；
  - 相机位姿在退出后还原；
  - 角色模型 `skinned=1 animations=18`；
  - 6 个 clip 名都能 `findAnimation`；
  - 初始 `currentClipName == "Idle"`；
  - PASS 行：`[Editor] runtime character selfTest PASS | camera follows feet, exit restores pose | skinned=1 animations=18 clips=Idle/Walk/Run/Jump/Jump_Idle/Jump_Land active=Idle`。
- `tools/Validate.ps1` 的 `e9-runtime-character` 断言上述 PASS 行与 clips 字段。
- 手工回归：进入运行模式后按 `W`/`Shift+W`/`Space`，日志应依次出现 `clip=Walk`/`clip=Run`/`clip=Jump` → `clip=Jump_Land` → `clip=Idle`。

## 8. 已知边界

- 单角色、单套骨架；没有动画混合/过渡、动画事件、IK、root motion。
- CPU 蒙皮每帧重算全部顶点；角色数量上来后必须改为 GPU skinning 或实例化。
- 出生点只沿相机视线在 2..180m 内找地面；相机完全朝天或脚下没有可站立表面时会走“前方 2m + 重力”兜底。
- 动态顶点缓冲是 3 帧环形；如果未来把角色渲染延迟到 3 帧以上（例如额外的时间性 pass），需要同步扩大环大小。
