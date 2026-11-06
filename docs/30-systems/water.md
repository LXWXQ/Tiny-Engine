---
id: systems/water
title: 水面
type: system
scope: src/Render/WaterPass.{h,cpp}, src/Render/ReflectionPass.{h,cpp}, shaders/water.vert, shaders/water.frag, Resources/Texture/Water/water_normal.png
read-when: 改水面外观/波浪/反射、排查水面不出现或被地形裁掉
size: ~260 行
updated: 2026-11-06
---

# 水面

水面是 **Lighting 之后、TAA 之前**的前向混合通道（`WaterPass`）：读已经着色好的
线性 HDR 与 G-Buffer 深度，把水色、环境反射、泡沫按 alpha 叠上去。它不写 G-Buffer，
也不写深度。片元阶段按像素足迹过滤大波，并用两层滚动法线贴图补细节，避免远处条带/闪烁；同一套距离雾也作用于
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

## 3. 波浪与水面细节

顶点只做 4 个方向波的高度位移，像素阶段解析求梯度得到大波法线。

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

**大波 footprint 过滤**：远处/掠射角下，一个像素覆盖的世界 XZ 范围会迅速变大，解析法线
在相邻像素间跳相位，形成条带和闪烁。着色器用 `dFdx(p)` / `dFdy(p)` 得到像素足迹，
对每个波判断相邻像素的相位跨度；超过约 1 rad 就淡出该波：

```
footprint = |dot(dir, dFdx(p))| + |dot(dir, dFdy(p))|
resolve   = 1 - smoothstep(0.5, 1.5, k * footprint)
grad     += dir * (amplitude * strength) * k * cos(phase) * resolve
variance += 0.5 * ((amplitude * strength) * k)^2 * (1 - resolve^2)
```

淡出的斜率能量不会丢，而是累加成 `slopeVariance`，下一节转成粗糙度。高频细节不再用
正弦 ripple，全部交给下面的法线贴图。

**法线贴图细节**：`Resources/Texture/Water/water_normal.png` 是 1024x1024、可无缝平铺的
水波法线（高通 FFT 频谱合成，保留 K>=32 的斜率频段，RGB 编码 `0.5 + 0.5 * n`，CC0）。片元按世界 XZ 平铺两层：

| 层 | 周期 | 流速 (UV/s) | 斜率权重 |
|---|---|---|---|
| A | 18m | (0.020, 0.012) | 0.55 |
| B | 42m | (-0.013, 0.017) | 0.35 |

低频（K<32）被刻意去掉：18m 这一层在低渲染倍率下（编辑器视口约 0.37x、710px 宽）
每个平铺块只占约 43px，低频脊线会随平铺重复成规则的亮暗格；去掉后只留下 18m 块内
约 0.56m 以下的细纹，mip 过滤能把它平均掉。低频形状由 4 个解析大波负责，不靠贴图。
生成命令（numpy + Pillow，默认参数即当前资产）：

```bash
python tools/GenerateWaterNormal.py --out Resources/Texture/Water/water_normal.png
```

两层都用 `textureGrad` 采样，显式传入 `dFdx(p)` / `dFdy(p)` 换算到各自 UV 的像素足迹，
掠射角下由硬件选 mip 和各向异性过滤。纹理的 mip 链不是 `vkCmdBlitImage` 逐级 blit，
而是 `Texture::tileableMips` 在 CPU 用 2x2 box filter 按尺寸取模生成，整条链一次
`vkCmdCopyBufferToImage`，平铺边缘不会因 clamp 偏色。

```
uvA = p / 18 + t * (0.020, 0.012)
uvB = p / 42 + t * (-0.013, 0.017)
slope = -(nA.xy * 0.55 + nB.xy * 0.35) * strength * detailFade
```

`detailFade = 1 - smoothstep(150, 700, distanceToCamera)`，避免极远处只剩重复纹理。
贴图 LOD 超过 1 后，用 `textureQueryLod` 的最大 LOD 估算被 mip 抹掉的斜率能量
`slopeVariance = smoothstep(1, 6, lod)^2 * 0.008`，和 footprint 过滤的方差一起进入
粗糙度，而不是让高光随机闪。

法线：

```
gradient = filteredBigWaveGradient + waterDetailGradient
N = normalize(vec3(-gradient.x, 1.0, -gradient.y))
```

## 4. 着色

