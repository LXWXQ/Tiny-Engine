---
id: systems/scene-ecs
title: 场景、ECS、Transform 层级与空间索引
type: system
scope: src/Scene/Scene.h, src/Scene/Scene.cpp, src/Scene/Components.h, src/Scene/ComponentTypes.h, src/Scene/ComponentRegistry.h, src/Scene/SystemScheduler.h, src/Scene/FeatureComponents.h, src/Scene/NativeComponents.h, src/Scene/SceneComponents.h, src/Scene/SceneComponents.cpp, src/Scene/SpatialIndex.h, src/Scene/GLTFLoader.cpp, src/Scene/Model.cpp
read-when: 修改场景结构、增删组件、注册新组件/系统、改变换传播或剔除逻辑
size: ~430 行
updated: 2026-11-10
---

# 场景、ECS、Transform 层级与空间索引

## 1. 组件

`src/Scene/Components.h` 定义了引擎内置实体组件；场景级渲染功能组件在 `src/Scene/SceneComponents.h`（见 §1.2）：

| 组件 | 内容 |
|---|---|
| `TransformComponent` | local `translation` / `rotation` / `scale` + `orientation`，带 `localRevision`/`dirty`；通过 setter 修改 |
| `WorldTransformComponent` | `worldMatrix` / `normalMatrix` / `worldPosition` + `sourceRevision`/`valid` |
| `HierarchyComponent` | `parent` + `children` |
| `MeshComponent` | 指向 `Model` 的子网格 |
| `MaterialComponent` | 材质槽位 |
| `RenderTagComponent` | `isSkybox` / `isGrid` / `isTerrain` |
| `BoundsComponent` / `WorldBoundsComponent` | 局部 / 世界 AABB，分别带 dirty/revision 标记 |
| `CameraComponent` | 相机标签 |
| `TerrainComponent` | 地形参数、生成结果统计、`generationBackend` / `meshGeneratedOnGpu`、`generating` 状态与错误（不含 GPU 句柄） |
| `TerrainStreamingComponent` | 流送 manager：chunk 模板、半径、每帧加载预算 |
| `TerrainChunkComponent` | 运行期 chunk 标记与 chunk 坐标；不逐项序列化 |
| `CharacterControllerComponent` | kinematic 角色：半径/高度/移速/跳跃/重力/坡度/step/snap；`verticalVelocity`/`grounded`/`groundEntity` 为运行期状态 |
| `CharacterAnimationComponent` | 运行模式角色的动画状态机：当前 clip / time / loop / landTimer + 6 个 clip 名（默认 Idle/Walk/Run/Jump/Jump_Idle/Jump_Land） |

以上是**引擎内置、有专用代码路径**的实体组件。在此之上有两层注册组件：实体级元数据组件（§1.1）与场景级渲染功能组件（§1.2）；它们都不需要改 `Components.h`，Inspector 与序列化走同一张描述表。

### 1.1 元数据组件与系统调度（F5-01 / F5-04）

EnTT 只提供存储。F5-01 在它上面加了两件事：**组件描述表**和**按阶段运行的系统**，
让“把某个特性挂到实体上”变成数据操作，而不是每个特性复制一遍编辑器/序列化代码。

**组件描述（`src/Scene/ComponentTypes.h` + `ComponentRegistry.h`）**

`ComponentDescriptor` 为一个 C++ 组件类型记录：稳定 id（如 `effects.spin`）、显示名、
分类、是否在 `+ Add Component` 菜单暴露，以及属性列表。每个属性是一个
`PropertyDescriptor`：稳定属性名、显示名、`PropertyType`、类型擦除的 getter/setter。
`makeComponentDescriptor<T>()` 负责把具体类型包成 `addDefault` / `has` / `remove` /
`read` / `write` / `defaults` 回调；`read`/`write` 只在注册属性上工作，不碰裸组件内存。

