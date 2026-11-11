---
id: pipeline/gbuffer
title: G-Buffer 布局
type: pipeline
scope: src/RHI/GBuffer.h, shaders/mrt.frag, shaders/grid_mrt.*, shaders/skybox_mrt.*
read-when: 修改 G-Buffer 附件、格式，或改几何着色器输出
size: ~140 行
updated: 2026-11-11
---

# G-Buffer 布局

## 1. 五个附件

格式常量在 `src/RHI/GBuffer.h` 的 `namespace GBufferFormat`——**唯一来源**。

| 附件 | 格式 | 内容 | 谁写 | 谁读 |
|---|---|---|---|---|
| Position | `R32G32B32A32_SFLOAT` | 世界空间位置，`.w = 1` | mrt / grid_mrt / skybox_mrt | SSAO, Lighting, SSR, Water, Cloud |
| Normal | `R16G16B16A16_SFLOAT` | 世界空间法线；`.w` 是 SSAO policy mask（1=参与，0=跳过）；**天空盒 xyz 写 `(0,0,0)`** | 同上 | SSAO, Lighting |
| Albedo | `R16G16B16A16_SFLOAT` | 线性基础色；**天空盒往这里写线性 HDR 辐照度（可 > 1）** | 同上 | Lighting（透传） |
| Pbr | `R8G8B8A8_UNORM` | `.r` = 金属度（取 metallicRoughness 的 `.b`），`.g` = 粗糙度（取 `.g`） | mrt / grid_mrt | Lighting |
| Depth | `findDepthFormat()` -> 首选 `D32_SFLOAT` | 深度 | Geometry | 深度测试 |

### 1.1 为什么 Position 必须是 32F

Position 存的是**绝对世界坐标**，精度需求随离原点的距离增长。`R16G16B16A16_SFLOAT`
只有 10 位尾数，在 `|x|≈144` 处 ULP 已经是 `0.125`；而 SSAO 默认 `bias=0.047`、
`radius=0.33`，量化误差和 bias 同量级，深度比较会把同一块表面判成互相遮挡，
角色/远处物体上出现大片黑色花瓣或条带。相机挪到原点附近时伪影会消失，正是
“绝对坐标越大越严重”的判据。

改成 `R32G32B32A32_SFLOAT` 后 ULP 降到微米级，SSAO 在世界任意位置都稳定。
代价是每像素从 8 字节涨到 16 字节（1920×1080 多约 16.6 MB）；G-Buffer 采样器
是 `NEAREST`、附件也不混合，所以不需要 16F 的线性过滤能力，32F 只多占显存/带宽。
`src/RHI/GBuffer.h` 里用 `static_assert` 锁住格式，避免以后为了省显存改回 16F。

> 不要用“把 SSAO bias 调大”来掩盖：bias 调到 0.15 确实能压掉花瓣，但接触阴影
> 也会一起消失。详见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 38。

## 2. 为什么 Albedo 是 16F

天空盒把"线性 HDR 辐照度"直接写进 albedo 通道，由 Lighting 透传、Present 统一 tonemap。

8bit UNORM/SRGB 会在写入时把 > 1.0 的太阳/亮斑**直接截断**——截断后的 1.0
恰好又超过 Bloom 阈值，结果**整块天空都在起辉光**，变成一层假雾。

改成 16F 后 > 1 的高光才能活到 Bloom。

## 3. 天空盒的"接头暗号"

`skybox_mrt.frag` 把 `outNormal` 写成 `(0,0,0)`；`lighting.frag` 和 `ssao.frag`
都靠 `length(N) < 0.1` 判断"这是天空，原样透传 / 跳过遮蔽"。

**改动天空 XYZ=0 约定要三处同步**：`skybox_mrt.frag`、`lighting.frag`、`ssao.frag`。**改动 `.w` SSAO policy mask 要同步**：`mrt.frag`（普通几何写 1 / 地形写 0）、`grid_mrt.frag`（0）、`skybox_mrt.frag`（0）和 `ssao.frag`。

## 4. 三条几何路径

| 路径 | 管线 | 用途 |
|---|---|---|
| 普通网格 | `mrt.vert/.frag` | Sponza、OBJ 模型；`terrainMode=1` 时写顶点色 albedo |
| 网格线 | `grid_mrt.vert/.frag` | 地面网格（`isGrid` 标签；**当前无实体触发**） |
| 天空盒 | `skybox_mrt.vert/.frag` | 环境 cubemap |

地形仍走 `mrt.*` 和同一组 G-Buffer：法线/深度来自真实网格，albedo 写顶点色 + 程序化材质层，PBR 使用粗糙电介质。地形把 Normal `.w` 写成 0，`ssao.frag` 对它输出 1.0（无遮蔽），避免大面积平滑表面上的 SSAO 干涉条纹。细节见 [terrain](../30-systems/terrain.md)。

按 `RenderTagComponent` 的 `isSkybox` / `isGrid` / `isTerrain` 选择，见
[decisions](../10-architecture/decisions.md) 决策 14。

## 5. 法线贴图当前关闭

`mrt.frag::getNormalFromMap()` 保留但**不调用**：模型还不能提供可靠切线，
`outNormal` 直接写顶点法线。该函数内有两道拦截（UV 跨度 > 0.5 回退、
行列式 < 1e-5 回退），见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 9。

代价：bindless 的 `normalMaps` 数组白占描述符，见
[descriptors](../10-architecture/descriptors.md) §6。
