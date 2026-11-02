---
id: editor/serialization
title: 场景序列化
type: system
scope: 场景描述模型、文件格式、以及"什么该存什么不该存"
read-when: 改 *.scene.json 格式、加组件要存盘、或加载后场景不对
size: ~150 行
updated: 2026-11-01
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
- 将来加 Prefab、场景 diff、整场景撤销，都以它为单位

## 什么存，什么不存

| 存 | 不存 | 为什么 |
|---|---|---|
| 实体名、变换、父子关系 | 实体 ID | ID 是运行期分配的，跨进程无意义 |
| 网格**文件路径** | `VkBuffer` / `VmaAllocation` | GPU 句柄不能序列化 |
| 材质名字 + PBR 参数 + 贴图路径 | 材质**槽位下标** | 槽位是 `DescriptorManager` 运行期分发的 |
| 渲染标签（skybox/grid/terrain） | `SpatialIndex` / `indexedBounds` | 可增量刷新的派生数据，不存盘 |
| 地形参数对象 | 地形顶点/索引/GPU 句柄/纹理 | 加载时按相同 seed/算法重新生成；16 张地形纹理由启动代码确定性生成 |

父子关系用**数组下标**表示（`parent: 2`），不用实体 ID。
加载时按顺序重建即可。

## 文件格式（version 1）

```json
{
  "version": 1,
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
      }
    }
  ]
}
```

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

这是**可选字段**，所以场景文件 `version` 仍为 1，旧场景不缺字段照常加载。
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

## 路径处理

存盘写**相对 `Resources/` 的路径**，读回时拼回绝对路径。

原因：绝对路径换机器就失效。而引擎本来就用 `AssetPaths.h` 在 CMake 配置阶段
固化了资源根目录，所以相对路径是可解析的。

## 加载的原子性

`SceneSerializer::loadFromFile()` 先把整个 JSON 解析进 `SceneDescription`，
**全部成功之后**才动真实场景。中途失败不会留下"半个场景"。

## 加一个要存盘的组件

1. 在 `SceneDescription.h` 的 `EntityDescription` 加字段
2. `Scene::captureDescription()` 里填
3. `Scene::buildFromDescription()` 里恢复
4. `SceneSerializer` 的 `toJson` / `fromJson` 各加一行
5. 升级 `version`（若新字段是可选的、旧文件缺省语义明确，可保持版本不变，例如 `terrain`）
6. 在 `EditorLayer::selfTest()` 的 roundTrip 用例里加断言