属性类型目前是固定集合：`Bool`、`Int`、`Float`、`Vec3`、`Color`（vec4）、`String`。
`ComponentRecord` 是这些值的纯数据快照（`type` + `NamedProperty` 列表），
不依赖 EnTT / Vulkan，供序列化、命令栈、复制/删除共用。

**系统调度（`SystemScheduler.h`）**

`SystemPhase::Simulation` 在 `updateWorldTransforms()` **之前**运行，适合改变换/状态；
`SystemPhase::PostTransform` 在之后运行。每个系统有 `id`、`order`（稳定排序）和一个
`(entt::registry&, float dt)` 回调。调度器在运行前拷贝阶段表，所以系统内部可以安全地
注册/移除系统。

`Scene::update()` 的两个分支（profiling / 非 profiling）都在
`processVoxelChunkBuilds()` 之后、`refreshSpatialIndex()` 之前执行：

```
Simulation systems -> updateWorldTransforms() -> PostTransform systems -> refreshSpatialIndex()
```

**扩展组件（第一批，`FeatureComponents.h`）**

| 组件 id | 分类 | 属性 | 系统行为 |
|---|---|---|---|
| `effects.spin` | Effects | `axis` / `degreesPerSecond` / `enabled` | 每帧绕本地轴累乘四元数 |
| `effects.orbit` | Effects | `center` / `radius` / `degreesPerSecond` / `angle` / `enabled` | 累加角度并写世界平移 |
| `effects.bob` | Effects | `axis` / `amplitude` / `frequency` / `phase` / `enabled` | 沿轴正弦浮动，增量写平移以保留手动拖动 |
| `effects.sway` | Effects | `axis` / `amplitudeDegrees` / `frequency` / `phase` / `enabled` | 绕轴正弦摆动，增量乘四元数 |
| `effects.pulse` | Effects | `amplitude` / `frequency` / `phase` / `enabled` | 缩放脉冲，增量乘缩放以保留手动缩放 |
| `effects.lookAt` | Motion | `target` / `up` / `turnSpeed` / `enabled` | 朝向 target；`turnSpeed=0` 瞬时，否则 slerp |
| `effects.follow` | Motion | `target` / `speed` / `stopDistance` / `enabled` | 向 target 匀速靠近并在 stopDistance 停下 |

系统顺序为 Spin 100、Orbit 110、Bob 120、Sway 130、Pulse 140、Follow 150、Look At 160。
Look At/Follow 的 target/up 是父空间坐标；顶层实体即世界坐标。它们都是可用的效果/运动，
也是新组件的模板：注册组件 +（可选）注册系统，不需要改 Inspector、SceneSerializer 或 Commands。

**原生组件接入（第二批，F5-04）**

F5-04 把已有原生组件也纳入同一张描述表，但**不复制它们的序列化路径**。
`src/Scene/NativeComponents.cpp` 注册 9 个 `native.*` 描述符：

| 描述符 | 分类 | 可添加 | 说明 |
|---|---|---|---|
| `native.name` / `native.transform` / `native.hierarchy` | Core | 否 | 必需组件，始终存在，只读；`required=true` |
| `native.characterController` | Scene | 是 | 8 个 Float 属性（radius/height/moveSpeed/jumpSpeed/gravity/maxSlopeAngle/stepHeight/groundSnapDistance），固定 Inspector 面板绘制 |
| `native.renderTag` | Scene | 是 | `skybox` / `grid` 两个 Bool；`isTerrain` 是派生只读，地形实体禁止移除 |
| `native.camera` | Managed | 否 | 唯一相机，随场景创建；菜单中置灰并说明原因 |
| `native.terrain` | Managed | 否 | 用 创建 → 地形 创建；裸加组件无法生成网格 |
| `native.terrainStreaming` | Managed | 否 | 用 创建 → 流送地形 创建 |
| `native.voxelWorld` | Managed | 否 | 用 创建 → 体素世界 创建 |

描述符新增五个标志，含义是：

