---
id: systems/vegetation
title: 地形植被
type: system
scope: src/Vegetation/, src/Render/VegetationPass.{h,cpp}, shaders/vegetation.vert, shaders/vegetation.frag
read-when: 改草/树的分布、外观、风摆，或排查植被不出现、长在水下/陡坡上、帧率骤降
size: ~150 行
updated: 2026-11-10
---

# 地形植被

F6-07 给程序化地形加了一层**程序化**草与树：没有外部模型/贴图资产，全部在
`VegetationGenerator` 里生成网格和实例，由 `VegetationPass` 在
**Geometry 之后、SSAO 之前**以实例化方式写进同一套 G-Buffer。因此草和树能接收
SSAO、级联阴影和延迟光照，但**不投自身阴影**，也不进入平面反射。

植被是**场景级组件** `scene.vegetation`，不是实体组件：它描述“这张场景的地表
植被”，挂在隐藏的场景设置实体上，随场景 v4 的 `sceneComponents` 存读；添加/
启用/调参/移除即功能开关，默认关闭（`EngineSettings::vegetationEnabled = false`）。

## 1. 为什么是独立 Pass

- 草/树是**实例化**的：一棵树 96 顶点、一簇草 12 顶点，但实例有上万；走普通
  `GeometrySystem` 会把它们塞进场景 ECS 的可见集合，既浪费 CPU 剔除，也不方便
  按地形变化整体重排。
- 草叶需要**逐片元 alpha-test**（`discard`），不能写进普通不透明 G-Buffer 管线；
  专用管线可以在片元里裁形状，同时保持不透明深度。
- 植被必须写 G-Buffer 才能被 SSAO/阴影/延迟光照正确着色；如果放在 Lighting 后
  做前向透明，会失去与地形一致的受光，也会被水面/玻璃的排序干扰。
- 放在 Geometry 后面可以用 `LOAD` 保留地形已写好的四张颜色和深度，只追加植被，
  不用重画地形；放在 SSAO 前，SSAO 能把草/树的接触阴影算进去。

管线顺序见 [pass-order](../20-pipeline/pass-order.md)：
`Geometry -> HiZBuild -> Vegetation -> SSAO -> Lighting -> ...`。

## 2. 组件与参数

`scene.vegetation` 有 12 个属性，Inspector 的 Add Scene Component 里分类为“地形”：

| 属性 | 默认 | 作用 |
|---|---|---|
| `enabled` | false（引擎默认）/ true（手动添加时） | 总开关；关闭时 Pass 早退，不放置、不绘制 |
| `grassDensity` | 0.8 | 每平方米草簇数；间距 = `1 / sqrt(density)` |
| `grassDistance` | 50 m | 草的放置半径（围绕相机） |
| `grassHeight` | 0.55 m | 草簇高度基数；实例再乘 0.72–1.28 的随机 |
| `treeDensity` | 1.2 | 每 1000 m² 树的数量；间距 = `sqrt(1000 / density)` |
| `treeDistance` | 120 m | 树的放置半径 |
| `treeHeight` | 7 m | 树高基数；实例再乘 0.78–1.26 的随机 |
| `windStrength` | 0.6 | 风摆幅度；0 时完全静止 |
| `seed` | 1337 | 确定性种子；换种子会换整片分布 |
| `grassColor` | (0.24, 0.36, 0.10) | 草的基础色，实例再乘 0.82–1.12 的亮度扰动 |
| `treeColor` | (0.12, 0.24, 0.08) | 树冠基础色 |
| `trunkColor` | (0.22, 0.14, 0.08) | 树干基础色 |

`EngineSettings` 用同名的 `vegetation*` 字段保存这些值，`syncSceneSettings()`
把它们写回；`engine.json` 的顶层 `vegetation` 节点与场景 v4 都按这 12 项存读。
命令行 `--vegetation-demo` 只负责创建/启用组件并摆一个取景位，不改变默认配置。

## 3. 放置规则

`VegetationGenerator::generate()` 是纯 CPU、确定性的：给定相机位置、参数和一个
“只查地形”的高度回调，返回两组 `Instance`。

**草**围绕相机铺一个圆盘：

- 抖动网格：单元大小 `spacing = 1 / sqrt(grassDensity)`，每格中心加两个 hash
  抖动，再做圆形距离剔除；相机附近也有草，不会出现空心。
- 对每个候选点调用 `Scene::sampleTerrainOnlyGroundHeightWorld()`；它只接受
  `TerrainComponent` 的地面，**不会长在体素 chunk 上**。
- 水面以下（`height < waterLevel + 0.05`）跳过；坡度用 ±0.75 m 四点高度差算，
  超过 **38°** 跳过。
- 每簇随机偏航、0.72–1.28 的高度、0.82–1.12 的颜色扰动。

**树**用更稀的网格：

- `spacing = sqrt(1000 / treeDensity)`，同样抖动 + 圆形剔除；
- 距相机小于 3 m 的候选跳过，避免树贴在镜头上；
- 水面以下（`waterLevel + 0.10`）跳过，坡度超过 **32°** 跳过；
- 树高随机 0.78–1.26，宽度 0.92–1.08，偏航随机。

实例上限是硬保护：草 **49152**、树 **4096**；达到上限会打印
`[Vegetation] warning: ... instance cap reached`。

## 4. 程序化网格

`buildMeshes()` 只建两份静态网格，所有实例共享：