- **Fresnel**：`0.02 + 0.98 * pow(1 - NdotV, 5)`；从水下往上看时翻转法线。
- **像素足迹 → 粗糙度**：把上一步的 `bigSlopeVariance + detailSlopeVariance` 转成
  `roughness = clamp(sqrt(bigSlopeVariance + detailSlopeVariance) * 7.0 + distanceRoughness, 0.02, 0.65)`；
  `distanceRoughness = smoothstep(120, 3000, distanceToCamera) * 0.30`。
  `roughNorm = smoothstep(0.03, 0.55, roughness)` 控制反射模糊与高光展宽。
- **环境反射**：直接采样 Set0 binding 1 的等距柱状 HDR `environmentMap`，用
  `textureGrad` 并在 ±PI 接缝处折回导数，避免接缝退到最低 mip 出现亮线；导数再乘
  `1 + roughness * 12`，让远处未解析的波变成略模糊的反射，而不是随机镜面点。
- **平面反射**：开启 `waterPlanarReflection` 时，Set0 binding12 绑定 `ReflectionPass`
  输出的 960x540 反射色，片元按 `screenUV + N.xz * (0.0015 + roughness * 0.006)`
  扰动后采样，覆盖环境图反射；关闭时 binding12 回退到环境图。反射通道用镜像相机
  重画不透明几何并做第二套延迟光照，水面以下几何被 `clipY` 裁掉；玻璃、水面和
  调试网格不进入倒影。半分辨率结果直接按屏幕 UV 上采样，边缘比全分辨率软。
- **环境反射底色**：平面反射/SSR 之后，按反射亮度补天空底色：
  `luma = dot(reflection, (0.2126,0.7152,0.0722))`、
  `dark = 1 - smoothstep(0.04, 0.22, luma)`、
  `reflection = mix(reflection, environmentReflection, 0.34 * dark)`。掠射角下平面反射
  往往整片采到对岸深色地形，若完全覆盖环境图，远水会变成一块暗色平板；只提亮
  暗反射，亮的水面/天空倒影保持原对比度，岸线倒影也仍然可见。
- **屏幕空间反射（F6-01）**：`ssr.frag` 在半分辨率 960x540 上从水面像素沿反射方向做 48 步 DDA（水面用解析平面 y = waterLevel 求交，只读本帧 HDR 与 G-Buffer 世界坐标，不读 Normal/Depth），把射线步进点投影回屏幕后采样 G-Buffer 位置，点到射线的垂直距离超过厚度（默认 0.5m，按步长自适应放大）的命中会被丢弃；命中的 alpha 是置信度，屏幕外/超过 `ssrMaxDistance`（默认 200m）返回 0。Water 用同一个扰动后的 `reflectionUV` 采样绑定 13 的 SSR 结果，权重为 `ssr.a * (1 - smoothstep(0.20, 0.55, roughness)) * (1 - smoothstep(120, 220, distanceToCamera))`，再与平面反射/环境图混合：粗糙水面和远水仍交给平面反射/IBL，近场平静水面才吃屏幕空间结果。SSR 只服务水面，不写深度，不影响不透明几何。
- **厚度吸收**：读 G-Buffer `inPosition`（不透明场景的世界坐标），沿视线算
  `thickness = sceneDistance - waterDistance`；`depthFactor = 1 - exp(-thickness * 0.22)`，
  水薄处偏浅、厚处偏深。天空的 10000m 让远处自动变成深海。
