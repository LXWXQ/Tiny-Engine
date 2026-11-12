---
id: systems/fog-volume
title: 局部体积雾（Fog Volume）
type: system
scope: src/Render/FogVolumePass.*, src/Scene/FeatureComponents.*, src/Editor/panels/InspectorPanel.cpp, shaders/fog_volume.frag
read-when: 在场景里摆放局部雾、调雾的位置/体积/浓度/高度/噪声/风，或修改体积雾 Pass
size: ~180 行
updated: 2026-11-12
---

# 局部体积雾（Fog Volume）

## 这篇解决什么

场景里需要一团**可以摆放**的雾：山谷的晨雾、林间的薄雾、洞口涌出的冷气，而不是全屏统一的距离雾。距离雾（`scene.fog`）跟着相机做全局指数衰减，无法只覆盖一块区域，也无法做高度分层和风吹流动。

体积雾是一个**实体组件** `effects.fogVolume`：把它挂到任意有 Transform 的实体上，组件自带的 `center`/`size`/`rotation` 定义局部雾盒，实体的 WorldTransform 再整体移动/旋转/缩放这个盒子，雾只在盒子里累积。一个场景最多同时渲染 8 个雾盒。

入口：选中一个有 Transform 的实体 → `+ Add Component` → “体积效果”分类 → “体积雾 (Fog Volume)”。如果选中的是地形/流送地形/体素对象，Inspector 会提示先点“新建空实体并选中”，再到新实体上添加；弹窗会列出全部组件并给已挂载项打勾，不会出现空列表。

组件在 Inspector 里有专用 **Fog Volume** 面板（不是通用属性列表）：Placement 直接调中心、尺寸、旋转，并提供 20×8×20 / 40×12×40 / 80×20×80 三个尺寸预设；Appearance / Lighting / Quality 分组调浓度、颜色、噪声、风、相函数和步数。视口里用青色线框显示实际盒体。

它和 `scene.fog` 是两套东西，可以同时开：`scene.fog` 负责远处整体空气感，`effects.fogVolume` 负责近处可摆放的局部雾团。

## 事实

### 组件参数

| 属性 | 默认值 | 范围 | 说明 |
|---|---|---|---|
| `enabled` | true | — | 关闭后该雾盒不参与渲染 |
| `center` | (0, 0, 0) | — | 盒心相对实体的局部坐标（m） |
| `size` | (20, 8, 20) | 0.05 ~ 2000 | 盒体**全尺寸**（m），不是半尺寸 |
| `rotation` | (0, 0, 0) | -360 ~ 360 | 盒体局部旋转（度），顺序与 Transform 一致 |
| `density` | 0.02 | 0 ~ 0.2 | 基础消光系数（1/m）；越大越浓 |
| `color` | (0.78, 0.84, 0.92, 1) | — | 散射色调，不是"雾的颜色"而是被照亮后散射出来的颜色 |
| `heightFalloff` | 0.08 | 0 ~ 0.5 | 高于盒心后的高度衰减（1/m）；0 表示整盒均匀 |
| `noiseStrength` | 0.55 | 0 ~ 1 | 3D 噪声对密度的调制强度；0 为均匀雾 |
| `noiseScale` | 0.08 | 0.005 ~ 0.5 | 噪声采样频率（1/m）；越大雾团越碎 |
| `wind` | (0.35, 0, 0.12) | — | 噪声滚动速度（m/s），只动噪声不动盒体 |
| `edgeSoftness` | 0.18 | 0.01 ~ 0.5 | 盒体边缘淡出宽度（局部空间 0~0.5） |
| `anisotropy` | 0.55 | -0.9 ~ 0.9 | Henyey-Greenstein 相函数 g；正值为前向散射（逆光更亮） |
| `ambient` | 0.35 | 0 ~ 2 | 环境散射强度；越大阴影面越不黑 |
| `steps` | 12 | 1 ~ 32 | 每个雾盒的光线步进次数；性能与质量的主要开关 |

### 摆放规则

