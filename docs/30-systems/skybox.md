---
id: systems/skybox
title: 天空：2D 环境贴图与 3D 几何天空
type: system
scope: src/Scene/SkyCamera.*, src/Scene/Components.h, src/Render/GeometrySystem.cpp, shaders/skybox_mrt.*, shaders/sky_geometry.*
read-when: 远处景物要怎么画？天空盒和场景几何的关系是什么？无限远的东西为什么还能有视差？
size: ~120 行
updated: 2026-11-17
---

# 天空：2D 环境贴图与 3D 几何天空

引擎里有两套"天空"，它们互斥：同时启用时 2D 天空盒会让位给 3D 几何天空。

| | 2D 天空盒（默认） | 3D 几何天空（SkyCamera） |
| --- | --- | --- |
| 几何 | 一个球体网格实体（`RenderTagComponent::isSkybox`） | 任意实体（`RenderTagComponent::isSkyGeometry`），照 Source 的 `sky_camera` 做 |
| 位置 | 顶点着色器里 `mat3(model) * pos + cameraPos`，永远跟相机走 | 整块几何按 `1/scale` 微缩摆在 `origin`，渲染时放大回原尺寸并平移到相机 |
| 深度 | `gl_Position.xyww` 钉在远平面 | 同样钉在远平面（`clip.z = clip.w * 0.999999`） |
| 视差 | 没有（纯背景） | 有（相机走动时远处山体会缓慢移动） |
| 光照 | 不参与（写 `outNormal = 0` 作天空标记） | 参与（写真实法线，走延迟光照） |
| 着色器 | `skybox_mrt.vert/.frag` | `sky_geometry.vert/.frag` |

## 为什么需要 3D 天空

2D 天空盒是一张等距柱状环境贴图，画面上永远是同一张图：走到哪、太阳怎么转，远景都不变。
把远景做成真实几何、整体微缩后锁在相机上，既保留"无限远"的观感，又让远景接受阳光、雾和反射，
而且美术可以直接摆模型，不需要再烘一张全景图。

## SkyCameraComponent

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `enabled` | `true` | 关掉就退回 2D 天空盒 |
| `origin` | `(0, 0, 0)` | 远景几何在"缩微空间"里的原点（世界坐标） |
| `scale` | `0.0625` | 缩微比例，1/16 表示远景画成真实尺寸的十六分之一 |

矩阵（`src/Scene/SkyCamera.cpp`）：

```text
skyCameraMatrix = translate(cameraPos) * scale(1/scale) * translate(-origin)
skyEntityMatrix = skyCameraMatrix * worldMatrix
```

顺序不能换：先把几何搬到 `origin` 附近、再放大 `1/scale`、最后整体平移到相机位置。
换任何一个顺序，远景都会在玩家走动时漂移或者塌到原点。

## 渲染流程

1. 每帧在 `GeometrySystem` 主循环前取第一个 `SkyCameraComponent`（没有就用默认值，`hasSkyCamera = false`）。
2. 循环里 `isSkyGeometry` 的实体走独立的 `skyGeometryPipeline`（与天空盒同配置：无背面剔除、不写深度、`LESS_OR_EQUAL`）。
3. `entityMatrix` 换成 `skyEntityMatrix(settings, cameraPosition, worldTransform->matrix)`，法线矩阵重新算，材质标记强制为 0。
4. `hasSkyCamera` 为真时 `isSkybox` 实体直接 `continue` —— 两套天空都画在远平面、都不写深度，谁后画谁盖住对方。
5. 天空几何不进程空间索引、不进视锥剔除（`Scene.cpp`）、不参与主几何批处理与阴影（`SceneGpuBatch.cpp`、`ShadowPass.cpp`）。

## 两个着色器坑（都踩过）

1. **零法线会得到 NaN**。顶点法线缺失时 `normalize(vec3(0))` 是 NaN，而 `NaN < 1e-12` 为假，
   几何会在光照阶段整块消失、画面上只剩天空。判断要写成 `if (!(dot(n, n) > 1e-12))`。
2. **不要用顶点色当基色**。程序化远景的顶点色通道可能是未初始化数据（同样出 NaN），
   现在按世界高度做地面到高空的渐变。

另外，如果基色恰好接近天空色，几何画出来了也看不出来 —— 调试时先换成洋红常量色确认管线通，
再调颜色。

## 命令

```powershell
.\build\Debug\TinyEngine.exe --sky3d-test
```

稳定输出：`[Sky] cameraSelfTest PASS | cases=6 scale=1/16 parallax=locked`。
`Validate.ps1` 里的 `m1-d3b-sky3d` 用例断言这一行。

## 已知限制

- 还没有真实远景资产：目前的 3D 天空用的是程序化球体，观感上是一条地平线。
- 编辑器还没做 `isSkyGeometry` 与 `SkyCameraComponent` 的 JSON 持久化，只能在代码里建。
- 天空几何不参与阴影、反射与 SSAO（它们走主 G-Buffer 的深度），只保证光照与雾正确。
- `scale` 太小（< 0.001）时精度会明显下降，组件面板把下限卡在 0.001。