- `addable` / `removable`：控制 `+ Add Component` 与右键移除；不可添加项仍显示为禁用菜单项，悬停给出 `unavailableReason`。
- `drawProperties`：`false` 表示属性由固定 Inspector 面板绘制，通用组件列表只显示标题，避免重复控件。
- `serialized`：`false` 表示不进入 v3 `components`；原生状态仍走 `EntityDescription` 的专用字段。
- `required`：Name/Transform/Hierarchy 不可增删。
- `canRemove`：按实体状态否决删除（Render Tag 在地形实体上返回 false）。

属性描述符另带一组**纯 UI 元数据**（F5-05，不写入 JSON）：`hasRange` / `minimum` / `maximum` /
`dragSpeed` / `tooltip`。注册时用 `withRange()` / `withTooltip()` 链式附加，例如
`withTooltip(withRange(makeSpinSpeedProperty(), -3600, 3600, 0.5f), "局部自转速度")`；
`makeFloatProperty` 同时把范围用于 setter clamp。Inspector 只消费这些字段，不再为每个组件写一套控件。

因此 `+ Add Component` 的可选项是 7 个 `effects.*` + Character Controller + Render Tag；
Camera/Terrain/Streaming/Voxel 以置灰项出现。另有 9 个 `scene.*` 场景组件，只出现在无选中实体的
Scene Components 区，见 §1.2。Character 面板的 8 个参数与 Render Tag 的移除按钮
都走 `SetComponentRecordCommand`，可撤销。`DeleteEntityCommand` 为 Character 增加显式快照，
因为 `native.*` 不会进入通用 `components` 数组；Render Tag 原本就有专用快照。

**Scene 公开 API**

| API | 语义 |
|---|---|
| `getComponentRegistry()` / `getSystemScheduler()` | 注册表与调度器 |
| `addComponent(e, type)` / `removeComponent` / `hasComponent` | 按注册 id 增删查；`addComponent` 检查 `addable`，`removeComponent` 检查 `removable`/`required`/`canRemove` |
| `readComponent(e, type)` | 取当前值快照；未挂载返回 `nullopt` |
| `defaultComponentRecord(type)` | 新建组件时的默认 record |
| `applyComponentRecord(e, record)` | 写入快照；缺组件时按默认值新建 |
| `captureComponentRecords(e)` | 收集实体上所有已注册且 `serialized` 的组件；原生组件走专用字段，不在此列 |
| `getSceneSettingsEntity()` | 隐藏的场景设置实体，9 个场景组件的宿主；不是可见实体 |
| `addSceneComponent(type)` / `removeSceneComponent` / `hasSceneComponent` | 场景组件增删查；只能作用于设置实体，增删后立即同步 `EngineSettings` |
| `captureSceneComponents()` | 捕获设置实体的全部场景组件 record |
| `resetSceneComponentsFromSettings()` / `clearSceneComponents()` | 按当前 `EngineSettings` 重建 9 个组件 / 全部移除并关闭功能 |
| `syncSceneSettings()` | 把场景组件写回渲染缓存的 `EngineSettings` |

**编辑器与持久化**

- Inspector 先画固定原生面板（Transform/Character/Terrain/Streaming/Mesh/Material/Render Tag），
  再显示已挂载且 `drawProperties` 的注册组件。组件区顶部是 `+ Add Component` 与剪贴板粘贴按钮；
  Add 弹窗支持按名称/分类/id 搜索，组件头默认展开。
- 属性控件按注册表元数据设置范围、拖动速度和悬停提示；右键属性可单独“重置为默认值”。
  组件头右键与原生面板底部提供复制 / 粘贴 / 重置 / 移除，全部走 `SetComponentRecordCommand`，可撤销。
  剪贴板是跨实体的纯数据快照，复制不置脏；粘贴要求同类型、目标可添加且值不同，
  重置使用 `defaultComponentRecord(type)`。
