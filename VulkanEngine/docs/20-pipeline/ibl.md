---
id: pipeline/ibl
title: IBL split-sum 烘焙
type: pipeline
scope: src/Render/IBLBaker.h, src/Render/IBLBaker.cpp, src/RHI/CubemapTexture.cpp, shaders/ibl_*.frag
read-when: 修改环境光照、cubemap 处理，或调整 IBL 质量
size: ~130 行
updated: 2026-10-30
---

# IBL split-sum 烘焙

## 1. 三步

初始化时一次性在 GPU 上烘焙，运行时只做采样。

| 步骤 | 输入 | 输出 | 尺寸 | shader |
|---|---|---|---|---|
| 1. 等距柱状 -> cubemap | `environment.hdr` (2048x1024) | `envCube` | 256x256, 9 mips | `ibl_equirect_to_cube.frag` |
| 2. 漫反射 irradiance | `envCube` | `irradiance` | 32x32, 1 mip | `ibl_irradiance.frag` |
| 3. 镜面 prefilter | `envCube` | `prefiltered` | 128x128, 5 mips | `ibl_prefilter.frag` |
| 4. BRDF LUT | 无（纯解析积分） | `brdfLut` | 256x256 | `ibl_brdf_lut.frag` |

对应输出日志：

```
[Cubemap] envCube  256x256   mips 9
[Cubemap] irradiance  32x32   mips 1
[Cubemap] prefiltered  128x128   mips 5
[IBL] 烘焙完成（irradiance / prefiltered / brdfLut）
```

## 2. 为什么 split-sum

漫反射的半球卷积是几万次采样/纹素，运行时做不可能，所以离线烘焙。

镜面部分老实现是 `envSpecular *= (1.0 - roughness)`，**能量完全不守恒**
（粗粗糙度时高光直接消失）。split-sum 把镜面积分拆成两个可预计算项：

```
∫ L(l) * f(l,v) * (n·l) dl  ≈  prefiltered(R, roughness) * (F0 * A + B)
                              ^^^^^^^^^^^^^^^^^^^^^^^^^^    ^^^^^^^^^^^
                              步骤 3 的 cubemap            步骤 4 的 BRDF LUT
```

运行时只有 2 次采样。**实测过曝像素从 95.9% 降到 7.1%。**

## 3. 实现要点

- 使用 Vulkan 1.3 动态渲染（`vkCmdBeginRendering`），不建 `VkRenderPass`/`VkFramebuffer`。
- 每个 cubemap 面的渲染命令都在**单次提交**里完成，`cmdEndSingleTime()` 内部
  `vkQueueWaitIdle` 后才销毁引用的资源（见
  [resource-lifetime](../10-architecture/resource-lifetime.md) §4.2）。
- **不走渲染图**，所以必须自己调 `cmdSetFullViewportAndScissor()`
  （见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 6）。
- 结果作为 `persistent` 资源整帧只读，因此被 W2 语义校验豁免
  （见 [rendergraph](../10-architecture/rendergraph.md) §5）。

## 4. 采样器归属

三个采样器由 `IBLBaker` / `CubemapTexture` 持有，写进 Set0 的 binding 7/8/9。
渲染图不管采样器（见 [decisions](../10-architecture/decisions.md) 决策 4），
因此 `buildGlobalSet()` 必须在 IBL 烘焙之后调用。

## 5. 改动注意

- 改 `envCube` 分辨率要同步 `prefiltered` 的 mip 级数（当前 5 级）。
- 改格式要检查 `lighting.frag` 里的 `samplerCube` 采样。
- 烘焙是一次性的，改完必须重启进程才能看到效果（没有热重载）。
