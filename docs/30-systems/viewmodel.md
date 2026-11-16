---
id: systems/viewmodel
title: 第一人称手持物（Viewmodel）
type: system
scope: src/Scene/ViewModel.*, src/Render/ViewModelPass.*, shaders/viewmodel.*, src/Scene/Components.h
read-when: 手持物/武器显示不对、贴墙被裁、要换模型或调视野时
size: ~120 行
updated: 2026-11-16
---

# 第一人称手持物（Viewmodel）

## 1. 为什么需要独立通道

场景走延迟管线：几何写 G-Buffer，光照读 G-Buffer 写 `hdrScene`。手持物如果和场景共用同一个深度缓冲和近平面，会出现两个问题：

- 贴着墙站时，墙的深度比武器近，武器被裁掉半截；
- 场景近平面为了不裁地形通常设得较大（0.1m），武器离相机只有几十厘米，很容易被裁。

所以 viewmodel 用**自己的投影矩阵和自己的深度范围**，并且在所有场景内容之后、TAA 之前画到 `hdrScene` 上。它不写场景深度，也不参与阴影/SSAO/反射。

## 2. 数据

`ViewModelComponent`（`src/Scene/Components.h`，编辑器里叫 `viewmodel.camera`，分类"渲染"）：

| 字段 | 含义 |
|---|---|
| `visible` | 关掉后整帧不画 |
| `fovDegrees` | 手持物自己的视场角，默认 54（比场景略窄，显得更近） |
| `offset` | 相机空间位置（米，**+z 朝前**），默认 `(0.28, -0.24, 0.55)` |
| `rotation` | 欧拉角（度，Y→X→Z），默认 `(0, -14, 0)` |
| `scale` | 缩放，默认 `0.055` |
| `color` | 基色，与光照相乘，默认 `(0.42, 0.44, 0.48)` |

注意引擎的视图空间是 **+z 朝前**（见 `Camera::setViewYXZ`，w 行是前方向），与 GLM 教材里的 -z 约定相反。写死 -z 会让整套矩阵算出 `ndc.z ∈ [1,2]`，武器直接不可见。

## 3. 数学

`src/Scene/ViewModel.h`：

- `viewModelProjection(fov, aspect, near, far)`：与 `VulkanCamera::setPerspectiveProjection` 完全一致的 Vulkan 约定（`[1][1] *= -1`），默认 near 0.01 / far 10。
- `viewModelOffsetMatrix(settings)`：`translate * rotateY * rotateX * rotateZ * scale`，输入非有限或缩放含 0 时回退默认值。

`viewModelProjection` 与场景投影是两套：**不要把场景投影矩阵直接拿来用**，否则近平面又回到 0.1m。

## 4. 渲染

`src/Render/ViewModelPass.*`：

1. 遍历 `registry.view<ViewModelComponent>()`，取第一个 `visible` 的组件；
2. `modelView = viewModelOffsetMatrix(settings)`（**只这一项**，不要再乘 `viewMatrix`——偏移矩阵已经是相机空间，多乘一次会被当成世界坐标再变换一遍，武器会跑到画面外）；
3. `projection = viewModelProjection(fov, aspect, near, far)`，`aspect` 从场景投影的 `|P11/P00|` 反推；
4. `graph.addGraphicsPass("ViewModel")` + `writeColor(hdrScene, LOAD)`，**不写深度**（`depthTestEnable = depthWriteEnable = VK_FALSE`）；
5. 几何是 Pass 自带的程序化立方体（24 顶点 / 36 索引，host-visible 常驻映射），不加载 glTF——`Model` 的构造需要 `VkCommandPool`，而 `Device` 不暴露它。

着色器：`shaders/viewmodel.vert/.frag`，push constant 160B（`modelView` + `projection` + `baseColor` + `lightDirView`），片元是 Lambert + 轻微边缘压暗。管线不带任何 descriptor set。

首次绘制时打印一次：

```
[ViewModel] 通道就绪 | fov=54 aspect=1.77778 draws=1 depth=0.01..10
```

## 5. 自检

```
build\Debug\TinyEngine.exe --viewmodel-test
```

`[ViewModel] selfTest PASS | cases=6 fov=54 depth=0.01..10 aspect=ok`，6 组用例：近/远平面映射到 0/1、深度随距离递增、窄 fov 放大 |ndc.x|、宽 aspect 缩小 |ndc.x|、TRS 顺序（含绕 Y 90° 的轴交换）、退化输入保持有限。

Validate 里对应 `m1-d3-viewmodel`（不需要 Vulkan）。

## 6. 手动验证

自检路径的场景是程序化构建的，没有 viewmodel 实体，所以 `--selftest` 不会画手持物。要看效果：

1. 打开编辑器，选中任意实体 `+ Add Component → Viewmodel`；
2. 或者临时在代码里 `registry.emplace<ViewModelComponent>(entity)` 再 `--selftest N --screenshot build\vm.png --screenshot-frame 4`。

已验证：组件存在时 `draws=1`、画面右下角出现方块、`[Validation] errors=0 warnings=0`；把角色贴到墙边时方块不会被墙裁掉（独立深度）。

## 7. 已知限制与后续（D3b）

- 只有程序化立方体，没有真正的武器资产（需要先给 `Model` 暴露 command pool 或改用预构建顶点缓冲）；
- 只有一次绘制，没有骨骼动画、没有摆动/后坐力；
- 不参与阴影与反射，也没有单独的曝光/色调映射；
- 3D 天空盒（计划里与 viewmodel 同批）**没有做**：天空盒目前仍走 geometry 阶段写 G-Buffer。