- `+ Add Component` 按分类列出未挂载且 `exposed` 的组件，不可添加的托管项置灰并显示原因。
- `SceneSerializer` 从 **version 4** 起把场景组件写进顶层 `sceneComponents`；实体 `components`
  仍从 v3 起（格式见 [serialization](../90-editor/serialization.md)）。`Scene::captureDescription()`
  只捕获已注册且 `serialized` 的组件；场景组件走 `captureSceneComponents()`，只允许 Scene scope。
  `buildFromDescription()` 逐条 apply，未注册/scope 不符的类型安全跳过，文件里伪造的 `native.*`
  记录会被忽略；旧文件缺 `sceneComponents` 时按当前 `EngineSettings` 重建 9 个组件。
  原生组件仍由专用字段存盘，不会重复写入。预制体复用实体描述：`captureDescription(root)` 收子树、
  `instantiatePrefab()` 追加恢复，顶层 `sceneComponents` 为空，见 [prefabs](../90-editor/prefabs.md)。
- `DeleteEntityCommand` 的快照与 `DuplicateEntityCommand` 都会带上注册组件 record，
  所以删除撤销/复制不会丢效果；Character 另由显式原生快照覆盖。

**边界**：组件和系统仍在 C++ 编译期注册，没有脚本/热重载/属性数组；属性范围/提示只是 UI 元数据，
不参与序列化。系统在编辑模式也会随 `Scene::update()` 推进，撤销删除后实体 ID 会变。
原生组件只把 Character/Render Tag 接进统一增删；Mesh/Material/Terrain chunk 等仍由
专用创建流程或固定面板管理。场景组件只有 9 个渲染功能，不覆盖 GPU scene/merged buffer 等调试开关，
也不改变“渲染器读 EngineSettings”的缓存结构。

### 1.2 场景级组件（scope=Scene，F5-06）

用户要在场景里像挂组件一样管理渲染功能：添加阴影、启用 Bloom、调雾密度，移除即关闭，
并且只维护一套组件 UI。F5-06 把 8 个渲染功能 + 场景环境做成 **Scene scope** 组件。

`ComponentScope` 在 `ComponentRegistry.h` 中定义，`Entity` / `Scene` 两种。描述符带 `scope`，
`makeComponentDescriptor<T>` 透传。当前共 **25 个描述符**：16 个实体组件（§1.1）+ 9 个场景组件。

| 组件 id | 名称 | 分类 | 属性（均含 `enabled`） |
|---|---|---|---|
| `scene.environment` | 场景环境 | 场景环境 | `exposure` / `renderScale` / `lightDirection` / `lightColor` / `lightIntensity` |
| `scene.shadows` | 阴影 | 光照 | `distance` / `strength` |
| `scene.bloom` | Bloom 辉光 | 后处理 | `threshold` / `knee` / `intensity` |
| `scene.ssao` | SSAO 环境遮蔽 | 后处理 | `radius` / `bias` / `kernelSize` |
| `scene.ssr` | SSR 屏幕空间反射 | 后处理 | `maxDistance` |
| `scene.motionBlur` | 运动模糊 | 后处理 | `strength` / `maxRadius` |
| `scene.taa` | TAA 时域抗锯齿 | 后处理 | `blend` / `jitterScale` |
| `scene.water` | 水面 | 水体 | `level` / `waveStrength` / `waveSpeed` / `planarReflection` |
| `scene.fog` | 距离雾 | 后处理 | `start` / `density` |

**宿主实体**：`Scene` 构造时 `registry.create()` 一个 `sceneSettingsEntity`，不挂 Transform / Hierarchy / Name。
Hierarchy、可见实体遍历和普通 `captureDescription()` 都看不到它；`clearEntities()` 只销毁带 Transform 的实体，
所以它跨“新建/加载场景”存活；`destroyEntity()` 对它直接返回。`acceptsComponentTarget()` 强制双向约束：
Scene scope 只能挂设置实体，Entity scope 不能挂设置实体；通用 add/remove/has/read/apply/capture 都走这条校验。

