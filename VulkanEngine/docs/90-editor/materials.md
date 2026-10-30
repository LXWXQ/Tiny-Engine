---
id: editor/materials
title: 占位材质的棋盘格着色
type: system
scope: 没有 albedo 贴图时为什么画棋盘格、怎么判断"没有 UV"
read-when: 新建物体显示成一团白 / 改 G-Buffer 着色 / 加材质相关逻辑
size: small
updated: 2026-10-30
---

# 占位材质的棋盘格着色

## 问题

编辑器里新建的实体用的是占位材质（没有任何贴图）。采样"默认白纹理"后
渲染成一团**纯白**，看不出形状、朝向、有没有在动。

## 做法

在 `mrt.frag` 里加程序化棋盘格，两种情况使用：

| 情况 | 判据 |
|---|---|
| 实体完全没材质 | `push.material.y == 0` |
| 有材质但没有 albedo 贴图 | `push.albedoFallback != 0` |

`albedoFallback` 由 `GeometrySystem` 填：

```cpp
push.albedoFallback = (materialComp && materialComp->material &&
                       !materialComp->material->albedoMap) ? 1u : 0u;
```

**注意**：只有"实体自己带材质、但那个材质没有 albedo 贴图"才算 fallback。
glTF 实体的材质在**子网格槽位**里（`materialComp` 为空），
误判成 fallback 会让 Sponza 整片变成棋盘格。

## UV 退化的回退

有些模型（`debug_cube.obj`）**没有有效 UV**（全是 0）。这时
`floor(uv * 8.0)` 恒定，棋盘格退化成一整片纯色 —— 等于没效果。

判据是 UV 的屏幕空间导数：

```glsl
float uvMotion = length(dFdx(uv)) + length(dFdy(uv));
if (uvMotion < 1e-7) {
    // 没有 UV：按法线主轴取世界坐标的两个分量（三平面映射的单轴版）
}
```

这样任何网格都能显示出格子。

## 为什么用程序化而不是一张贴图

- 不用额外纹理资源、不用管采样器与 sRGB；
- 没有 mip 相关的接缝问题；
- 改格子密度只要改一个常数。

代价是它不受材质系统控制 —— 这是**调试用的占位表现**，不是正式材质特性。
