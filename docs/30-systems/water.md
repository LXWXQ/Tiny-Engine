---
id: systems/water
title: 水面
type: system
scope: src/Render/WaterPass.{h,cpp}, src/Render/ReflectionPass.{h,cpp}, shaders/water.vert, shaders/water.frag
read-when: 改水面外观/波浪/反射、排查水面不出现或被地形裁掉
size: ~205 行
updated: 2026-11-05
---

# 水面

水面是 **Lighting 之后、TAA 之前**的前向混合通道（`WaterPass`）：读已经着色好的
线性 HDR 与 G-Buffer 深度，把水色、环境反射、泡沫按 alpha 叠上去。它不写 G-Buffer，
也不写深度。片元阶段按像素足迹过滤波浪，避免远处条带/闪烁；同一套距离雾也作用于
地形（`lighting.frag`）和玻璃（`glass.frag`）。开启平面反射时，水面从 `ReflectionPass`
的半分辨率反射色采样，而不是只采样环境图。

## 1. 为什么是独立 Pass

- 透明表面写进 G-Buffer 只会保留"最前一个表面"，水后面的地形会整块消失；
- 延迟光照阶段没有"按像素重新混合多层表面"的位置，Fresnel / 反射需要单独一遍；
- 放在 Lighting 后，水能拿到完整的直接光 + IBL 结果；放在 TAA 前，水边缘也会被
  时域抗锯齿；放在 Bloom 前，太阳高光会参与辉光。

管线里的顺序是
`... -> Lighting -> PlanarReflection -> Glass -> Water -> TAA -> Bloom -> Present`。完整顺序见
[pass-order](../20-pipeline/pass-order.md)。

## 2. 相机中心径向网格

水面不是一个绑定到地形上的实体，而是一张**始终跟随相机**的网格：

- 1 个中心顶点 + 160 圈、每圈 192 段，共 **30721 顶点 / 183744 索引**；
- 第 i 圈半径 `r_i = 1.2*i + 0.15*i^2`，近处约 1.35m 一格，最远 `kFarRadius = 4032m`；
- 顶点只存局部 XZ，顶点着色器里再加 `cameraPos.xz`；相机移动时网格自动铺满周围，
  不需要每帧重建顶点缓冲；
- 顶点缓冲和索引缓冲都是 host-visible + mapped，`init()` 时上传一次，之后不再写。

半径二次增长是刻意的：近处格子密，顶点位移看得清；远处格子疏，省顶点。水面一直铺到
地平线，天空在 G-Buffer 里的距离约 10000m，会被当成"没有底"的深海。

## 3. 波浪

顶点只做 4 个方向波的高度位移，像素阶段解析求梯度得到法线。

| 方向 (x,z) | 波长 (m) | 振幅 |
|---|---|---|
| (0.9239, 0.3827) | 23.0 | 0.180 |
| (-0.5736, 0.8192) | 14.0 | 0.100 |
| (0.7071, -0.7071) | 9.0 | 0.055 |
| (0.2588, 0.9659) | 5.5 | 0.025 |

每个波：

```
k     = 2*pi / wavelength
omega = sqrt(9.81 * k) * 0.65
phase = dot(dir, worldXZ) * k - time * omega + i * 1.7
h    += amplitude * sin(phase)
```

像素阶段再加 3 个高频 ripple（波长 1.7 / 1.1 / 0.75m，振幅 0.010 / 0.006 / 0.004），
它们只贡献法线细节，不做顶点位移。

**footprint 过滤**：远处/掠射角下，一个像素覆盖的世界 XZ 范围会迅速变大，解析法线
在相邻像素间跳相位，形成条带和闪烁。着色器用 `dFdx(p)` / `dFdy(p)` 得到像素足迹，
对每个波判断相邻像素的相位跨度；超过约 1 rad 就淡出该波：

```
footprint = |dot(dir, dFdx(p))| + |dot(dir, dFdy(p))|
resolve   = 1 - smoothstep(0.5, 1.5, k * footprint)
grad     += dir * (amplitude * strength) * k * cos(phase) * resolve
variance += 0.5 * ((amplitude * strength) * k)^2 * (1 - resolve^2)
```

淡出的斜率能量不会丢，而是累加成 `slopeVariance`，下一节转成粗糙度。ripple 仍保留
距相机 60..220m 的距离淡出，与 footprint 过滤相乘。

法线：

```
gradient = filteredBigWaveGradient + filteredRippleGradient
N = normalize(vec3(-gradient.x, 1.0, -gradient.y))
```

## 4. 着色

- **Fresnel**：`0.02 + 0.98 * pow(1 - NdotV, 5)`；从水下往上看时翻转法线。
- **像素足迹 → 粗糙度**：把上一步的 `bigSlopeVariance + rippleSlopeVariance` 转成
  `roughness = clamp(sqrt(variance) * 7.0 + distanceRoughness, 0.02, 0.65)`；
  `distanceRoughness = smoothstep(120, 3000, distanceToCamera) * 0.30`。
  `roughNorm = smoothstep(0.03, 0.55, roughness)` 控制反射模糊与高光展宽。
