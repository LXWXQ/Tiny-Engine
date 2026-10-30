---
id: pipeline/gbuffer
title: G-Buffer 布局
type: pipeline
scope: src/RHI/GBuffer.h, shaders/mrt.frag, shaders/grid_mrt.*, shaders/skybox_mrt.*
read-when: 修改 G-Buffer 附件、格式，或改几何着色器输出
size: ~120 行
updated: 2026-10-30
---

# G-Buffer 布局

## 1. 五个附件

格式常量在 `src/RHI/GBuffer.h` 的 `namespace GBufferFormat`——**唯一来源**。

| 附件 | 格式 | 内容 | 谁写 | 谁读 |
|---|---|---|---|---|
| Position | `R16G16B16A16_SFLOAT` | 世界空间位置，`.w = 1` | mrt / grid_mrt / skybox_mrt | SSAO, Lighting |
| Normal | `R16G16B16A16_SFLOAT` | 世界空间法线，`.w = 1`；**天空盒写 `(0,0,0)`** | 同上 | SSAO, Lighting |
| Albedo | `R16G16B16A16_SFLOAT` | 线性基础色；**天空盒往这里写线性 HDR 辐照度（可 > 1）** | 同上 | Lighting（透传） |
| Pbr | `R8G8B8A8_UNORM` | `.r` = 金属度（取 metallicRoughness 的 `.b`），`.g` = 粗糙度（取 `.g`） | mrt / grid_mrt | Lighting |
| Depth | `findDepthFormat()` -> 首选 `D32_SFLOAT` | 深度 | Geometry | 深度测试 |

## 2. 为什么 Albedo 是 16F

天空盒把"线性 HDR 辐照度"直接写进 albedo 通道，由 Lighting 透传、Present 统一 tonemap。

8bit UNORM/SRGB 会在写入时把 > 1.0 的太阳/亮斑**直接截断**——截断后的 1.0
恰好又超过 Bloom 阈值，结果**整块天空都在起辉光**，变成一层假雾。

改成 16F 后 > 1 的高光才能活到 Bloom。

## 3. 天空盒的"接头暗号"

`skybox_mrt.frag` 把 `outNormal` 写成 `(0,0,0)`；`lighting.frag` 和 `ssao.frag`
都靠 `length(N) < 0.1` 判断"这是天空，原样透传 / 跳过遮蔽"。

**改动这条约定要三处同步**：`skybox_mrt.frag`、`lighting.frag`、`ssao.frag`。

## 4. 三条几何路径

| 路径 | 管线 | 用途 |
|---|---|---|
| 普通网格 | `mrt.vert/.frag` | Sponza、OBJ 模型 |
| 网格线 | `grid_mrt.vert/.frag` | 地面网格（`isGrid` 标签；**当前无实体触发**） |
| 天空盒 | `skybox_mrt.vert/.frag` | 环境 cubemap |

按 `RenderTagComponent` 的 `isSkybox` / `isGrid` 选择，见
[decisions](../10-architecture/decisions.md) 决策 14。

## 5. 法线贴图当前关闭

`mrt.frag::getNormalFromMap()` 保留但**不调用**：模型还不能提供可靠切线，
`outNormal` 直接写顶点法线。该函数内有两道拦截（UV 跨度 > 0.5 回退、
行列式 < 1e-5 回退），见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 9。

代价：bindless 的 `normalMaps` 数组白占描述符，见
[descriptors](../10-architecture/descriptors.md) §6。
