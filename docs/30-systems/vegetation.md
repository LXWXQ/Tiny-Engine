---
id: systems/vegetation
title: 地形植被
type: system
scope: src/Vegetation/, src/Render/VegetationPass.{h,cpp}, shaders/vegetation.vert, shaders/vegetation.frag, shaders/vegetation_shadow.vert
read-when: 改草/树的分布、外观、风摆、阴影，或排查植被不出现、长在水下/陡坡上、修改地形后不更新、帧率骤降
size: ~170 行
updated: 2026-11-13
---

# 地形植被

F6-07 给程序化地形加了一层**程序化**草与树：没有外部模型/贴图资产，全部在
`VegetationGenerator` 里生成网格和实例，由 `VegetationPass` 在
**Geometry 之后、SSAO 之前**以实例化方式写进同一套 G-Buffer。草和树能接收
SSAO、级联阴影和延迟光照；树通过独立的深度 pipeline 投入 CSM，草不投影，
用树根处的接触圆盘做视觉兜底；两者都不进入平面反射。

植被是**场景级组件** `scene.vegetation`，不是实体组件：它描述「这张场景的地表
植被」，挂在隐藏的场景设置实体上，随场景 v4 的 `sceneComponents` 存读；添加/
启用/调参/移除即功能开关，默认关闭（`EngineSettings::vegetationEnabled = false`）。

## 1. 为什么是独立 Pass

- 草/树是**实例化**的：当前一簇草 24 顶点、一棵树 217~1017 顶点，但实例有上万；
  走普通 `GeometrySystem` 会把它们塞进场景 ECS 的可见集合，既浪费 CPU 剔除，
  也不方便按地形变化整体重排。
- 草叶需要**逐片元 alpha-test**（`discard`），不能写进普通不透明 G-Buffer 管线；
  专用管线可以在片元里裁形状，同时保持不透明深度。
- 植被必须写 G-Buffer 才能被 SSAO/阴影/延迟光照正确着色；如果放在 Lighting 后
  做前向透明，会失去与地形一致的受光，也会被水面/玻璃的排序干扰。
- 放在 Geometry 后面可以用 `LOAD` 保留地形已写好的四张颜色和深度，只追加植被，
  不用重画地形；放在 SSAO 前，SSAO 能把草/树的接触阴影算进去。

管线顺序见 [pass-order](../20-pipeline/pass-order.md)：
`Geometry -> HiZBuild -> Vegetation -> SSAO -> Lighting -> ...`。

## 2. 组件与参数

`scene.vegetation` 有 12 个属性，Inspector 的 Add Scene Component 里分类为「地形」：

| 属性 | 默认 | 作用 |
|---|---|---|
| `enabled` | false（引擎默认）/ true（手动添加时） | 总开关；关闭时 Pass 早退，不放置、不绘制 |
| `grassDensity` | 0.8 | 每平方米草簇数；间距 = `1 / sqrt(density)` |
| `grassDistance` | 50 m | 草的放置半径（围绕相机） |
| `grassHeight` | 0.55 m | 草簇高度基数；实例再乘 0.72–1.28 的随机 |
| `treeDensity` | 1.2 | 每 1000 m² 树的数量；间距 = `sqrt(1000 / density)` |
| `treeDistance` | 120 m | 树的放置半径 |
| `treeHeight` | 7 m | 树高基数；实例再乘 0.74–1.26 的随机 |
| `windStrength` | 0.6 | 风摆幅度；0 时完全静止 |
| `seed` | 1337 | 确定性种子；换种子会换整片分布 |
| `grassColor` | (0.24, 0.36, 0.10) | 草的基础色，实例再乘 0.78–1.12 的亮度扰动 |
| `treeColor` | (0.12, 0.24, 0.08) | 树冠基础色 |
| `trunkColor` | (0.22, 0.14, 0.08) | 树干基础色 |

`EngineSettings` 用同名的 `vegetation*` 字段保存这些值，`syncSceneSettings()`
把它们写回；`engine.json` 的顶层 `vegetation` 节点与场景 v4 都按这 12 项存读。
命令行 `--vegetation-demo` 只负责创建/启用组件并摆一个取景位，不改变默认配置。

## 3. 放置规则

`VegetationGenerator::generate()` 是纯 CPU、确定性的：给定相机位置、参数和一个
「只查地形」的高度回调，返回两组 `Instance`。

**草**围绕相机铺一个圆盘：

- 抖动网格：单元大小 `spacing = 1 / sqrt(grassDensity)`，每格中心加两个 hash
  抖动，再做圆形距离剔除；相机附近也有草，不会出现空心。
- 每个候选点通过 `TerrainHeightSampler` 查高度；它只接受 `TerrainComponent`
  的地面，**不会长在体素 chunk 上**。
