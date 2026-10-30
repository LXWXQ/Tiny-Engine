---
id: editor/serialization
title: 场景序列化
type: system
scope: 场景描述模型、文件格式、以及"什么该存什么不该存"
read-when: 改 *.scene.json 格式、加组件要存盘、或加载后场景不对
size: medium
updated: 2026-10-30
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
| 渲染标签（skybox/grid） | `SpatialIndex` | 每帧重建的派生数据 |

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
5. 升级 `version`
6. 在 `EditorLayer::selfTest()` 的 roundTrip 用例里加断言
