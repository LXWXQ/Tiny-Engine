---
id: editor/serialization
title: 场景序列化
type: system
scope: 场景描述模型、文件格式、以及"什么该存什么不该存"
read-when: 改 *.scene.json 格式、加组件要存盘、或加载后场景不对
size: ~240 行
updated: 2026-11-10
---

# 场景序列化

## 三层分工

```
entt::registry  <-- Scene::captureDescription / buildFromDescription -->  scene::SceneDescription
scene::SceneDescription  <-- SceneSerializer -->  JSON 文本
```

切开的原因：

- `SceneDescription` 是**纯数据**，不含任何 Vulkan / EnTT 类型
- 序列化逻辑可以脱离 Vulkan 设备单独测试（自检里的 `roundTrip` 就是这样）
- 预制体（Prefab）、场景 diff、整场景撤销，都以它为单位

## 什么存，什么不存

| 存 | 不存 | 为什么 |
|---|---|---|
| 实体名、变换、父子关系 | 实体 ID | ID 是运行期分配的，跨进程无意义 |
| 网格**文件路径** | `VkBuffer` / `VmaAllocation` | GPU 句柄不能序列化 |
| 材质名字 + PBR 参数 + 贴图路径 | 材质**槽位下标** | 槽位是 `DescriptorManager` 运行期分发的 |
| 渲染标签（skybox/grid/terrain） | `SpatialIndex` / `indexedBounds` | 可增量刷新的派生数据，不存盘 |
| 地形参数对象 | 地形顶点/索引/GPU 句柄/纹理 | 加载时按相同 seed/算法重新生成；16 张地形纹理由启动代码确定性生成 |
| 元数据注册的实体组件（v3 `components`；`native.*` 不写入） | 未注册的运行期组件 | 只有注册过的类型能稳定读写；运行期临时组件没有稳定的磁盘语义 |
| 场景级组件（v4 顶层 `sceneComponents`，`scope=Scene`） | GPU scene / merged buffer / Hi-Z 等编辑器调试开关 | 功能存在性和参数属于场景；调试开关属于引擎/编辑器配置 |

父子关系用**数组下标**表示（`parent: 2`），不用实体 ID。
加载时按顺序重建即可。

## 文件格式（version 4）

```json
{
  "version": 4,
  "sceneComponents": [
    {
      "type": "scene.shadows",
      "properties": [
        { "name": "enabled", "type": "bool", "value": true },
        { "name": "distance", "type": "float", "value": 90 },
        { "name": "strength", "type": "float", "value": 0.65 }
      ]
    }
  ],
  "entities": [
    {
      "name": "Sponza",
      "translation": [0, 0, 0],
      "rotation": [0, 0, 0],
      "scale": [1, 1, 1],
      "parent": -1,
      "mesh": "Models/Sponza/glTF/Sponza.gltf",
      "skybox": false,
      "grid": false,
      "camera": false,
      "material": {
        "name": "glTF/Default",
        "baseColorFactor": [1, 1, 1, 1],
        "metallicFactor": 1.0,
        "roughnessFactor": 1.0,
        "alphaCutoff": 0.5,
        "alphaMode": 0,
        "albedoMap": "Textures/xxx.png"
      },
      "components": [
        {
          "type": "effects.spin",
          "properties": [
            { "name": "axis", "type": "vec3", "value": [0, 1, 0] },
            { "name": "degreesPerSecond", "type": "float", "value": 45 },
            { "name": "enabled", "type": "bool", "value": true }
          ]
        }
      ]
    }
  ]
}
```

版本历史：

| version | 变化 | 兼容性 |
|---|---|---|
| 1 | 实体、变换、父子、网格、材质 | — |
| 2 | 新增可选 `voxelWorlds`（seed + 差分） | 旧文件缺省为空 |
| 3 | 每个实体新增可选 `components`（元数据注册实体组件；`native.*` 不写入） | 旧文件缺省为空；未知组件/属性跳过 |
| 4 | 顶层新增 `sceneComponents`（当前 12 个 `scene.*` 场景功能组件，`scope=Scene`） | 旧文件缺字段时按当前 `EngineSettings` 重建；空数组表示全部移除/关闭 |

`terrain` / `terrainWorld` / `character` / `components` / `sceneComponents` 都是**可选字段**，所以旧场景
不需要迁移；读取端不按版本号拒绝，只有写入端跟 `kCurrentVersion`（当前 4）升版本。
`sceneComponents` 与实体 `components` 分开：前者只接受 `scope=Scene` 的组件，且只存在一份。

## 预制体文件（`.prefab.json`）

预制体复用同一个 v4 `SceneDescription`，不引入新版本号，只是**约束文件内容**：