- **环境反射**：直接采样 Set0 binding 1 的等距柱状 HDR `environmentMap`，用
  `textureGrad` 并在 ±PI 接缝处折回导数，避免接缝退到最低 mip 出现亮线；导数再乘
  `1 + roughness * 12`，让远处未解析的波变成略模糊的反射，而不是随机镜面点。
- **平面反射**：开启 `waterPlanarReflection` 时，Set0 binding12 绑定 `ReflectionPass`
  输出的 960x540 反射色，片元按 `screenUV + N.xz * (0.0015 + roughness * 0.006)`
  扰动后采样，替代环境图反射；关闭时 binding12 回退到环境图。反射通道用镜像相机
  重画不透明几何并做第二套延迟光照，水面以下几何被 `clipY` 裁掉；玻璃、水面和
  调试网格不进入倒影。半分辨率结果直接按屏幕 UV 上采样，边缘比全分辨率软。
- **厚度吸收**：读 G-Buffer `inPosition`（不透明场景的世界坐标），沿视线算
  `thickness = sceneDistance - waterDistance`；`depthFactor = 1 - exp(-thickness * 0.22)`，
  水薄处偏浅、厚处偏深。天空的 10000m 让远处自动变成深海。
- **水体颜色**：`mix(shallow(0.045,0.150,0.160), deep(0.008,0.035,0.055), depthFactor)`；
  再乘 `0.55 + 0.45 * max(dot(N, L), 0)` 接方向光。
- **太阳高光**：高光指数从近处的 900 按 `roughNorm` 展宽到 24，峰值从 3.0 降到 0.55；
  宽层从 120 展宽到 8、强度从 0.10 提到 0.18。这是 specular AA：远处高光变宽变暗，
  不会在一条线上闪。
- **距离雾**：在泡沫之后、alpha 之前，用真实视线距离
  `waterDistance` 做指数雾：
  `fogAmount = 1 - exp(-fogDensity * max(waterDistance - fogStart, 0))`。
  雾色取环境图地平线方向 `normalize(vec3(-V.x, 0.035, -V.z))` 的 LOD3，和天空同源，
  远水与地平线无缝淡出而不是一条硬切边。`lighting.frag` 对不透明地形做同一套雾
  （天空提前返回、不参与）；`glass.frag` 只淡出玻璃颜色，alpha 不变。
- **岸边泡沫**：`shore = 1 - smoothstep(0, 1.6, thickness)` 加波峰项，再乘两个交叉正弦
  噪声；泡沫同时抬高 alpha。
