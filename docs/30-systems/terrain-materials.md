---
id: systems/terrain-materials
title: 地形材质与 Set2
type: system
scope: src/Render/TerrainMaterial.*, DescriptorManager 的 terrain sets, shaders/mrt.frag 的 terrainMode, Resources/Texture/Terrain/
read-when: 改地形 Set2 纹理布局、材质集加载/回退、四层 PBR 混合、Surface Style 或材质切换统计
size: ~75 行
updated: 2026-11-05
---

# 地形材质与 Set2

## 1. 这篇解决什么

地形不是普通 PBR 材质：它用独立描述符集（Set2）承载四层地表纹理，在片元着色器里
按高度/坡度混合。这篇说明 Set2 的固定布局、四套材质集怎么加载和回退、切换材质时
重新绑定的代价，以及着色器地形分支的混合顺序。网格与 LOD 见
[terrain](terrain.md)。

## 2. Set2 布局

地形材质使用独立的 **set 2**，共 12 个固定 `sampler2D`：

| binding | 内容 |
|---|---|
| 0~3 | 沙地 / 草地 / 岩石 / 雪 albedo |
| 4~7 | 沙地 / 草地 / 岩石 / 雪 tangent-space normal |
| 8~11 | 沙地 / 草地 / 岩石 / 雪 packed height(R) + roughness(G) |

`DescriptorManager::createTerrainLayout()` 实际声明 16 个 binding：0~11 被
`shaders/mrt.frag` 使用，12~15 预留。16 个 binding 全部带
`UPDATE_AFTER_BIND | PARTIALLY_BOUND`，layout 也带 `UPDATE_AFTER_BIND_POOL` ——
编辑器切材质集时会在飞行帧中重写 set，缺任一标志就是
`VUID-vkUpdateDescriptorSets-None-03047`。

## 3. 四套材质集与加载 / 回退

`TerrainMaterial` 管理最多 4 个材质 set（`kMaxMaterialSets=4`），每个 set 持有
上述 12 张 GPU 纹理（4 albedo + 4 normal + 4 packed height/roughness）；外部
PNG 是每套 16 张源文件。set 0 在启动时从 `Resources/Texture/Terrain/` 加载；
若该目录 16 张 PNG 不全，则生成 4 层 256² 程序化纹理。set 1~3 按需从
`Resources/Texture/Terrain/setN/` 加载，同样要求 16 张齐全，缺任意一张就整套
回退到程序化纹理，避免层间 UV/风格错配。albedo 按 sRGB 加载；
normal/height/roughness 按线性 UNORM 加载；height 与 roughness 在 CPU 侧打包，
因此同一层两张图尺寸必须一致。文件名约定见 `README.txt`。

四层纹理都以 `tileableMips=true` 上传：CPU 用 2x2 box filter 按纹理尺寸取模生成
完整 mip 链，再整链 `vkCmdCopyBufferToImage`；不再逐级 `vkCmdBlitImage`。blit 在
图像边缘 clamp，会让每个平铺块的 mip 边缘带上不同偏色，远看就是一块块拼布。

## 4. 绑定与切换

`TerrainConfig::materialSetIndex`（0..3）随地形配置写入场景 JSON；仅切换材质
不改变 `geometryEquals()`，不会重建网格。渲染时 Set2 按相邻地形实体的
`materialSetIndex` 变化重新绑定；每帧统计 `terrainMaterialBatches`/
`terrainMaterialSwitches`/`terrainMaterialFallbacks`，非法索引回退 set 0。

## 5. 片元着色器的地形分支

片元着色器在真实几何法线写入后进入地形分支：

- 用地形配置的全局 Y 范围（`baseHeight` / `heightScale`）还原高度（0..1），
  用 `1 - normal.y` 还原坡度；不要用每个 chunk 的 `BoundsComponent` 局部范围：
  流送 chunk 覆盖的高度切片不同，按局部范围归一化会让同一世界高度在 chunk
  边界两侧得到不同层权重，出现矩形沙/雪色块；
- 按高度/坡度得到沙地、草地、岩石、雪基础权重，再应用 `surfaceStyle`
  的权重偏置；
- 以世界 XZ 采样四层 height，对层权重乘 0.90~1.10 的高度调制并重新归一化
  （幅度过大时会在草地上形成整块深色/浅色斑）；
- albedo 用第二尺度样本和 patch mask 混合，patch mask 只保留 35% 对比度，
  抑制单张纹理的明显平铺，同时收敛低频大色块；
- 对四层 normal 解码后按权重混合，投影到由几何法线构造的 XZ 切线基；
  屏幕空间 footprint 控制法线细节淡出；
- 四层 roughness 按权重混合，再叠加 Style 偏置和低频噪声；
- 用低频 fBm 做宏观亮度变化（±6%，只保留轻微起伏），metallic 固定为 0；
- `outNormal.w = 0` 保持 SSAO policy mask，地形继续跳过 SSAO。

## 6. Surface Style 与"不改几何"的字段

Surface Style 是 shader-only 变体：Arid 提高沙地、压低雪和草；Alpine 提高
岩石/雪并加强法线；Lush 提高草、压低岩石和沙。切换在下一帧生效，无需 Generate。

`uvTiling`、`materialDetailStrength`、`surfaceStyle` 和 `lodDistanceScale` 不改变
网格；`geometryEquals()` 排除这四个字段。`lodLevelCount` 和 `skirtsEnabled`
改变预生成的网格内容，属于几何参数，需要 Generate。