- 只保存一棵子树；根实体 `parent: -1`，其余下标都指向子树内部。
- 变换是**相对父节点**的局部值，与场景文件一致。
- 不写顶层 `voxelWorlds`；`sceneComponents` 固定为空数组，不携带场景功能；保存时拒绝体素世界、地形/流送地形、相机、天空盒、Grid 与体素 chunk。
- 仍是普通场景 JSON，可以直接当场景打开查看；编辑器里的“实例化预制体”会把它追加到当前场景，而不是替换场景。

```json
{
  "version": 4,
  "entities": [
    { "name": "Root", "parent": -1, "mesh": "Models/debug_cube.obj" },
    { "name": "Spinner", "parent": 0, "translation": [1, 2, 0],
      "components": [ { "type": "effects.spin", "properties": [
        { "name": "degreesPerSecond", "type": "float", "value": 90 } ] } ] }
  ]
}
```

读写与校验在 `src/Editor/Prefab.*`，实例化在 `Scene::instantiatePrefab()`；
使用说明与限制见 [prefabs](prefabs.md)。

## 地形字段（可选）

带 `TerrainComponent` 的实体在实体 JSON 里多一个 `terrain` 对象：

```json
"terrain": {
  "version": 5,
  "seed": 12345,
  "resolutionX": 257,
  "resolutionZ": 257,
  "sizeX": 100,
  "sizeZ": 100,
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
  "chunkGridX": 2,
  "chunkGridZ": 2,
  "lodLevelCount": 3,
  "lodDistanceScale": 1.0,
  "surfaceStyle": 0
}
```

这是**可选字段**，旧场景缺字段照常加载；场景文件当前 `version` 为 4（v4 起有顶层 `sceneComponents`）。
`terrain.version` 是地形子对象自己的版本（stage 7 版为 5；旧 v1~v4 缺省字段由
TerrainConfig 默认值补齐）。`sampleOriginX/Z` 是连续噪声域的中心，
`localColorRange` 控制顶点色是否按局部高度范围拉伸；流送 chunk 使用绝对 [0,1]。
`skirtsEnabled` 控制是否追加向下裙边；B-02 起关闭时依靠全分辨率边界 + 过渡环带缝合。
加载时 `Scene::buildFromDescription()` 走 `buildTerrainModel()` 重新生成网格；
不改写 `mesh` 路径，也不分配材质槽位。
`materialDetailStrength`、`uvTiling`、`surfaceStyle` 与 `lodDistanceScale`
只影响着色或运行时 LOD 选择；`chunkGridX/Z`、`lodLevelCount`、`sampleOriginX/Z`、
`skirtsEnabled` 属于几何参数，修改后需要 Generate。地形不写顶点、索引、包围盒、GPU 句柄或纹理数据。
外部 `Resources/Texture/Terrain/` 覆盖是全局设置，同样不进入场景 JSON。

流送 manager 在实体 JSON 里使用可选 `terrainWorld`：

```json
"terrainWorld": {
  "chunkConfig": { "version": 5, "resolutionX": 65, "sizeX": 80,
                   "localColorRange": false, "skirtsEnabled": false },
  "loadRadius": 1,
  "unloadPadding": 1,
  "maxLoadsPerFrame": 2
}
```

`terrainWorld` 只保存 manager 模板和流送预算；`TerrainChunkComponent` 实体是
运行期派生数据，不逐项写入。加载后由 `Scene::updateTerrainStreaming()` 围绕相机
重建 chunk 集。详细规则见 [terrain](../30-systems/terrain.md) §6.1。

## 扩展组件（v3+）

`components` 是元数据注册组件的通用容器，格式由组件描述表决定，序列化器不针对
单个类型写代码：

- `type`：注册 id（如 `effects.spin`）。读取时只做语法还原；**是否注册、scope 是否匹配由
  `Scene::buildFromDescription()` 判断**，未知 id 或放错容器（如 `scene.*` 写进实体）安全跳过，
  不影响场景其余部分。
- `properties`：属性数组，每个属性是 `name` / `type` / `value`。`name` 必须是
  描述符里的稳定名；`type` 只接受 `bool` / `int` / `float` / `vec3` / `color` / `string`。
- 值编码：Bool/Int/Float 是标量，Vec3 是 3 元数组，Color 是 4 元数组，String 是字符串。
- 坏属性只跳过自己；坏组件（缺 `type`）跳过整个组件。
- `label` 不写盘，读取后由注册表重新生成；`ComponentRecord::equivalentTo()` 因此只比较
  `type` + 属性名 + 值。

`roundTrip` 自检给 Root 实体挂一个覆盖全部 6 种属性类型的 `test.sixTypes` 记录，逐实体比较组件数量
与 `equivalentTo()`；同时比较顶层 `sceneComponents` 的数量与内容。删除撤销、复制实体也会带走
实体 record，见 [commands](commands.md)。

### 场景级组件与 `sceneComponents`（v4，F5-06；F6-05~F6-07 扩到 12 个）