| 项 | 规则 |
|---|---|
| 局部盒体 | `T(center) * R(rotation) * S(size)`；`size` 是**全尺寸**，例如 (30, 10, 36) 表示 30m × 10m × 36m |
| 最终盒体 | 实体 WorldTransform × 局部盒体；移动实体会整体移动雾盒，实体缩放会再乘到尺寸上 |
| 高度基准 | `params1.x = volumeWorld[3][1]`，即最终盒心的世界 Y；`heightFalloff` 只作用于盒心以上 |
| 无效实体 | 没有 `WorldTransformComponent`、`world.valid=false`、`density<=0`、`steps<=0` 或变换行列式接近 0 的实体直接跳过 |
| 数量上限 | 8 个；超过的部分不进入 GPU 缓冲，日志只报告实际提交数 |
| 地形/体素 | 地形、流送地形、体素世界/chunk 由专用面板管理，`canAddComponent` 会拒绝 `effects.*`；需要雾盒时先创建空实体 |

### 渲染路径

| 项 | 值 | 证据 |
|---|---|---|
| 插入位置 | Lighting 之后、PlanarReflection / SSR / Glass / Water 之前 | `Application.cpp` 中 `fogVolumePass` 在 `cloudPass` 之后加入 |
| 输入 | `gbPosition`（世界坐标）+ GlobalUbo + 32³ 3D 噪声 | `FogVolumePass::createDescriptorSets` |
| 输出 | 直接混合到 HDR 场景，无独立附件、无深度附件 | 管线 `kHdrSceneFormat`，depthTest 关闭 |
| 混合 | 预乘 alpha：颜色 `ONE / ONE_MINUS_SRC_ALPHA`，alpha `ZERO / ONE` | `FogVolumePass::createPipeline` |
| 深度 | 不写深度，也不进 G-Buffer | 全屏三角形，只读 `gbPosition` |
| 遮挡 | 用 `gbPosition` 把步进截到最近不透明表面之前（最多 500m） | `fog_volume.frag` 的 `sceneDistance` |
| 噪声 | CPU 生成 32³ RGBA8 可平铺 value-noise：R 低频团、G 中频、B 高频、A=255 | `FogVolumePass::createNoiseTexture` |
| 描述符 | set0 GlobalUbo；set1 binding0 雾盒 UBO、binding1 sampler3D、binding2 sampler2D gbPosition | `FogVolumePass.h` |
| 每帧上传 | `VolumeUbo{vec4 header; GpuVolume[8];}`，1552 B | `static_assert` 锁尺寸 |

### 着色模型

片元着色器对每个雾盒做一次世界空间 slab 相交，然后在盒内步进：

1. **边缘淡出**：`edge = min(0.5 - |localP.xyz|)`，`smoothstep(0, edgeSoftness, edge)`，避免看到硬切的立方体边界。
2. **高度衰减**：`exp(-heightFalloff * max(p.y - centerY, 0))`，只让雾贴地/贴水面聚集。
3. **噪声形状**：`nv = 0.70*R + 0.30*G`，`smoothstep(0.22, 0.78, nv)` 得到絮状团块，再按 `noiseStrength` 在均匀与絮状之间插值；额外乘一层高频 B 通道细节。
4. **散射颜色**：体积色 ×（太阳色 × 相函数 × 0.55 + 高度渐变环境色 × ambient × 0.90）。
5. **累积**：每步 `alpha = 1 - exp(-density*stepSize)`，`scattered += transmittance*alpha*stepColor`；透射率低于 0.02 提前退出。
6. **输出**：`vec4(scattered, 1 - transmittance)`，由预乘 alpha 混合叠到 HDR 上。

步进起点用交错梯度噪声抖动，避免固定层状条带；每个雾盒用不同的抖动偏移，叠放时不容易出现同相位条纹。

## 为什么这样（取舍）

**为什么是盒体而不是球体/任意 SDF？** 盒体用 `center`/`size`/`rotation` 就能直观摆放和拉伸，slab 相交只有 6 次除法，CPU/GPU 都便宜；球体不适合做长条山谷雾，SDF 则要额外资产和烘焙。代价是雾的形状永远是长方体，靠噪声和边缘淡出掩盖硬边。

