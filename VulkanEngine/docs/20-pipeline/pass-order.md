---
id: pipeline/pass-order
title: Pass 顺序与原因
type: pipeline
scope: src/Core/Application.cpp, src/Render/*.cpp
read-when: 调整 Pass 顺序、新增 Pass、排查"某张图为什么是黑的"
size: ~180 行
updated: 2026-10-30
---

# Pass 顺序与原因

## 1. 顺序

```
GeometryPass      写 G-Buffer 5 张附件（4 颜色 + 深度，全部 CLEAR）
   |
SSAOPass          读 G-Buffer.Position/Normal -> 写 R8_UNORM 遮蔽图
   |
SSAOBlur          模糊遮蔽图
   |
LightingPass      读 G-Buffer + SSAO + IBL 三件套 + 环境图 -> 写 R16G16B16A16_SFLOAT（线性 HDR）
   |
Bloom.Bright      HDR -> level0（1/2 分辨率）
Bloom.Down0/1/2   level0 -> level1 -> level2 -> level3
Bloom.Up2/1/0     level3 -> level2 -> level1 -> level0（加法混合累加）
   |
PresentPass       HDR + Bloom*intensity -> 曝光 -> ACES -> gamma(1/2.2) -> 交换链，并叠加 ImGui
   |
PresentTransition 只发一个布局转换到 PRESENT_SRC_KHR
```

渲染图里注册的 pass 名依次是：
`Geometry`, `SSAO`, `SSAOBlur`, `Lighting`, `Bloom.Bright`, `Bloom.Down0/1/2`,
`Bloom.Up2/1/0`, `Present`, `PresentTransition`。

> `PresentTransition` 不是 `IRenderPass`——它是 `RenderGraph::presentTransition()`
> 临时 `addGraphicsPass` 出来的**没有颜色/深度附件**的 pass。

## 2. 为什么是这个顺序

| 约束 | 原因 |
|---|---|
| SSAO 在 Lighting **之前** | SSAO 调制环境光项（`ambient *= ssao`），是光照的输入 |
| Bloom 在 tonemap **之前** | ACES 最后一步是 `clamp(0,1)`，高光被压到 1.0 后 Bloom 只能"把白涂白" |
| 曝光在 Bloom **之后** | 曝光是 tonemap 的输入缩放；若写在光照里，Bloom 阈值会随曝光漂移，拖滑块时辉光乱跳 |
| ImGui **最后** | 放在 Present 里，以后往 Present 前插任何后效，UI 都自动不被糊掉 |

## 3. 可关闭的 Pass

| 开关 | 关闭时行为 |
|---|---|
| `--no-ssao` | SSAO Pass 仍执行，但着色器最前面就 `enabled < 0.5 -> 输出 1.0`（连 G-Buffer 采样都省） |
| `--no-bloom` | **真的跳过 7 个 Bloom pass**；Present 走 `present_no_bloom.frag` 单输入管线 |

为什么 SSAO 是"Pass 照跑 + 着色器短路"而不是跳过：`lighting.frag` 里 `ambient *= ssao` 是
**无条件**执行的，跳过会让它乘上未定义内容。这是真实踩过的 bug（见
[pitfalls](../60-debug-profiling/pitfalls.md) 坑 14）。

为什么 Bloom 能真跳过：PresentPass 准备了**两套描述符布局**（开：`hdrScene`+`bloomResult`；
关：只有 `hdrScene`），关闭时根本不绑 `bloomResult`，于是语义校验的不变量成立。

> 这两条差异是刻意的，不是不一致。**新增可关闭 Pass 时先判断它属于哪一类。**

## 4. 管线约定

- 所有图形管线启用 `VK_DYNAMIC_STATE_VIEWPORT` + `VK_DYNAMIC_STATE_SCISSOR`，
  渲染图在 `vkCmdBeginRendering` 之后自动调 `cmdSetFullViewportAndScissor()`。
- **不走渲染图的录制路径**（`IBLBaker::renderFaces` / `renderBrdfLut`）必须自己调。
- ImGui 后端自己设 viewport/scissor，PresentPass 里不用补。
- 颜色附件格式必须与 [gbuffer](gbuffer.md) 的 `GBufferFormat` 常量**完全一致**，
  否则动态渲染管线与渲染目标不匹配。

## 5. 新增 Pass 的步骤

1. 实现 `IRenderPass`（`init` / `addToGraph` / 可选 `resize`）；
2. 在 `Application` 构造的 Pass 注册段按顺序 `renderPipeline.addPass(...)`；
3. 声明全部 `read` / `writeColor` / `writeBuffer`（**漏声明 = 漏同步**）；
4. 如果需要中间目标，在渲染图 `createTexture` 并登记到 `RenderResources`；
5. 如果可关闭，加命令行开关 + 在 `tools/Validate.ps1` 加一项回归；
6. 更新 [shaders](shaders.md) 清单与本文件 §1。