- **水体颜色**：`mix(shallow(0.050,0.160,0.167), deep(0.016,0.058,0.075), depthFactor)`；
  再乘 `0.55 + 0.45 * max(dot(N, L), 0)` 接方向光。深水色比旧值提亮约 2 倍，
  避免没有几何底的海面显示成近黑色。
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
- **混合**：`alpha = mix(0.18, 0.89, depthFactor) + fresnel * 0.55`，上限 0.985；
  管线用 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`，alpha 通道写 1。远水上限从 0.92
  降到 0.89，保留少量背景透出，避免整片水变成不透明深色板。

**不做折射**：RenderGraph 没有 image copy，折射需要先把 `hdrScene` 复制成可采样
纹理，这部分仍未做。反射现在有三层来源：屏幕空间 SSR（近场、平静水面）、
半分辨率平面反射（屏幕外/远水）、环境图（最终回退）。

## 5. Pass 合同

```
writeColor(hdrScene[frame], LOAD)
writeDepth(gbDepth[frame], LOAD)
blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA
depthTest: on, depthWrite: off, compare: LESS_OR_EQUAL
cullMode: NONE
```

- 只绑 Set0 全局描述符，不需要材质 Set；binding12 是平面反射纹理，binding13 是
  SSR 结果，binding14 是 1024x1024 无缝水波法线；对应开关关闭时由 `Application`
  回退绑定环境图（描述符必须始终有效，水面 shader 用 `params3.x/y` 分支决定是否采样）；
- push constant 48 字节：`params=(time, level, waveStrength, waveSpeed)`、
  `params2=(farRadius, foamStrength, fogStart, fogDensity)`、
  `params3=(planarReflection, ssrEnabled, 0, 0)`；雾关闭时密度写 0；
- `GlobalUbo` 尾部新增 `fogParams=(fogStart, fogDensity, 0, enabled)`，光照/水面/玻璃共用；
  `lighting.frag`、`glass.frag` 的参数分别来自 UBO 与 32 字节的 glass push constant；
- `timeSeconds` 由 Pass 自己按 `min(max(frameTime, 0), 0.1)` 累加，暂停或断点后不会跳变；
- 每次 draw 累加 telemetry 的 `drawCalls / triangles / vertices / descriptorBinds`。

## 6. 启用条件与开关

水面只在**有地面**时注册：registry 里至少有一个 `TerrainComponent` 或
`VoxelChunkComponent`。Sponza / 空场景不会出现一张 2m 高的全球水面。

| 入口 | 行为 |
|---|---|
| 编辑器 Render 面板 / 运行时 ImGui | `Water` 分组：Enabled、Level、Wave Strength、Wave Speed、Planar Reflection、Screen Space Reflection、SSR Max Distance；另有 `距离雾` / `Atmosphere / Fog` 分组：Enabled、Start、Density |
| `--no-water` | 关闭水面；Water/PlanarReflection/SSR 都不注册，打印一次 `[Water] disabled` |
| `--no-ssr` | 关闭屏幕空间反射；SSR 不注册，打印一次 `[SSR] disabled`，水面 `ssr=0` |
| `config/engine.json` | `water: { enabled, level, waveStrength, waveSpeed, planarReflection }`、`ssr: { enabled, maxDistance }`、`fog: { enabled, start, density }` |
| 水波法线贴图 | Set0 binding14 常驻；没有独立开关，文件缺失会在启动加载时报错 |

默认值：`waterEnabled=true`、`waterLevel=2.0`、`waterWaveStrength=1.0`、
`waterWaveSpeed=1.0`、`waterPlanarReflection=true`、`ssrEnabled=true`、`ssrMaxDistance=200m`；`fogEnabled=true`、`fogStart=80m`、`fogDensity=0.00045`
（约 1km 处 34%、4km 处 83%）。雾是全局设置，地形/水面/玻璃共用；没有单独的
`--no-fog`，通过配置或 UI 关闭。

## 7. 可观测与回归

启动：

```
[Tiny] Water normal map loaded | 1024x1024 UNORM wrapped mips.
[Reflection] planar reflection initialized | extent=960x540
[SSR] initialized | extent=960x540 steps=48
[Water] initialized | rings=160 segments=192 vertices=30721 indices=183744 radius=4032m
```

首帧绘制：

```
[Reflection] first draw | level=2 mirrorY=-6.8105 extent=960x540
[SSR] first draw | extent=960x540 waterLevel=2 maxDistance=200 planarReflection=1
[Water] first draw | level=2 waveStrength=1 waveSpeed=1 cameraY=46 planarReflection=1 ssr=1
```

平面反射关闭时没有 `[Reflection] first draw`，水面日志为 `planarReflection=0`；
SSR 关闭时没有 `[SSR] first draw`，水面日志为 `ssr=0`。

回归用例：

| 名称 | 命令 | 断言 |
|---|---|---|
| `f3v-water` | `--selftest 20 --voxel-world 1` | `[Tiny] Water normal map loaded \|`、`[Water] initialized \|`、`[Water] first draw \|`、`[Validation] errors=0 warnings=0` |
| `f3v-water-reflection` | `--selftest 20 --voxel-world 1` | `[Reflection] planar reflection initialized \|`、`[Reflection] first draw \|`、`planarReflection=1`、验证层 0/0 |
| `f3v-water-off` | `--selftest 20 --voxel-world 1 --no-water` | `[Water] disabled`、验证层 0/0 |
| `f3v-ssr` | `--selftest 20 --voxel-world 1` | `[SSR] initialized`、`[SSR] first draw`、`[Water] first draw`、`ssr=1`、验证层 0/0 |
| `f3v-ssr-off` | `--selftest 20 --voxel-world 1 --no-ssr` | `[SSR] disabled`、`[Water] first draw`、`ssr=0`、验证层 0/0 |

截图可以用 `--screenshot build/water_test.png --screenshot-frame 20` 目检水体颜色、
反射与岸边泡沫；把相机转向开阔水面、拉远到 1~4km，可对比 `fog.enabled` 开关前后的
地平线过渡与远处高光。

## 8. 已知边界

- 水面是水平面，不支持任意朝向的水体；
- 没有折射、没有水下后处理；SSR 只做水面近场（半分辨率、屏幕空间、默认 200m），
  屏幕外的物体不会出现在 SSR 里，靠平面反射/环境图回退；
- 距离雾是均匀指数雾，没有高度衰减、体积光/太阳散射，也没有时间变化；雾色固定取
  环境图地平线 LOD3，天空本身不参与雾；
- footprint 过滤只修法线和高光，不改变几何；外圈网格段长约 132m，5.5m 波仍有几何
  欠采样，远水细节依赖法线过滤而不是真实位移；
- 没有交互（角色入水、涟漪、溅射），也没有流场/泡沫贴图；水面细节来自单张高通法线贴图，
  18m/42m 两层滚动只能降低重复感，极近处仍可能看出周期；低渲染倍率下的平铺亮暗格已
  通过去掉 K<32 低频解决，但这不是通用的反走样方案；
- 只在有地形/体素块时启用，不能单独给任意场景加水；
- 平面反射只渲染不透明几何：玻璃、水面、调试网格不进入倒影，粒子/透明材质也不会；
- 反射用主相机的阴影级联，镜像视角下的阴影/SSAO 不精确；半分辨率且没有独立时域
  累积，快速移动相机时倒影边缘会偏软或有轻微抖动；
- TAA 只有相机重投影、没有运动向量，水面动画和物体自身运动不会被单独补偿；
- `--no-water` 时 Pass 与 PlanarReflection/SSR 都完全不注册，没有"着色器短路"路径；
  `--no-ssr` 只关闭 SSR。雾没有命令行开关，平面反射只有 UI/配置开关；
- SSR 是半分辨率、单帧、无时域累积的屏幕空间追踪：水面动画和薄几何会产生轻微
  抖动/漏光，快速移动相机时近场倒影可能断续；这是屏幕空间方法的固有限制。

## 9. 关键文件

| 文件 | 职责 |
|---|---|
| `src/Render/WaterPass.h` | 接口、push constant、径向网格常量 |
| `src/Render/WaterPass.cpp` | 网格/缓冲、管线、Pass 注册、均匀量与统计 |
| `src/Render/ReflectionPass.{h,cpp}` | 镜像相机、半分辨率 G-Buffer/光照、反射纹理与开关 |
| `src/Render/SSRPass.{h,cpp}` | 半分辨率 SSR 目标、描述符、48 步追踪 Pass、开关与日志 |
| `shaders/water.vert` | 径向网格跟随相机、4 个大波位移 |
| `shaders/water.frag` | 大波 footprint 过滤、两层法线贴图细节、粗糙度反射、Fresnel、SSR/平面反射混合、厚度吸收、泡沫、距离雾 |
| `shaders/ssr.frag` | 半分辨率屏幕空间 DDA、G-Buffer 世界位置/射线厚度校验、置信度输出 |
| `shaders/lighting.frag` | 不透明几何的距离雾；天空提前返回 |
| `shaders/glass.frag` | 玻璃距离雾（只淡出颜色，alpha 不变） |
| `src/Core/FrameInfo.h` | `EngineSettings` 的 water/fog 字段、`GlobalUbo.fogParams` |
| `Resources/Texture/Water/water_normal.png` | 1024x1024 高通无缝 FFT 水波法线（K>=32），`tileableMips` 生成 wrapped mip 链 |
| `tools/GenerateWaterNormal.py` | 可复现生成上述法线贴图（cutoff=32、seed=44） |
| `src/Core/Application.cpp` | 加载水波法线；每帧上传 `fogParams`；开关变化时重建全局描述符，binding12 在反射纹理/环境图之间切换 |
| `src/Core/EngineConfig.cpp` | `water` / `fog` 节点读写 |
| `src/Editor/panels/RenderSettingsPanel.cpp` | 编辑器 Water / 距离雾面板 |
| `src/Core/ImGuiSystem.cpp` | 运行时 Water / Atmosphere 面板 |