**同步到渲染**：场景组件是功能开关与参数的来源，但渲染 pass 仍每帧读 `EngineSettings` 缓存。
`Scene::syncSceneSettings()` 调 `scene::syncSceneSettings(registry, settingsEntity, settings)`：先把
本函数负责的字段恢复成 `EngineSettings{}` 默认值，再让“存在且 `enabled=true`”的组件覆盖。
因此**组件缺失或禁用 = 功能关闭**；环境组件的方向光会归一化（近零向量回默认），
`lightColor.rgb * lightIntensity` 写入 `directionalLightColor`。add/remove/apply 场景组件后立即同步，
不依赖每帧轮询，避免覆盖调试命令行直接改 settings 的路径。

**初始化与兼容**：Scene 构造先挂 9 个默认组件；`Application` 载入 `engine.json` 后、
`FirstApp::loadConfig()` 与 `newScene()` 都调用 `resetSceneComponentsFromSettings()`，
让默认画面与旧配置一致。v4 场景文件**没有** `sceneComponents` 字段时同样按当前 `EngineSettings` 重建；
字段存在但为空数组表示用户移除了全部功能。预制体只捕获实体子树，不携带场景组件。

**边界**：场景组件仍编译期注册，没有脚本/热插拔；`EngineSettings` 仍是渲染读取缓存，不是场景文档的
source of truth；GPU scene 批、merged buffers、Hi-Z 等调试开关不在场景组件里，仍由 Render 面板管理。
当前旧场景初始化使用“当前 `EngineSettings`”，若上一个场景已通过移除组件把 settings 改成全关，
下一个无 `sceneComponents` 的旧场景会继承这份运行时状态，而不是 `engine.json` 原值。

## 2. Transform 层级

`Scene::setParent()` 维护 children，**拒绝自环与循环**。

每帧按固定顺序计算世界变换：

```
World = T * mat4_cast(orientation) * S
```

`orientation` 是 `glm::quat`，旋转的唯一事实来源；`setEuler()` 把
`(x,y,z)` 转成 `qy * qx * qz`，与旧的 `T*Ry*Rx*Rz*S` 完全等价。
用 `World` 的左上 3x3 求逆再转置得到 `normalMatrix`。
顺序固定（`T*Ry*Rx*Rz*S`）是刻意的——不同顺序会得到不同的旋转结果，
而配置里存的是欧拉角，必须与渲染一致。

B-06 起 Transform/Bounds 字段私有，只暴露只读 getter、精确 setter 和
`setPose()`：

- 读取用 `getTranslation()` / `getScale()` / `getEuler()` /
  `getOrientation()` / `getLocal()` / `getRevision()`；写入用
  `setTranslation()` / `setScale()` / `setEuler()` / `setOrientation()` /
  `setLocal()` 或显式 `markDirty()`；setter 对未变化的值早退；
- `setPose()` 一次写入平移/欧拉/缩放/四元数，只标脏一次，供命令 undo/redo
  保留完整快照；`clearDirty()` 由 Scene 消费 dirty 集时调用；
- `Scene::attachDirtyTracking()` 把 Transform/Bounds 的回调绑定到所属实体；
  setter / `markDirty()` 会直接填充 Scene 的 dirty 队列，不再要求调用方额外
  `markTransformDirty()` / `markSpatialDirty()`；
- 快照恢复前清空回调，组件 emplace 后重新绑定，避免撤销到不同实体 ID 时
  通知旧实体；
- `updateAllWorldTransforms()` 只重算 dirty 节点；父节点变化会强制整棵子树
  重算，父节点干净时仍会向下遍历以找到脏后代；
- 重算节点会写 `sourceRevision`/`valid`，并把已有 `WorldBoundsComponent` 标脏；
- 字段私有后编译器阻止直接字段写入；`registry.destroy()` 仍可能绕过 Scene
  清理，生产路径必须走 `Scene::destroyEntity()`。

## 3. 空间索引与剔除