12 个渲染功能组件（environment/shadows/bloom/ssao/ssr/motionBlur/taa/water/fog/clouds/sun/vegetation）挂在
`Scene` 的隐藏设置实体上，存盘写在**顶层**：

```json
"sceneComponents": [
  { "type": "scene.bloom", "properties": [
    { "name": "enabled", "type": "bool", "value": true },
    { "name": "threshold", "type": "float", "value": 2.2 }
  ] }
]
```

- 容器复用 `ComponentRecord` 格式，`type` / `properties` 规则与实体组件相同。
- `SceneDescription::sceneComponentsAuthored` 区分“旧文件没有字段”和“v4 空数组”：前者按当前
  `EngineSettings` 调 `resetSceneComponentsFromSettings()` 重建 12 个组件；后者保留空容器，
  等于用户把功能全部移除。
- `captureDescription()` 总是把 `sceneComponentsAuthored` 置 true；`toJson` 总是写数组（可为空）。
- 加载时逐条校验 `scope=Scene`，`scene.*` 写进实体 `components` 会被警告并跳过；
  实体组件写进 `sceneComponents` 同样拒绝。
- 预制体只捕获子树实体，顶层数组为空，不把当前场景的阴影/雾等设置带进预制体。
- 局部体积雾是**实体组件** `effects.fogVolume`（带 Transform 的普通实体），走实体 `components` 数组，不进 `sceneComponents`；一个场景可以有多个雾盒。组件的 `center`/`size`/`rotation` 定义局部盒体，再乘实体 WorldTransform；旧场景缺这三个字段时按默认值 (0,0,0)/(20,8,20)/(0,0,0) 补齐，见 [fog-volume](../30-systems/fog-volume.md)。

### 原生组件与 `components` 去重（v3+，F5-04）

`src/Scene/NativeComponents.cpp` 注册的 `native.*` 描述符只用于 Inspector 元数据与增删校验，
不进入实体 `components`：

- 描述符带 `serialized=false`；`Scene::captureComponentRecords()` 跳过它们，
  `SceneSerializer` 不会把 Character/Render Tag 等写成通用组件。
- 原生状态仍走 `EntityDescription` 的专用字段：`character`、`skybox/grid/isTerrain`、
  `terrain`、`terrainWorld`、`camera`、`voxelWorlds`、mesh/material 等。
- 加载时"文件里没有的已注册组件就移除"的循环只处理 `serialized` 描述符，
  所以复用相机或实体上的 Character/Render Tag 不会被误删；文件里伪造的 `native.*`
  记录会打印警告并跳过。
- 因此同一个 Character 不会既写 `character` 又写 `components`；Render Tag 也不会重复。
  删除/撤销对 Character 的原生快照在 `DeleteEntityCommand` 单独维护。
- `scene.*` 只进顶层 `sceneComponents`，不会挂在实体上；原生组件的 `components` 去重规则
  与场景组件容器无关。

## 路径处理

存盘写**相对 `Resources/` 的路径**，读回时拼回绝对路径。

原因：绝对路径换机器就失效。而引擎本来就用 `AssetPaths.h` 在 CMake 配置阶段
固化了资源根目录，所以相对路径是可解析的。

## 加载的原子性

`SceneSerializer::loadFromFile()` 先把整个 JSON 解析进 `SceneDescription`，
**全部成功之后**才动真实场景。中途失败不会留下"半个场景"。

## 加一个要存盘的组件

有两条路，优先第一条：

**A. 元数据注册组件（推荐，不改序列化器）**

1. 写组件结构体并在 `ComponentRegistry` 里 `registerComponent`（参考 `FeatureComponents.cpp`）；
   实体功能用 `scope=Entity`，场景功能用 `scope=Scene`（参考 `SceneComponents.cpp`）
2. 需要每帧行为时在 `SystemScheduler` 注册系统；场景组件改参数后需同步 `EngineSettings`
3. 需要默认值/属性 UI 时把属性加进描述符
4. 不需要改 `SceneSerializer`、`SceneDescription`、Commands 或 Inspector（实体组件进 `components`，
   场景组件进顶层 `sceneComponents`）
5. 在 `Scene::selfTest()` / `EditorLayer::selfTest()` 的 roundTrip 用例里加断言

只有**已注册且 `serialized=true`** 的类型会进 `captureDescription()`；未注册组件和
`native.*` 会在存盘时被过滤。原生组件的专用字段仍按 B 路径维护。

**B. 内置专用组件（有专用渲染/物理路径）**

1. 在 `SceneDescription.h` 的 `EntityDescription` 加字段
2. `Scene::captureDescription()` 里填
3. `Scene::buildFromDescription()` 里恢复
4. `SceneSerializer` 的 `toJson` / `fromJson` 各加一行
5. 升级 `version`（若新字段是可选的、旧文件缺省语义明确，可不升，例如 `terrain`）
6. 在 `EditorLayer::selfTest()` 的 roundTrip 用例里加断言
