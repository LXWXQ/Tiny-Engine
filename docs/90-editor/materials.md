---
id: editor/materials
title: 占位材质的棋盘格着色
type: system
scope: 没有有效材质槽位 / 没有 albedo 贴图时的程序化棋盘格
read-when: 新建物体显示纯白、棋盘格发白、改 G-Buffer 着色或加材质相关逻辑
size: ~160 行
updated: 2026-11-01
---

# 占位材质的棋盘格着色

## 问题

编辑器里新建的实体、以及 glTF 中没有贴图的子网格，如果直接采样"默认白纹理"，
会渲染成一团**纯白**：看不出形状、朝向、有没有在动。

## 两种触发条件

在 `mrt.frag` 里，棋盘格用于两种情况：

| 情况 | 判据 |
|---|---|
| 子网格没有可用的材质槽位 | `push.material.y == 0` |
| 有材质，但材质没有 albedo 贴图 | `push.albedoFallback != 0` |

`albedoFallback` **按子网格**填，两个来源取或：

```cpp
// 实体材质没有 albedo（编辑器新建的占位材质）
const bool entityAlbedoFallback = materialComp && materialComp->material &&
                                  !materialComp->material->albedoMap;
// glTF 子网格材质没有 baseColorTexture（由 GLTFLoader 写入 SubMesh）
push.albedoFallback = (subMesh.albedoFallback || entityAlbedoFallback) ? 1u : 0u;
```

这样"**一切没有纹理的网格**"都会走棋盘格：
编辑器创建的实体、没有材质的子网格、以及 glTF 内部没有贴图的材质。
Sponza 只在它的材质确实没有 albedo 贴图时才变格子，不会整片误判。

## 槽位来源只有一个（已修的 bug）

实体材质槽位归 `MaterialComponent::materialSetIndex` 所有：每个实体分配一个槽位，
`Scene::clearEntities()` 也按实体归还。`Material::descriptorSetIndex` 是**材质对象**
的字段，同名实体会共享同一个 `Material`，所以它不能代表"这个实体有没有槽位"。

原来的 `GeometrySystem` 却优先读 `material->descriptorSetIndex`。编辑器里通过
`createMeshEntity()` 创建的实体只写了组件字段，于是每帧都误判成
"没有有效材质槽位"：

```
[GeometrySystem] entity 2 的子网格没有有效材质槽位，退化为默认着色。
[GeometrySystem] entity 2 的子网格没有有效材质槽位，退化为默认着色。
... 每帧刷屏 ...
```

修法：

1. `GeometrySystem` 只认 `MaterialComponent::materialSetIndex`；
2. 分配仍然由 `createMeshEntity()` / `buildFromDescription()` 按实体做，
   不把槽位写进共享的 `Material`；
3. 提示本身改成**只提示一次**，并且走 `std::cout`。
   每帧写 `std::cerr` 既刷屏，又会破坏"stderr 0 字节"的回归判据。

## 棋盘格怎么画

有两条路径，由 CPU 根据模型的 UV 范围选择：

| 条件 | 路径 |
|---|---|
| UV 两个方向都覆盖约 `[0,1]`（平面、teapot） | UV 棋盘：8 格，按实际 UV 子区间归一化 |
| UV 是图集子区间（Quad-Sphere 只占 0.05~0.43）或没有 UV（debug_cube） | 局部空间 **3D 棋盘** |

### 为什么不再用三平面

旧实现把三个 2D 投影按 `pow(abs(normal), 8)` 混合。立方体没问题，但球面在
45° 过渡带三个投影权重接近，黑白被混成大片灰色抹痕；UV 图集子区间强行拉伸
又会出现接缝和扭曲。3D 棋盘在物体内部是连续场，球面、多面体、没有 UV 的
网格都能得到均匀格子，没有选轴接缝。

```glsl
float checker3D(vec3 p)
{
    vec3 q = p + vec3(0.317, 0.619, 0.733); // 避免平面正好落在格子边界
    vec3 f = fract(q);
    vec3 toBoundary = min(f, vec3(1.0) - f);
    vec3 w = (abs(dFdx(q)) + abs(dFdy(q))) * 0.5 + 1e-5;
    vec3 inside = smoothstep(vec3(0.0), w, toBoundary);
    float parity = mod(floor(q.x) + floor(q.y) + floor(q.z), 2.0);
    return mix(0.5, parity, min(min(inside.x, inside.y), inside.z));
}
```