- 水面以下（`height < waterLevel + 0.05`）跳过。
- 不再逐株做四向坡度查询；统一把根部下沉 `height -= 0.11f`，实例再
  `positionScale.y = height - 0.015f`。这是性能与贴地之间的折中，极陡坡仍可能
  露出局部缝隙。
- 每簇随机偏航、0.72–1.28 的高度、0.78–1.12 的颜色扰动。

**树**用更稀的网格：

- `spacing = sqrt(1000 / treeDensity)`，同样抖动 + 圆形剔除；
- 距相机小于 3 m 的候选跳过，避免树贴在镜头上；
- 水面以下（`waterLevel + 0.10`）跳过；
- 坡度仍按中心点 ±0.75 m 四点高度差估算，超过 **32°** 跳过；
- 树根高度取中心 + 四个 0.45 m 邻点的**最低值**再 `-0.04`，降低坡地悬空；
- 树高随机 0.74–1.26，宽度 0.88–1.12，偏航随机；
- `hash01(...) < 0.38` 的树是阔叶，写入 `Instance.color.a`；其余是针叶。

实例上限是硬保护：草 **49152**、树 **4096**；达到上限会打印
`[Vegetation] warning: ... instance cap reached`。

## 4. 程序化网格

`buildMeshes()` 建三份静态网格，所有实例共享：

| 网格 | 组成 | 顶点 / 索引 |
|---|---|---|
| 草 | 6 张绕 Y 每 60° 的竖直 quad，每张沿自身法线偏移 0.11，高 1、宽 1，UV.y 从根到梢 | 24 / 36 |
| 针叶树 | 6 棱柱树干（底半径 0.045、顶 0.028、高 0.56）+ 6 层 10 边不规则锥形树冠（base/radius/top = 0.14/0.39/0.40、0.26/0.35/0.56、0.38/0.30/0.72、0.50/0.25/0.86、0.62/0.19/0.95、0.72/0.13/1.00）+ 接触阴影圆盘 | 217 / 252 |
| 阔叶树 | 6 棱柱树干 + 7 个 10 边 × 4 环低模椭球团簇 + 接触阴影圆盘 | 1017 / 1332 |

顶点色约定：

- `color.r` = 树冠遮罩（树干/草/圆盘为 0，树冠为 1）；
- `color.g` = 垂直明暗层（0~1，供片元做树冠层次）；
- `color.b` = 接触阴影圆盘标记（1 = 圆盘）；
- 实例 `color.rgb` 是 albedo 乘色，`color.a` 是树类型（0 针叶 / 1 阔叶，草忽略）。

接触圆盘是 12 边三角扇：针叶半径 0.26、阔叶 0.32，y=0.012，rim UV 用径向坐标；
它跟随树实例变换，在坡地上大致贴地，不是真实方向光阴影。

## 5. GPU 路径

`VegetationPass` 的构造参数是
`(Device&, RenderGraph&, RenderResources&, VkDescriptorSetLayout globalSetLayout, VkFormat depthFormat, Scene*)`；
`init()` 先建网格/实例缓冲，再建可见管线与阴影管线。

**缓冲**

- 静态顶点/索引：host-visible + mapped，`init()` 上传一次；
- 每帧槽位（`kFramesInFlight` 份）各一组草/树实例缓冲：
  `49152 × 48 B` 与 `4096 × 48 B`，每帧 `memcpy` + `vmaFlushAllocation`；
- 实例 48 B：`positionScale`(xyz=世界基点, w=高度缩放)、
  `rotationTint`(x=偏航, y=宽度缩放, z=风相位, w=颜色抖动)、`color`(albedo 乘色 + 类型)。

**可见管线**

- 4 个颜色附件 = `GBufferFormat::Position/Normal/Albedo/Pbr`，1 个深度附件；
- 无混合；深度测试 `LESS_OR_EQUAL`、深度写 **开**、`cullMode = NONE`；
- 顶点输入 binding 0 = `Vertex`（44 B），binding 1 = 实例（48 B，`INSTANCE`）；
- 描述符 set0 = GlobalUbo；push constant 64 B：
  `timeWind`(x=时间, y=风强, z=模式 0 草/1 树)、`grassColor`、`treeColor`、`trunkColor`；
- 草、针叶、阔叶各一次 `vkCmdDrawIndexed`，共 **3 个 draw call**；阔叶用
  `firstInstance = coniferCount` 复用同一份树实例缓冲。

**阴影管线**

- `VegetationPass::prepareShadow(frameInfo, frame)` 更新放置并上传树实例；
- `VegetationPass::recordShadow(cmd, lightVP, frame, drawCalls, triangles)` 只画树，
  针叶/阔叶各一次；push constant 80 B（lightVP + timeWind）；
- `ShadowPass` 在每级 cascade 画完普通网格后调用它；草不投影。

**图注册**

