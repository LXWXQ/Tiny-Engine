---
id: systems/water
title: 水面
type: system
scope: src/Render/WaterPass.{h,cpp}, shaders/water.vert, shaders/water.frag
read-when: 改水面外观/波浪/反射、排查水面不出现或被地形裁掉
size: ~150 行
updated: 2026-11-05
---

# 水面

水面是 **Lighting 之后、TAA 之前**的前向混合通道（`WaterPass`）：读已经着色好的
线性 HDR 与 G-Buffer 深度，把水色、环境反射、泡沫按 alpha 叠上去。它不写 G-Buffer，
也不写深度。

## 1. 为什么是独立 Pass

- 透明表面写进 G-Buffer 只会保留"最前一个表面"，水后面的地形会整块消失；
- 延迟光照阶段没有"按像素重新混合多层表面"的位置，Fresnel / 反射需要单独一遍；
- 放在 Lighting 后，水能拿到完整的直接光 + IBL 结果；放在 TAA 前，水边缘也会被
  时域抗锯齿；放在 Bloom 前，太阳高光会参与辉光。

管线里的顺序是
`... -> Lighting -> Glass -> Water -> TAA -> Bloom -> Present`。完整顺序见
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
它们只贡献法线细节，不做顶点位移，并在距相机 60..220m 内淡出——否则细密波纹在
低采样率下会变成闪烁点阵。

法线：

```
gradient = bigWaveGradient + rippleGradient * rippleFade
N = normalize(vec3(-gradient.x, 1.0, -gradient.y))
```

## 4. 着色

- **Fresnel**：`0.02 + 0.98 * pow(1 - NdotV, 5)`；从水下往上看时翻转法线。
- **环境反射**：直接采样 Set0 binding 1 的等距柱状 HDR `environmentMap`，用
  `textureGrad` 并在 ±PI 接缝处折回导数，避免接缝退到最低 mip 出现亮线。
- **厚度吸收**：读 G-Buffer `inPosition`（不透明场景的世界坐标），沿视线算
  `thickness = sceneDistance - waterDistance`；`depthFactor = 1 - exp(-thickness * 0.22)`，
  水薄处偏浅、厚处偏深。天空的 10000m 让远处自动变成深海。
- **水体颜色**：`mix(shallow(0.045,0.150,0.160), deep(0.008,0.035,0.055), depthFactor)`；
  再乘 `0.55 + 0.45 * max(dot(N, L), 0)` 接方向光。
- **太阳高光**：`pow(NdotH, 900) * 3.0 + pow(NdotH, 120) * 0.10`，前者很窄、后者很淡。
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

- 只绑 Set0 全局描述符，不需要材质 Set；
- push constant 32 字节：`params=(time, level, waveStrength, waveSpeed)`、
  `params2=(farRadius, foamStrength, 0, 0)`；
- `timeSeconds` 由 Pass 自己按 `min(max(frameTime, 0), 0.1)` 累加，暂停或断点后不会跳变；
- 每次 draw 累加 telemetry 的 `drawCalls / triangles / vertices / descriptorBinds`。

## 6. 启用条件与开关

水面只在**有地面**时注册：registry 里至少有一个 `TerrainComponent` 或
`VoxelChunkComponent`。Sponza / 空场景不会出现一张 2m 高的全球水面。

| 入口 | 行为 |
|---|---|
| 编辑器 Render 面板 / 运行时 ImGui | `Water` 分组：Enabled、Level、Wave Strength、Wave Speed |
| `--no-water` | 关闭水面；Pass 不注册，打印一次 `[Water] disabled` |
| `config/engine.json` | `water: { enabled, level, waveStrength, waveSpeed }` |

默认值：`waterEnabled=true`、`waterLevel=2.0`、`waterWaveStrength=1.0`、
`waterWaveSpeed=1.0`。

## 7. 可观测与回归

启动：

```
[Water] initialized | rings=160 segments=192 vertices=30721 indices=183744 radius=4032m
```

首帧绘制：

```
[Water] first draw | level=2 waveStrength=1 waveSpeed=1 cameraY=46
```

回归用例：

| 名称 | 命令 | 断言 |
|---|---|---|
| `f3v-water` | `--selftest 20 --voxel-world 1 --hidden` | `[Water] initialized \|`、`[Water] first draw \|`、`[Validation] errors=0 warnings=0` |
| `f3v-water-off` | `--selftest 10 --voxel-world 1 --no-water --hidden` | `[Water] disabled`、验证层 0/0 |

截图可以用 `--screenshot build/water_test.png --screenshot-frame 20` 目检水体颜色、
反射与岸边泡沫。

## 8. 已知边界

- 水面是水平面，不支持任意朝向的水体；
- 没有折射、没有 SSR、没有水下后处理；
- 没有交互（角色入水、涟漪、溅射），也没有流场/泡沫贴图；
- 只在有地形/体素块时启用，不能单独给任意场景加水；
- 反射来自全局环境图，近处物体不会出现在水面倒影里；
- `--no-water` 时 Pass 完全不注册，没有"着色器短路"路径。

## 9. 关键文件

| 文件 | 职责 |
|---|---|
| `src/Render/WaterPass.h` | 接口、push constant、径向网格常量 |
| `src/Render/WaterPass.cpp` | 网格/缓冲、管线、Pass 注册、均匀量与统计 |
| `shaders/water.vert` | 径向网格跟随相机、4 个大波位移 |
| `shaders/water.frag` | 解析法线、Fresnel、环境反射、厚度吸收、泡沫 |
| `src/Core/FrameInfo.h` | `EngineSettings` 的 4 个 water 字段 |
| `src/Core/EngineConfig.cpp` | `water` 节点读写 |
| `src/Editor/panels/RenderSettingsPanel.cpp` | 编辑器 Render 面板 |
| `src/Core/ImGuiSystem.cpp` | 运行时 `Water` 面板 |
