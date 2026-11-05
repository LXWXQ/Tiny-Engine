---
id: pipeline/pass-order
title: Pass 顺序与原因
type: pipeline
scope: src/Core/Application.cpp, src/Render/*.cpp
read-when: 调整 Pass 顺序、新增 Pass、排查"某张图为什么是黑的"
size: ~125 行
updated: 2026-11-05
---

# Pass 顺序与原因

## 1. 顺序

```
TerrainGpuSelection  B-03：无附件 pass；compute 选层/剔除，写 indirect command/count
   |
SceneGpuCull        F1：无附件 pass；摊平实例 + 视锥/Hi-Z 剔除，写组级 indirect command
   |
GeometryPass      写 G-Buffer 5 张附件（4 颜色 + 深度，全部 CLEAR），消费 GPU 命令
   |
HiZBuild          F1-c：无附件 pass；读 G-Buffer.Position 建下一帧用的 Hi-Z 金字塔（错帧一帧）
   |
SSAOPass          读 G-Buffer.Position/Normal -> 写 R8_UNORM 遮蔽图
   |
SSAOBlur          模糊遮蔽图
   |
LightingPass      读 G-Buffer + SSAO + IBL 三件套 + 环境图 -> 写 R16G16B16A16_SFLOAT（线性 HDR）
   |
PlanarReflection  半分辨率平面反射：镜像相机重画 960x540 G-Buffer + 延迟光照 -> 反射色，供水面倒影
   |
GlassPass         V-01：体素玻璃前向混合；LOAD HDR + 深度，写深度关，按面质心远->近，只画 Glass
   |
WaterPass         水面前向混合：LOAD HDR + 深度，写深度关；径向网格铺到约 4km，逐像素 footprint 过滤波浪 + 距离雾
   |
TAAPass           时域抗锯齿：读 HDR / G-Buffer.Position / 上一帧 history，写 taaOutput + 新 history
   |
Bloom.Bright      taaOutput -> level0（1/2 分辨率）
Bloom.Down0/1/2   level0 -> level1 -> level2 -> level3
Bloom.Up2/1/0     level3 -> level2 -> level1 -> level0（加法混合累加）
   |
PresentPass       TAA 输出 + Bloom*intensity -> 曝光 -> ACES -> gamma(1/2.2) -> 交换链，并叠加 ImGui
   |
PresentTransition 只发一个布局转换到 PRESENT_SRC_KHR
```

`TerrainGpuSelection` 与 `SceneGpuCull` 都由 `GeometryPass::addToGraph()` 在 `Geometry` 之前注册（顺序固定为 Terrain -> SceneGpuCull -> Geometry）：两者写的 indirect command 会被 `Geometry` 的 `vkCmdDrawIndexedIndirect` 直接消费。
`HiZBuild` 必须排在 `Geometry` **之后**：金字塔要包含本帧刚画出的几何；它写的是 `(frameInFlight + 1) % kFramesInFlight` 槽，供下一帧 `SceneGpuCull` 读（错帧一帧，第一帧 `params.z = 0` 关闭遮挡）。
渲染图里注册的 pass 名依次是：
`TerrainGpuSelection`, `SceneGpuCull`, `Geometry`, `HiZBuild`, `SSAO`, `SSAOBlur`, `Lighting`, `PlanarReflection.GBuffer`, `PlanarReflection.Lighting`, `Glass`, `Water`, `TAA`,
`Bloom.Bright`, `Bloom.Down0/1/2`, `Bloom.Up2/1/0`, `Present`,
`PresentTransition`。

> `PresentTransition` 不是 `IRenderPass`——它是 `RenderGraph::presentTransition()`
> 临时 `addGraphicsPass` 出来的**没有颜色/深度附件**的 pass。
>
> `TerrainGpuSelection` 同样没有附件，但它是 **graphics queue 上的 pre-render
> compute**：必须放在 dynamic rendering 之外、与 `Geometry` 同一 command buffer，
> 这样 compute 写的 indirect/count buffer 可以直接被后续 draw 消费。不能改成
> `addComputePass` 丢到独立 compute queue，否则需要额外的跨队列所有权/信号量。
>
> `terrain_mesh.comp` **不是** render graph pass：它在场景 Update 阶段由
> `TerrainGpuMeshBuilder` 的 4 个异步 slot 提交到 graphics queue，Scene 每帧
> `poll`；完成后才把 device-local VBO/IBO 安装到 Model。submit 不在渲染线程
> 等待 fence，也不占用 Geometry pass 的 command buffer。

## 2. 为什么是这个顺序

| 约束 | 原因 |
|---|---|
| SSAO 在 Lighting **之前** | SSAO 调制环境光项（`ambient *= ssao`），是光照的输入 |
| Glass 在 Lighting **之后**、Bloom **之前** | 玻璃必须混合在已经着色好的 HDR 上（延迟着色只存一个表面，玻璃进 G-Buffer 会把后面的层盖掉）；放在 Bloom 前，玻璃高光也参与辉光。深度 `LOAD` 但 `depthWrite = FALSE`、`compare = LESS_OR_EQUAL`：不透明几何仍挡住玻璃，玻璃之间由 CPU 排序（远->近）决定顺序 |
| PlanarReflection 在 Lighting **之后**、Glass/Water **之前** | 反射必须用本帧 G-Buffer 和 IBL 重新着色；透明 Pass 之前写完，Water 才能把反射色当输入。只有水面启用、平面反射开启且场景有地形/体素块时才注册；镜像相机沿水面对称，用未抖动投影重画半分辨率 960x540 的 G-Buffer，再用第二套 Lighting 着色，开销约为全分辨率的一半 |
| Water 在 Glass **之后**、TAA **之前** | 水面要混合进已经着色的 HDR；先于 TAA 才能让水边缘参与时域抗锯齿；先于 Bloom 才能让太阳高光参与辉光。深度 `LOAD`、`depthWrite = FALSE`、`compare = LESS_OR_EQUAL`，不透明几何与玻璃都能挡住它；相机中心径向网格跟随相机，铺到约 4km。片元用 `dFdx/dFdy` 足迹逐波过滤，未解析能量转成粗糙度/高光展宽；距离雾在 Lighting、Water、Glass 内共用 `fogParams` |
| TAA 在 Water **之后**、Bloom **之前** | 先把玻璃和水面混合进 HDR 再做时域解析，透明边缘也能被抗锯齿；Bloom 读到的是已收敛的 HDR，不会把抖动噪声放大成辉光。TAA 用未抖动 VP 把 G-Buffer 世界坐标反投影到上一帧，不需要运动向量（当前只有相机运动）；ImGui 在 Present 内叠加，不参与历史混合 |
| Bloom 在 tonemap **之前** | ACES 最后一步是 `clamp(0,1)`，高光被压到 1.0 后 Bloom 只能"把白涂白" |
| 曝光在 Bloom **之后** | 曝光是 tonemap 的输入缩放；若写在光照里，Bloom 阈值会随曝光漂移，拖滑块时辉光乱跳 |
| ImGui **最后** | 放在 Present 里，以后往 Present 前插任何后效，UI 都自动不被糊掉 |

## 3. 可关闭的 Pass

| 开关 | 关闭时行为 |
|---|---|
| `--no-ssao` | SSAO Pass 仍执行，但着色器最前面就 `enabled < 0.5 -> 输出 1.0`（连 G-Buffer 采样都省） |
| `--no-bloom` | **真的跳过 7 个 Bloom pass**；Present 走 `present_no_bloom.frag` 单输入管线 |
| `--no-taa` | TAA Pass 仍执行，但 `historyWeight = 0`、jitter = 0，着色器直接输出当前帧；窗口/渲染目标 resize 与重新开启时 `historyValid = false`，首帧不吃旧历史 |
| `--no-water` | Water Pass **完全不注册**，只打印一次 `[Water] disabled`；有地形/体素块时才可能注册。PlanarReflection 的两个 Pass 也因 `waterEnabled=false` 不注册，全局 Set0 的 binding12 回退到环境图 |
| 距离雾 | 没有命令行开关；只能通过 `config/engine.json` 的 `fog.enabled` 或编辑器/运行时 UI 关闭。它不是 Pass，关闭时着色器按密度 0 跳过混合 |

距离雾不是独立 Pass：`GlobalUbo.fogParams`（x=起始距离、y=密度、z=保留、w=启用）由
`Application` 每帧上传，`lighting.frag` 对不透明几何、`water.frag` 对水面各自在
输出前混合；`glass.frag` 通过自己的 push constant 拿到同一组参数，只淡出颜色而
不改 alpha。雾色统一采样环境图地平线方向（LOD3），所以远景和天空无缝衔接。天空像素
在 `lighting.frag` 提前返回，不会被雾二次染色。

为什么 SSAO 是"Pass 照跑 + 着色器短路"而不是跳过：`lighting.frag` 里 `ambient *= ssao` 是
**无条件**执行的，跳过会让它乘上未定义内容。这是真实踩过的 bug（见
[pitfalls](../60-debug-profiling/pitfalls.md) 坑 14）。

为什么 Bloom 能真跳过：PresentPass 准备了**两套描述符布局**（开：`taaOutput`+`bloomResult`；
关：只有 `taaOutput`），关闭时根本不绑 `bloomResult`，于是语义校验的不变量成立。

为什么 TAA 不能真跳过：Bloom 与 Present 的输入都固定在 `taaOutput` 上，跳过 TAA 会让这张
瞬时图本帧没人写，渲染图的"必须有人写"硬校验会直接抛异常。关闭时让权重与抖动都为 0，
输出等价于未抖动的当前帧；重新开启时 `historyValid = false`，避免把关闭期间的内容拉进历史。

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
