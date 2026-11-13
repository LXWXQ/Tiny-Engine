---
id: pipeline/shadow
title: 级联阴影（F6-a）
type: architecture
scope: [render, shadow]
size: ~110 行
read-when: 调阴影质量、排查阴影不显示、或改光照/相机参数时
updated: 2026-11-13
---

# 级联阴影（F6-a）

方向光阴影：三级联 CSM（Cascaded Shadow Maps），每级一张 2048x2048 深度图，
3 层合成一个 `VK_IMAGE_VIEW_TYPE_2D_ARRAY`；光照 pass 默认用 5x5 PCF 采样，可在 `scene.shadows.filterMode` 切换为 blocker-search PCSS。

## 1. 数据流

1. `ShadowPass::updateCascades(cameraView, cameraProjection, lightDirection)` 每帧在主相机
   矩阵确定后调用，算出三级联的光空间矩阵（含纹素吸附，避免相机移动时阴影边缘抖动）；
2. `FirstApp::render()` 把 `shadowMatrices[3]`、`shadowCascadeSplits`、`shadowParams`、`shadowFilterParams`
   写进 `GlobalUbo` 尾部；
3. `ShadowPass` 在渲染图里注册成**零附件 pass**（没有颜色/深度附件时渲染图只发屏障，
   不 `vkCmdBeginRendering`），回调里自己开一个只有深度附件的渲染过程，逐级联画全部
   投射者；
4. `shaders/lighting.frag` 的 `SampleShadow()` 按视深度选级、投影到光空间，再按 `shadowFilterParams.x` 分派 PCF/PCSS。
5. 树木投射者由 `VegetationPass::recordShadow()` 用独立深度管线追加；草不投影，用树根接触圆盘兜底。

## 2. PCF / PCSS

`scene.shadows.filterMode`（0 = PCF，1 = PCSS）由 `syncSceneSettings()` 写进
`EngineSettings::shadowFilterMode`，再打包到 `GlobalUbo.shadowFilterParams.x`；
`shaders/lighting.frag` 的 `SampleShadow()` 在选级/投影/bias 之后按 x 分派。

- **PCF**：固定 5x5 双线性采样，`texel = (1/res) * max(shadowParams.z, 1) * 0.5`。
- **PCSS**：先做 5x5 blocker search，`searchRadius = max(shadowParams.z, 1) * 2`；
  `stored < compare` 的样本计入遮挡，平均 blocker 深度与接收面深度差决定
  `penumbra = clamp((compare - averageBlocker) * 300.0, 1.0, 12.0)`，再用可变半径
  PCF 采样；没有 blocker 时直接返回 1。
- **交界处**：接触面 blocker 与接收面接近，penumbra 小，阴影硬；遮挡物离接收面远，
  penumbra 变大，阴影变软。
- **UI**：Inspector 的 `场景阴影` 组件 -> `阴影滤波` 下拉框；Render 面板的组件创建/
  编辑向导复用同一个枚举属性控件。
- **配置**：`engine.json` 的 `engineSettings.lighting.shadows.filterMode`；缺省 0，
  旧场景/旧配置自动按 PCF。

## 3. 手写正交投影（不要换回 glm::ortho）

```cpp
glm::mat4 lightProjection(1.0f);
lightProjection[0][0] = 1.0f / radius;
lightProjection[1][1] = 1.0f / radius;
lightProjection[2][2] = -1.0f / (2.0f * radius);
lightProjection[3][2] = -0.5f;
```

光相机放在级联包围球中心沿光线反方向 `radius * 2` 处，覆盖「距光相机 `r..3r`」，
深度按 `z_ndc = (-z_view - r) / (2r)` 线性映射到 `[0, 1]`。

**踩过的坑**：`glm::ortho(-r, r, -r, r, 0, 4r)` 在 `glm::lookAt` 的右手视空间
（相机前方是 −Z）+ `GLM_FORCE_DEPTH_ZERO_TO_ONE` 下会把整个场景压到**负深度**
（实测 `ndc.z ≈ -0.68`），阴影图永远是空的；换成 `(-4r, 0)` 又会推到 `> 1`
（1.24~1.73）。两个参数约定都不能凭直觉写，所以这里直接手写矩阵。

## 4. 剔除：AABB 8 角点

`cascadeVisible()` 把包围盒 8 个角点投影到光空间 NDC 取 min/max，再与
`[-1,1]x[-1,1]x[0,1]` 求交；任一角的 `clip.w <= 1e-5`（角点落在地形相机后方）时
保守返回 `true`。任何一维不相交就整块剔除。

早期版本用「包围盒中心 + 包围球半径」做单点判据，把 30x12x18 的 Sponza 整栋剔掉
（中心在近级联外、体积却有一半在框内），阴影图因此全空。

`recordCascade()` 里**不做相机视锥剔除**：阴影投射者可以来自相机外（视野外的高处
物体同样会把阴影投进画面），靠深度测试和 2048² 分辨率兜住开销。

## 5. 深度偏移的两处

- 光栅化端：`depthBiasEnable = VK_FALSE`（常量/斜率都关）。掠射角下把深度往外推
  会连接触阴影一起抹掉。
- 采样端：法线偏移 `samplePos = fragPosWorld + N * 0.05`，再叠斜率 bias
  `max(0.0008, 0.0030 * (1 - NdotL))`。

`cullMode` 用 `VK_CULL_MODE_BACK_BIT`（默认）。试过 `FRONT_BIT`（只画背面，几何上
消自遮蔽）：地形是单面网格，FRONT 剔除会让整个地形不进阴影图，所以不能用于本工程。

## 6. 已知限制

- **默认场景的阴影偏淡**：F6 的默认光照里 IBL 环境光占主导，方向光只占最终亮度的
  约 5%（实测把 `CalcDirectionalLight` 返回 0 只让画面均值掉 13/255）。阴影只能作用
  在这 5% 上，所以在外景里几乎看不出差异；室内或压低环境光后非常明显（室内机位
  实测开/关差异 `mean 15.6 / gt5 1,020,849 / max 59`）。
- 级联数固定 3、分辨率固定 2048；`kMaxShadowDistance = 90`、`kSplitLambda = 0.5`。
- 只有一个方向光投射阴影；点光源阴影未做。

## 7. 排查清单

阴影不显示时按这个顺序看：

1. `[Shadow] first frame | cascades=3 entities=N drawCalls=M` —— `M` 为 0 就是没画；
2. `[Shadow] selfTest PASS | cascades=3 lastSplit=90 corners inside, culling holds`
   自检挂了说明级联矩阵本身不对；
3. 光照强度：把方向光关掉（`return direct * 0.0;`）看画面是否**明显**变暗。若不明显，
   阴影再怎么对也看不出来，先调光照配比；
4. 采样端深度对比：`proj.z - stored` 的分布应跨零（接收面与自身背面的正常差值是
   负的，遮挡处为正）。

## 8. 相关

- [pass-order](pass-order.md)：Shadow 在 Geometry 之前注册
- [gbuffer](gbuffer.md)：法线附件被光照 pass 采样
- [cli-reference](../50-validation/cli-reference.md)：`--no-shadows`