`addToGraph()` 注册 `Vegetation`，四张颜色和深度全部 `LOAD`；execute 里若
`settings.vegetationEnabled == false` 直接返回，连实例上传都不做。

## 6. 着色

顶点着色器做实例变换、绕 Y 偏航和风摆：

- 草整簇摆；树冠摆，树干权重为 0（`weight *= max(顶点色.r, 0.0)`）；
- 摆幅随 `顶点色.r²` 增大，避免根部漂移；
- 树的摆幅乘 1.45，相位由实例的 `rotationTint.z` 决定，位置项加一点世界坐标，
  整片不会同步摆动。

片元着色器写 G-Buffer 四输出：

- `outPosition = vec4(worldPos, 1)`；
- `outNormal = vec4(normal, 1)`：草把法线向 +Y 混 62%，树冠混 30%，让大片草/树冠
  不至于一半面全黑；
- `outAlbedo = vec4(albedo, 1)`；
- `outPBR = vec4(0, roughness, 0, 1)`：草 0.88、树 0.80，metallic 0。

草的形状：每张卡片 UV.x 分 **4 根**叶片，宽度从根到梢收窄，hash 决定高度、
弯曲和 RGB 差异；`blade` 低于阈值 0.28 的片元 `discard`，根部额外压暗。
树冠用 `fragLayer` 做垂直明暗，加世界坐标 hash 的细颗粒；树干用 `trunkColor`。
接触圆盘在树分支最前判定，输出深绿灰 albedo 与朝上的法线。

## 7. 重排与性能

放置是 CPU 同步完成的，不能每帧做。`updatePlacement()` 在以下情况重排：

- 第一次放置；
- 相机水平移动超过 **6 m**；
- 12 个参数任意变化；
- 地形签名变化：先等 `terrainStableSeconds >= 0.15s`；如果签名持续变化（流送/
  连续雕刻），`sinceLastPlacement >= 0.5s` 时强制刷新，避免永远等不到稳定。

地形高度查询由 `TerrainHeightSampler` 缓存：

- 构造时快照所有 `TerrainComponent + WorldTransformComponent` 的 world/inverse/
  halfExtent/HeightField；
- 用 **32 m 世界网格** 建 `Surface` 索引，查询只检查覆盖该格的 1~4 个 chunk；
- 不再每点遍历 registry、不再重复 `glm::inverse`。

实测（Debug，`--empty-scene --vegetation-demo --streaming-world 3`）：

- 旧实现首次放置 **1416.1 ms**；
- 采样器 + 空间索引后，仍有逐株坡度查询时约 39~42 ms；
- 移除草的逐株四向坡度查询后降到 **8.8~11.1 ms**；主场景 7782 草约 5.5~6.7 ms；
- 最终 demo 约 128 FPS、239k triangles、10,930 草、117 棵树（74 针叶 / 43 阔叶）。

日志会打印
`[Vegetation] placed | grass=... trees=... grassRadius=... treeRadius=... seed=...`
与 `regenerated` 计数。

## 8. 已知边界

- 树通过 `recordShadow()` 投 CSM；草不投影，树根接触圆盘是近似兜底。
- 不进入 **PlanarReflection**：反射通道只重画普通几何，水面倒影里没有植被。
- 没有 LOD、没有 alpha-to-coverage、没有外部模型/贴图；草是 6 张交叉卡片，
  近看是程序化叶片，远景仍偏平。
- 草/树的颜色是顶点色 × 组件颜色，没有季节/湿度/生物群系分布。
- 实例上限是硬上限（49152 / 4096），超过会截断并打 warning。
- 只长在 `TerrainComponent` 上；体素世界的地面不会自动长草/树。
- 放置是整批重排，仍有约 9 ms 单帧峰值；当前没有分帧/异步放置。
- 草的贴地是统一下沉 + 邻点最低值近似，极陡坡可能仍有局部悬空/穿插。

## 9. 相关文件

- `src/Vegetation/VegetationGenerator.{h,cpp}`：纯 CPU 放置与三套网格；
- `src/Render/VegetationPass.{h,cpp}`：缓冲、可见/阴影管线、图注册、重排、地形采样缓存；
- `shaders/vegetation.vert/.frag`：实例变换、风摆、叶片 alpha-test、G-Buffer 输出；
- `shaders/vegetation_shadow.vert`：树实例的 CSM 深度 pass；
- `src/Render/ShadowPass.{h,cpp}`：调用 `prepareShadow`/`recordShadow`；
- `src/Scene/SceneComponents.{h,cpp}`：`scene.vegetation` 的注册/同步/存读；
- `src/Scene/Scene.{h,cpp}`：`sampleTerrainOnlyGroundHeightWorld()`；
- `src/Core/FrameInfo.h`：`EngineSettings::vegetation*` 12 个字段。