```
世界 AABB 插入均匀网格（broad phase）
   -> 查询：与相机视锥 AABB 相交的网格单元
   -> 精确：frustum-AABB 测试
   -> 结果写入 Scene::visibleEntities
```

`GeometrySystem` 只遍历 `visibleEntities`，不再逐个实体做粗剔除判断。
地形实体进入该列表后，B-03 GPU selection pass 仍会在 chunk 级做精确
frustum-AABB、LOD 选择与 indirect count 输出。

阶段 7 起索引桶改为**增量 upsert/remove**：

- `SpatialIndex` 为每个 entity 记录它占用的 cell 列表；`insert` 先移除旧条目，
  `remove` 精确清掉旧 bucket，避免移动物体留下幽灵候选；
- `Scene::refreshSpatialIndex()` 只对 `BoundsComponent::dirty` 或
  `WorldBoundsComponent::dirty` 的实体重算世界 AABB 并 upsert；干净实体只做
  一次布尔检查；销毁/改为 skybox 的实体从索引删除；
- 世界变换也只传播 dirty 子树；`Scene` 暴露
  `getLastWorldTransformUpdates()` / `getLastSpatialIndexUpdates()`；
- Scene 维护 `dirtyTransformEntities` / `dirtySpatialEntities` 集合；正常 setter
  通过 owner callback 自动填充，保留 `markTransformDirty()` / `markSpatialDirty()`
  供批量或诊断路径使用；干净帧不扫描 Mesh/Bounds，也不做 stale sweep；
- `Scene::destroyEntity()` 同步从 SpatialIndex 与 dirty 集合移除实体；
- B-06 在统一 3D grid 之外增加 coarse XZ region bucket：
  `SpatialIndex::setRegionSize()` / `queryRegion()` /
  `queryRegionRange()`；Scene 把 region size 对齐 streaming chunk size，
  `queryTerrainChunksInRegion()` / `queryTerrainChunksInRegionRange()` 只返回
  当前 manager 的有效 chunk；
- 剩余契约：`registry.destroy()` 仍可能绕过 Scene 清理；区域索引当前只覆盖
  streaming chunk，其他实体仍走统一网格。

见 [weak-points](../10-architecture/weak-points.md) #3。

## 4. 模型加载

### 4.1 glTF（`GLTFLoader`）

- 遍历节点时**累积变换并直接把顶点烘焙到世界空间**（见
  [decisions](../10-architecture/decisions.md) 决策 13）。
- 子网格按 primitive 拆分，每个 primitive 一个材质槽位。
- 加载完成后几何合并进一个顶点缓冲：Sponza 合并为
  `顶点 192496 / 索引 786801 / 子网格 103`。

**后果**：Sponza 内部节点不是独立可动 entity。要局部动态必须保留 node 层级
（[roadmap](../80-roadmap/roadmap.md) F4）。

### 4.2 OBJ（`Model`）

- 顶点从 obj 直接读，UV 需要 `1.0 - v` 翻转（glTF 的 UV 原点在左上，
  与 Vulkan 一致；OBJ 在左下）。**两条路径翻转策略不同，别统一**。
- 同样支持子网格与材质分配。

## 5. 材质槽位

每个子网格持有一个 bindless 材质槽位（`MaterialSlot.h` 的 `kInvalidMaterialSlot`）。
加载时 `DescriptorManager::allocateMaterialSet()` 分配。当前**没有运行期卸载路径**，
所以槽位只增不减（场景反复加载会慢慢吃满 256 槽）——
见 [tech-debt](../70-status/tech-debt.md)。

## 5.5 地形实体