- 3D 格子周期为 1，CPU 用 `checkerScale = 6 / maxExtent`，最大尺寸约 6 格；
  不同模型局部尺寸差上百倍（`debug_cube` 是 1，`sphere.obj` 是 2000），
  写死常数会一边糊成灰、一边只有一格。
- 加一个非整数偏移，避免某个平面正好落在 `floor` 的边界上导致整面变成灰色。
- 边界用屏幕导数做 `smoothstep` 抗锯齿，远处不会变成噪点。

### UV 路径

UV 完整时按 8 格采样，并减去模型 UV 最小值再除以范围，保证图集子区间也能铺满。
u 在 1→0 处环绕会让 `dFdx` 跳约 8，接缝那一列会被糊成灰色；把跨接缝的导数
折回 `[-0.5,0.5]` 再滤波即可：

```glsl
dpdx.x -= round(dpdx.x);
dpdy.x -= round(dpdy.x);
```

### 法线

棋盘格本身**不需要法线**；没有法线的模型（`debug_cube.obj` 没有 `vn`）顶点法线
是零，`mrt.frag` 的 `outNormal` 仍用局部坐标的屏幕导数重建几何法线。
`Model.cpp` 与 `GLTFLoader.cpp` 都不再把缺失法线伪造成 `(0,1,0)`。

### 数据从 CPU 传来

`SimplePushConstantData` 新增 `checkerScale` 与 `checkerUvTransform`：

- `checkerScale` 由 `GeometrySystem` 按实体局部包围盒算；
- `checkerUvTransform = (1/uvExtent, uvMin)`，没有完整 UV 时置零，着色器据此选 3D 路径。

`glm::vec4` 在 C++ 里默认只有 4 字节对齐，而 GLSL std430 的 `vec4` 要求 16 字节，
所以字段必须写 `alignas(16)`，否则着色器读到的偏移会错 8 字节，表现为材质全乱。
`FrameInfo.h` 里有 `offsetof` 静态断言钉住偏移和总大小（阶段 6 纹理版 208 字节：`terrainMode` + minY/range/detail/uvTiling + 3 个 padding）。

## 为什么必须 clamp

`checkerFiltered` 的盒式滤波是对方波的近似。滤波窗口比格子小时，表达式会**外插**
到 `[-1.5, 2.5]`，而不是 `[0, 1]`。负的 albedo 写进 G-Buffer 后，
`PresentPass` 的 ACES 对负输入会把分子分母同时变负、再 clamp 到 **白色**：

```
现象：棋盘格整片发白，只剩格子缝是暗的
原因：mix(0.03, 0.85, -1.5) = 负值 -> ACES 负输入 -> 白色
修法：pattern = clamp(pattern, 0.0, 1.0)
```

3D 路径的返回值本身就是 `[0,1]`；UV 路径的 `checkerFiltered` 保留 clamp。

这是【坑 29】。

## PBR 参数也走占位

占位材质没有 metallic/roughness 贴图，默认白纹理会给出 `metallic = 1`，
棋盘格会变成金属反射，黑白对比被环境光吃掉。fallback 分支直接把
`outPBR` 写成"金属度 0、粗糙度 1"，保证是清晰的粗糙电介质。

## 为什么用程序化而不是一张贴图

- 不用额外纹理资源、不用管采样器与 sRGB；
- 没有 mip 相关的接缝问题；
- 改格子密度只要改一个常数。

代价是它不受材质系统控制 —— 这是**调试用的占位表现**，不是正式材质特性。
## 地形分支不是棋盘格

`mrt.frag` 的 `push.terrainMode != 0` 分支发生在材质下标判断之前。
地形没有 `MaterialComponent`，但会采样启动时创建的 Set2 四层 albedo/normal/heightRoughness 纹理，并计算自己的高度坡度权重与 Surface Style；
因此它不会落入没有材质槽位时的棋盘格路径，也不会触发对应的单次警告。