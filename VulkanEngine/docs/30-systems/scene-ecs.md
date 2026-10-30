---
id: systems/scene-ecs
title: 场景、ECS、Transform 层级与空间索引
type: system
scope: src/Scene/Scene.h, src/Scene/Scene.cpp, src/Scene/Components.h, src/Scene/SpatialIndex.h, src/Scene/GLTFLoader.cpp, src/Scene/Model.cpp
read-when: 修改场景结构、增删组件、改变换传播或剔除逻辑
size: ~230 行
updated: 2026-10-30
---

# 场景、ECS、Transform 层级与空间索引

## 1. 组件

`src/Scene/Components.h` 定义了全部组件：

| 组件 | 内容 |
|---|---|
| `TransformComponent` | local 的 `position` / `rotation` / `scale` |
| `WorldTransformComponent` | 计算出的 `worldMatrix` / `normalMatrix` |
| `HierarchyComponent` | `parent` + `children` |
| `MeshComponent` | 指向 `Model` 的子网格 |
| `MaterialComponent` | 材质槽位 |
| `RenderTagComponent` | `isSkybox` / `isGrid` |
| `BoundsComponent` / `WorldBoundsComponent` | 局部 / 世界 AABB |
| `CameraComponent` | 相机标签 |

## 2. Transform 层级

`Scene::setParent()` 维护 children，**拒绝自环与循环**。

每帧按固定顺序计算世界变换：

```
World = T * Ry * Rx * Rz * S
```

然后用 `World` 的左上 3x3 求逆再转置得到 `normalMatrix`。
顺序固定（`T*Ry*Rx*Rz*S`）是刻意的——不同顺序会得到不同的旋转结果，
而配置里存的是欧拉角，必须与渲染一致。

## 3. 空间索引与剔除

```
世界 AABB 插入均匀网格（broad phase）
   -> 查询：与相机视锥 AABB 相交的网格单元
   -> 精确：frustum-AABB 测试
   -> 结果写入 Scene::visibleEntities
```

`GeometrySystem` 只遍历 `visibleEntities`，不再逐个实体做剔除判断。

**注意**：网格**每帧重建**，没有 dirty / 增量更新。实体数上千后这是 CPU 热点，
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

## 6. 自检

```cpp
Scene::selfTest();
```

覆盖：父子/孙节点的世界变换传播、空间网格查询。
输出标记：`[Scene] ECS/spatial selfTest PASS | hierarchy=T*Ry*Rx*Rz*S | grid cells=9`。

## 7. 改动注意

- 加组件要在 `Scene::selfTest()` 里补相应校验。
- 改变换顺序要同步 `CameraController` 与配置里存的欧拉角约定。
- 改剔除逻辑后确认 Sponza `draw calls` 仍是 104（少了说明误剔除）。