地形是普通 ECS 实体：`TransformComponent + MeshComponent + TerrainComponent +
RenderTagComponent(isTerrain) + BoundsComponent`，**没有 MaterialComponent**。
`Scene::createTerrainEntity()` 成功生成 `Model` 后才创建实体；单 chunk 优先走
`TerrainGpuMeshBuilder` 的 device-local VBO/IBO，多 chunk/失败时回退 CPU builder
+ staging 上传。`Scene::regenerateTerrain()` 失败时保留旧
`MeshComponent`/`generatedConfig`。
显式 Generate 走 `Scene::requestRegenerateTerrain()`：GPU compute 先生成高度场，
随后单 chunk 由 4 槽异步 GPU mesh builder 组装；不支持或 submit/poll 失败时
回退 CPU JobSystem。`Scene::update()` 在主线程 poll/安装，slot 满时保持
pending，不阻塞等 fence；新请求 supersede 旧请求并 release
在途 GPU mesh 请求。`generationBackend` 会区分 `GPU height + GPU mesh`、
`GPU height`、`GPU mesh`、`CPU fallback`。

地形实体的 `MeshComponent::model` 仍然参与空间索引、视锥剔除、CPU 三角形拾取和
选中 OBB。B-07 起 `Scene::sculptTerrain()` 可修改运行期高度场并按 region 查询
只重建相交 chunk；`sampleTerrainGroundHeightWorld()` 用局部包围盒 + 双线性采样
服务角色贴地，`raycastTerrainWorld()` 供编辑器笔刷取最近命中。
生成算法、参数与渲染细节见 [terrain](terrain.md)。

`Scene::createStreamingTerrain()` 创建的 manager 是普通 ECS 实体，但不持有
Mesh/Bounds；相机移动时 Scene 为半径内每个 chunk 创建一个独立实体。chunk 实体
各自拥有 Transform/Mesh/Bounds/TerrainComponent，因此空间索引、LOD 状态缓存和
间接合批都按实体独立工作。每个 chunk 的 `chunkGridX/Z` 都是 1，B-04 起默认
由 GPU mesh builder 生成并标记 `meshGeneratedOnGpu=true`；CPU heightfield 继续
供 collision 使用。manager 序列化模板；chunk 集在加载后按相机重建。

## 6. 自检

```cpp
Scene::selfTest();
```

覆盖：父子/孙节点的世界变换传播、dirty 子树计数、空间网格查询与增量 upsert/remove、
XZ region 重建/范围查询/移除；以及元数据组件注册表（add/read/write/remove/default）、
`ComponentRecord` 值往返、7 个系统各跑一个完整周期（Spin 转 45°、Bob 到 0.25 再回 0、
Pulse 到 1.15 再回 1、Sway 摆动后回正、Look At 朝 +X、Follow 按 speed 靠近并在 stopDistance 停），
以及原生组件描述符（Character/Render Tag 增删、必需组件拒删、`captureComponentRecords` 不含 `native.*`），
以及 9 个场景组件的 scope 约束、`EngineSettings` 同步、capture/clear/reset 与隐藏实体保护。
输出标记：

```
[Scene] ECS/spatial selfTest PASS | hierarchy=T*Ry*Rx*Rz*S | grid cells=8
[Scene] ECS component selfTest PASS | components=25 | systems=7
```

## 7. 改动注意

- 加**元数据组件**：在 `FeatureComponents.cpp`（或新文件）写组件 + 描述符注册，必要时在
  `registerFeatureSystems()` 加系统，再在 `Scene::selfTest()` 补校验；不需要改 Inspector、
  序列化或命令栈。
- 加**内置组件**（有专用渲染/物理路径）仍要改 `Components.h`、场景捕获/重建、序列化和
  对应 Inspector 面板，并在 `Scene::selfTest()` 补校验。
- 往统一菜单接入内置组件：在 `NativeComponents.cpp` 注册描述符，并设
  `serialized=false`、`drawProperties=false`（由固定面板绘制），不要让它重复写进 v3
  `components`；必需/托管项用 `required`、`addable`、`removable`、`unavailableReason` 标注。
- 改变换顺序要同步 `CameraController` 与配置里存的欧拉角约定；
  旋转矩阵由 `TransformComponent::orientation` 生成，欧拉角只做显示/序列化。
- 改剔除逻辑后确认 Sponza `draw calls` 仍是 104（少了说明误剔除）。