- **混合**：`alpha = mix(0.18, 0.92, depthFactor) + fresnel * 0.55`，上限 0.985；
  管线用 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`，alpha 通道写 1。

第一版**不做折射**：RenderGraph 没有 image copy，折射需要先把 `hdrScene` 复制成可采样
纹理。也不做 SSR，反射只来自环境图。

## 5. Pass 合同

```
writeColor(hdrScene[frame], LOAD)
writeDepth(gbDepth[frame], LOAD)
blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA
depthTest: on, depthWrite: off, compare: LESS_OR_EQUAL
cullMode: NONE
```

- 只绑 Set0 全局描述符，不需要材质 Set；binding12 是反射纹理，平面反射关闭时
  由 `Application` 回退绑定环境图；
- push constant 48 字节：`params=(time, level, waveStrength, waveSpeed)`、
  `params2=(farRadius, foamStrength, fogStart, fogDensity)`、
  `params3=(planarReflection, 0, 0, 0)`；雾关闭时密度写 0；
- `GlobalUbo` 尾部新增 `fogParams=(fogStart, fogDensity, 0, enabled)`，光照/水面/玻璃共用；
  `lighting.frag`、`glass.frag` 的参数分别来自 UBO 与 32 字节的 glass push constant；
- `timeSeconds` 由 Pass 自己按 `min(max(frameTime, 0), 0.1)` 累加，暂停或断点后不会跳变；
- 每次 draw 累加 telemetry 的 `drawCalls / triangles / vertices / descriptorBinds`。

## 6. 启用条件与开关

水面只在**有地面**时注册：registry 里至少有一个 `TerrainComponent` 或
`VoxelChunkComponent`。Sponza / 空场景不会出现一张 2m 高的全球水面。

| 入口 | 行为 |
|---|---|
| 编辑器 Render 面板 / 运行时 ImGui | `Water` 分组：Enabled、Level、Wave Strength、Wave Speed、Planar Reflection；另有 `距离雾` / `Atmosphere / Fog` 分组：Enabled、Start、Density |
| `--no-water` | 关闭水面；Pass 不注册，打印一次 `[Water] disabled` |
| `config/engine.json` | `water: { enabled, level, waveStrength, waveSpeed, planarReflection }`、`fog: { enabled, start, density }` |

默认值：`waterEnabled=true`、`waterLevel=2.0`、`waterWaveStrength=1.0`、
`waterWaveSpeed=1.0`、`waterPlanarReflection=true`；`fogEnabled=true`、`fogStart=80m`、`fogDensity=0.00045`
（约 1km 处 34%、4km 处 83%）。雾是全局设置，地形/水面/玻璃共用；没有单独的
`--no-fog`，通过配置或 UI 关闭。

## 7. 可观测与回归

启动：

```
[Reflection] planar reflection initialized | extent=960x540
[Water] initialized | rings=160 segments=192 vertices=30721 indices=183744 radius=4032m
```

首帧绘制：

```
[Reflection] first draw | level=2 mirrorY=-6.8105 extent=960x540
[Water] first draw | level=2 waveStrength=1 waveSpeed=1 cameraY=46 planarReflection=1
```

平面反射关闭时没有 `[Reflection] first draw`，水面日志为 `planarReflection=0`。

回归用例：

| 名称 | 命令 | 断言 |
|---|---|---|
| `f3v-water` | `--selftest 20 --voxel-world 1` | `[Water] initialized \|`、`[Water] first draw \|`、`[Validation] errors=0 warnings=0` |
| `f3v-water-reflection` | `--selftest 20 --voxel-world 1` | `[Reflection] planar reflection initialized \|`、`[Reflection] first draw \|`、`planarReflection=1`、验证层 0/0 |
| `f3v-water-off` | `--selftest 20 --voxel-world 1 --no-water` | `[Water] disabled`、验证层 0/0 |

截图可以用 `--screenshot build/water_test.png --screenshot-frame 20` 目检水体颜色、
反射与岸边泡沫；把相机转向开阔水面、拉远到 1~4km，可对比 `fog.enabled` 开关前后的
地平线过渡与远处高光。

## 8. 已知边界

- 水面是水平面，不支持任意朝向的水体；
- 没有折射、没有 SSR、没有水下后处理；反射是镜像相机的半分辨率重画，不是屏幕空间
  追踪，屏幕外的物体不会出现在倒影里；
- 距离雾是均匀指数雾，没有高度衰减、体积光/太阳散射，也没有时间变化；雾色固定取
  环境图地平线 LOD3，天空本身不参与雾；
- footprint 过滤只修法线和高光，不改变几何；外圈网格段长约 132m，5.5m 波仍有几何
  欠采样，远水细节依赖法线过滤而不是真实位移；
- 没有交互（角色入水、涟漪、溅射），也没有流场/泡沫贴图；
- 只在有地形/体素块时启用，不能单独给任意场景加水；
- 平面反射只渲染不透明几何：玻璃、水面、调试网格不进入倒影，粒子/透明材质也不会；
- 反射用主相机的阴影级联，镜像视角下的阴影/SSAO 不精确；半分辨率且没有独立时域
  累积，快速移动相机时倒影边缘会偏软或有轻微抖动；
- TAA 只有相机重投影、没有运动向量，水面动画和物体自身运动不会被单独补偿；
- `--no-water` 时 Pass 与 PlanarReflection 都完全不注册，没有"着色器短路"路径；
  雾没有命令行开关，平面反射只有 UI/配置开关。

## 9. 关键文件

| 文件 | 职责 |
|---|---|
| `src/Render/WaterPass.h` | 接口、push constant、径向网格常量 |
| `src/Render/WaterPass.cpp` | 网格/缓冲、管线、Pass 注册、均匀量与统计 |
| `src/Render/ReflectionPass.{h,cpp}` | 镜像相机、半分辨率 G-Buffer/光照、反射纹理与开关 |
| `shaders/water.vert` | 径向网格跟随相机、4 个大波位移 |
| `shaders/water.frag` | footprint 波浪过滤、粗糙度反射、Fresnel、厚度吸收、泡沫、距离雾 |
| `shaders/lighting.frag` | 不透明几何的距离雾；天空提前返回 |
| `shaders/glass.frag` | 玻璃距离雾（只淡出颜色，alpha 不变） |
| `src/Core/FrameInfo.h` | `EngineSettings` 的 water/fog 字段、`GlobalUbo.fogParams` |
| `src/Core/Application.cpp` | 每帧上传 `fogParams`；开关变化时重建全局描述符，binding12 在反射纹理/环境图之间切换 |
| `src/Core/EngineConfig.cpp` | `water` / `fog` 节点读写 |
| `src/Editor/panels/RenderSettingsPanel.cpp` | 编辑器 Water / 距离雾面板 |
| `src/Core/ImGuiSystem.cpp` | 运行时 Water / Atmosphere 面板 |