**为什么读 G-Buffer 截断射线？** 全屏体积雾最容易犯的错是"雾穿过墙/地形"。用 `gbPosition` 把步进截到最近不透明表面之前，雾就会被地形、建筑、角色正确遮挡，而不需要额外的深度预渲染。天空没有有效位置（`dot(scenePos,scenePos)<=1e-6`）时用逆 VP 还原远平面方向，雾仍能铺到天际线。

**为什么最多 8 个、步数上限 32？** 每个像素对每个雾盒都要步进，成本约 `可见盒数 × steps`。8 × 32 已经是每像素 256 次循环的量级；默认 12 步在 1080p 下足够柔和，需要更细腻的贴脸雾再往上调。

**为什么噪声是 CPU 生成的 32³？** 体积雾需要可平铺的 3D 噪声，仓库里没有 3D 纹理资产管线；CPU 生成 32³ RGBA8 只有 128 KB，启动时一次上传，避免引入依赖。代价是频率固定，`noiseScale` 调得过大时会看到体素感。

**为什么插在 Cloud 之后、反射之前？** 这样水面、玻璃、平面反射都能读到带雾的 HDR，雾不会只贴在最终画面上；同时雾在云之后，云不会被雾盒错误地"包住"。PlanarReflection 内部第二套 Lighting 不含雾，这是已知边界。

**为什么是实体组件而不是场景级组件？** 场景级组件（`scene.*`）全场景唯一，适合环境/阴影/云这类全局设置；局部雾需要多个实例、各自变换，实体组件天然支持"一个场景摆很多个雾盒"。

## 改动时要注意

- **修改 GPU 布局必须同步三处**：`FogVolumePass.h` 的 `PushConstants/GpuVolume/VolumeUbo`、`static_assert`、`fog_volume.frag` 的对应结构体；错位不会编译报错，只会画面错乱。
- **新增参数要同时改**：`FogVolumeComponent` 字段、`FeatureComponents.cpp` 的 `addComponentDescriptor`、`FogVolumePass::addToGraph` 的打包、shader 使用点、`Scene.cpp` 的自检属性计数。
- **resize 必须重绑 `gbPosition`**：G-Buffer 在窗口尺寸变化时会重建视图，`FogVolumePass::resize()` 要重写 set1 binding2，否则 resize 后读到已销毁的 view。
- **实体必须有 Transform**：雾盒依赖 `WorldTransformComponent`，只加 `effects.fogVolume` 不加 Transform 的实体会被 `canAddComponent` 拒绝；已挂载但 Transform 被移除的旧场景实体不会出现在 `volumes` 计数里。
- **size 不是半尺寸**：Inspector 里把 size.x 从 20 改到 30，盒体是 30m 宽，不是 15m。
- **位置/体积改组件，不要只改 Transform**：`center`/`size`/`rotation` 是雾盒自身的局部 placement；Transform 的 position/rotation/scale 是整体变换。只改 Transform 的 scale 会把整个实体（包括子物体）一起缩放。
- **雾不影响阴影/SSAO/反射**：它只混合 HDR 颜色，不写深度、不进 G-Buffer；需要雾在阴影里也可见时调 `ambient`。
- **默认不添加**：新建场景没有雾盒，只有显式添加 `effects.fogVolume` 或使用 `--fog-demo` 才生效。
- **Render 窗口只做快捷开关**：场景组件的启用/添加/移除可以在 Render 窗口完成，详细参数仍在 Inspector 的 Fog Volume 面板编辑；两边写的是同一份 `ComponentRecord`，不会出现两套值。

## 相关

- [Pass 顺序](../20-pipeline/pass-order.md)
- [着色器清单](../20-pipeline/shaders.md)
- [场景与 ECS](scene-ecs.md)
- [场景序列化](../90-editor/serialization.md)
- [已知边界](../70-status/known-limits.md)
- [测试矩阵](../50-validation/test-matrix.md)