| 网格 | 组成 | 顶点 / 索引 |
|---|---|---|
| 草 | 3 张绕 Y 每 60° 的竖直 quad，高 1、宽 1，UV.y 从根到梢；顶点色 r=0（根）~1（梢） | 12 / 18 |
| 树 | 6 棱柱树干（底半径 0.045、顶 0.028、高 0.56）+ 3 层 8 边锥形树冠（base/radius/top = 0.30/0.30/0.62、0.48/0.24/0.82、0.64/0.17/1.00） | 96 / 108 |

树干顶点色 r=0，树冠 r=1；顶点着色器用这个梯度决定风摆权重，片元用它区分
“树冠”和“树干”两套着色。草叶形状不是几何，而是片元按 UV 程序化切出来的
（见第 6 节）。

## 5. GPU 路径

`VegetationPass` 的构造参数是
`(Device&, RenderGraph&, RenderResources&, VkDescriptorSetLayout globalSetLayout, VkFormat depthFormat, Scene*)`；
`init()` 先建网格/实例缓冲，再建管线。

**缓冲**

- 静态顶点/索引：host-visible + mapped，`init()` 上传一次；
- 每帧槽位（`kFramesInFlight` 份）各一组草/树实例缓冲：
  `49152 × 48 B` 与 `4096 × 48 B`，每帧 `memcpy` + `vmaFlushAllocation`；
- 实例 48 B：`positionScale`(xyz=世界基点, w=高度缩放)、
  `rotationTint`(x=偏航, y=宽度缩放, z=风相位, w=颜色抖动)、`color`(albedo 乘色)。

**管线**

- 4 个颜色附件 = `GBufferFormat::Position/Normal/Albedo/Pbr`，1 个深度附件；
- 无混合；深度测试 `LESS_OR_EQUAL`、深度写 **开**、`cullMode = NONE`；
- 顶点输入 binding 0 = `Vertex`（44 B），binding 1 = 实例（48 B，`INSTANCE`）；
- 描述符 set0 = GlobalUbo；push constant 64 B：
  `timeWind`(x=时间, y=风强, z=模式 0 草/1 树)、`grassColor`、`treeColor`、`trunkColor`；
- 草、树各一次 `vkCmdDrawIndexed`，共 2 个 draw call。

**图注册**

`addToGraph()` 注册 `Vegetation`，四张颜色和深度全部 `LOAD`；execute 里若
`settings.vegetationEnabled == false` 直接返回，连实例上传都不做。

## 6. 着色

顶点着色器做实例变换、绕 Y 偏航和风摆：

- 草整簇摆，树只有树冠摆（权重乘 `max(顶点色.r, 0.15)`）；
- 摆幅随 `顶点色.r²` 增大，避免根部漂移；
- 相位由实例的 `rotationTint.z` 决定，位置项加一点世界坐标，整片不会同步摆动。

片元着色器写 G-Buffer 四输出：

- `outPosition = vec4(worldPos, 1)`；
- `outNormal = vec4(normal, 1)`：草把法线向 +Y 混 62%，树冠混 18%，让大片草/树冠
  不至于一半面全黑；
- `outAlbedo = vec4(albedo, 1)`；
- `outPBR = vec4(0, roughness, 0, 1)`：草 0.88、树 0.82，metallic 0。

草的形状：UV.x 分 5 根叶片，每根用 hash 决定高度、弯曲和宽度，
`blade = 1 - smoothstep(width * 0.55, width, |local + bend|)`；
`blade < 0.42` 的片元 `discard`，其余保留。树冠按 `normal.y` 提亮，
树干按 UV.y 做根到梢的明暗。

## 7. 重排与性能

放置是 CPU 同步完成的，不能每帧做。`updatePlacement()` 只在以下情况重排：

- 第一次放置；
- 相机水平移动超过 **6 m**；
- 12 个参数任意变化；
- 地形签名变化后**稳定 0.5 s**（流送期间 chunk 增删会让签名一直变，先等它稳定，
  避免每来一个 chunk 就把上万株草重新采样一遍）。

流送世界刚启动时会先看到空植被，等地形稳定后一次生成；日志会打印
`[Vegetation] placed | grass=... trees=... grassRadius=... treeRadius=... seed=...`。
正常 90 帧 demo 在 Debug 下约 27 FPS（草 10930、树 117，含地形流送）。

## 8. 已知边界

- 草/树**不投自身阴影**：ShadowPass 不包含植被；它们能接收地形阴影。
- 不进入 **PlanarReflection**：反射通道只重画普通几何，水面倒影里没有植被。
- 没有 LOD、没有 alpha-to-coverage、没有外部模型/贴图；草是 3 张交叉卡片，
  近看是程序化叶片，远景会显得偏平。
- 草/树的颜色是顶点色 × 组件颜色，没有季节/湿度/生物群系分布。
- 实例上限是硬上限（49152 / 4096），超过会截断并打 warning。
- 只长在 `TerrainComponent` 上；体素世界的地面不会自动长草/树。
- 放置是整批重排，参数拖动时可能有一次明显卡顿；当前没有分帧/异步放置。

## 9. 相关文件

- `src/Vegetation/VegetationGenerator.{h,cpp}`：纯 CPU 放置与网格；
- `src/Render/VegetationPass.{h,cpp}`：缓冲、管线、图注册、重排；
- `shaders/vegetation.vert/.frag`：实例变换、风摆、叶片 alpha-test、G-Buffer 输出；
- `src/Scene/SceneComponents.{h,cpp}`：`scene.vegetation` 的注册/同步/存读；
- `src/Scene/Scene.{h,cpp}`：`sampleTerrainOnlyGroundHeightWorld()`；
- `src/Core/FrameInfo.h`：`EngineSettings::vegetation*` 12 个字段。
