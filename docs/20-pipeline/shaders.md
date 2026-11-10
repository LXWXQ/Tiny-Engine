---
id: pipeline/shaders
title: Shader 清单与编译管线
type: pipeline
scope: shaders/, CMakeLists.txt 的 shader 编译段
read-when: 增删 shader、编译报错、改 GLSL 版本或编译选项
size: ~130 行
updated: 2026-11-10
---

# Shader 清单与编译管线

## 1. 清单（42 个文件）

| 文件 | 阶段 | 用途 |
|---|---|---|
| `mrt.vert` / `mrt.frag` | vert/frag | 普通网格 / 地形写 G-Buffer；`terrainMode` 分支采样当前地形材质集 Set2（4 个描述符集，按 `materialSetIndex` 重绑）的四层 albedo/normal/packed heightRoughness，叠加坡度权重、Surface Style、宏观噪声、切线空间法线，并在 LOD 过渡帧用 IGN 抖动 mask 混合两级 |
| `glass.vert` / `glass.frag` | vert/frag | 体素玻璃前向着色；采样图集、输出 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`，不写深度 |
| `water.vert` / `water.frag` | vert/frag | 水面前向混合；相机中心径向网格、4 个大波位移、解析法线、环境反射、厚度吸收与泡沫 |
| `particle.vert` / `particle.frag` | vert/frag | 粒子 billboard：6 顶点四边形按相机 right/up 展开；实例带 materialId，frag 从 set1 bindless 数组采 sprite，0xFFFFFFFF 时回退程序化圆形软遮罩；前向 alpha 混合 |
| `ssr.frag` | frag | 半分辨率屏幕空间反射：读 HDR + G-Buffer 世界位置，48 步追踪后用命中置信度与平面反射/IBL 混合 |
| `grid_mrt.vert` / `grid_mrt.frag` | vert/frag | 网格线写 G-Buffer（当前无实体触发） |
| `skybox_mrt.vert` / `skybox_mrt.frag` | vert/frag | 天空盒写 G-Buffer（写 `outNormal=(0,0,0)`） |
| `ssao.frag` | frag | SSAO（视图空间遮挡判断）；Normal.w=0 的地形/网格线直接输出 1.0 |
| `ssao_blur.frag` | frag | SSAO 模糊 |
| `lighting.vert` / `lighting.frag` | vert/frag | HDR 光照 |
| `bloom_bright.frag` | frag | 亮部提取 |
| `bloom_down.frag` | frag | 降采样 |
| `bloom_up.frag` | frag | 升采样（加法混合） |
| `present.frag` | frag | tonemap + ImGui 前合成（Bloom 开） |
| `present_no_bloom.frag` | frag | 单输入 tonemap（Bloom 关） |
| `taa.frag` | frag | 时域抗锯齿：反投影上一帧、AABB clamp 历史、写当前颜色与世界半径到双附件 |
| `shadow.vert` / `shadow.frag` | vert/frag | 3 级 CSM 深度渲染，法线偏移 + 斜率 bias |
| `fullscreen.vert` | vert | 全屏三角 |
| `ibl_equirect_to_cube.frag` | frag | 等距柱状 -> cubemap |
| `ibl_irradiance.frag` | frag | 漫反射 irradiance |
| `ibl_prefilter.frag` | frag | 镜面 prefilter |
| `ibl_brdf_lut.frag` | frag | BRDF LUT |
| `ibl_capture.vert` | vert | IBL 面渲染 |
| `compute_probe.comp` | comp | D-5 compute 验收探针 |
| `scene_cull.comp` | comp | F1 场景实例视锥/Hi-Z 剔除，写组级 indirect command |
| `hiz_build.comp` / `hiz_downsample.comp` | comp | 从 G-Buffer.Position 建 Hi-Z 金字塔（下一帧剔除用） |
| `terrain_height.comp` | comp | GPU 地形高度场：Perlin/fBm、ridge/detail、island/edge mask、height curve |
| `terrain_lod.comp` | comp | B-03：chunk frustum cull、LOD 滞回/过渡状态、indirect command/count/stats/debug 输出；stateIndex 按 4-int slot 寻址 |
| `terrain_mesh.comp` | comp | B-04：单 chunk 顶点/索引组装；11-float tight Vertex、B-02 transition band、可选裙边 |
| `cross_queue_probe.comp` / `.frag` | comp/frag | TD-02 跨队列探针 |
| `editor_overlay.vert` / `.frag` | vert/frag | 编辑器选中框/辅助线叠加 |

## 2. 编译管线

CMake 对 `shaders/*.{vert,frag,comp}` 逐个：

```
glslangValidator -V --target-env vulkan1.3 <src> -o <build>/shaders/<name>.spv
spirv-val <build>/shaders/<name>.spv        # 静态合法性校验（P0-3）
```

- 输出到 `build/shaders/`，**源目录不允许出现 `.spv`**（`Validate.ps1` 的 TD-08 dead-file guard 会拒绝）。
- `spirv-val` 缺失时只警告并跳过静态校验。
- 新增 shader **不需要重新 configure**（`file(GLOB CONFIGURE_DEPENDS)` + PRE_BUILD），
  但**新增 `.cpp` 需要**。

## 3. `--target-env vulkan1.3` 的两个后果

1. **compute 改用 `LocalSizeId` 执行模式**（SPIR-V 1.6 弃用了 `WorkgroupSize` 内建），
   这避免了 best-practices 每次报 2 条弃用 warning。
2. 但 `LocalSizeId` **要求启用 `maintenance4` 特性**，否则验证层报
   `VUID-RuntimeSpirv-LocalSizeId-06434`。

`maintenance4` 是 Vulkan 1.3 核心特性、属于 `VkPhysicalDeviceVulkan13Features` 的成员：

```cpp
features13.maintenance4 = VK_TRUE;   // 正确
```

**不能**再单独挂一个 `VkPhysicalDeviceMaintenance4Features` 结构体——那会违反
`VUID-VkDeviceCreateInfo-pNext-06532`（当 pNext 链里已有 `Vulkan13Features` 时，
不允许再出现 `Maintenance4Features` 等被提升的结构体）。

## 4. 文件编码（重要）

**shader 文件必须是 UTF-8 无 BOM。** 带 BOM 会让 `glslangValidator` 把前 3 个字节
当成非法 token，报 `'#' : unexpected token` 或
`ES shaders for SPIR-V require version 310 or higher`（因为 `#version` 没被识别）。

用 PowerShell 的 `Set-Content` / `Out-File` 写文件会按 ANSI 或加 BOM，
**不要用它们改 shader 或源码**（见 [pitfalls](../60-debug-profiling/pitfalls.md)）。

快速检查：

```powershell
Get-ChildItem shaders -File | ForEach-Object {
    $b = [System.IO.File]::ReadAllBytes($_.FullName)[0..2]
    if ($b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { "BOM: $($_.Name)" }
}
```

## 5. 改 shader 后的验证

```powershell
# 1) 构建（隐含 glslangValidator + spirv-val）
cmake --build build --config Debug

# 2) 四种 Pass 组合，stderr 必须 0 字节
.\\build\\Debug\\TinyEngine.exe --selftest 120
.\\build\\Debug\\TinyEngine.exe --selftest 120 --no-bloom
.\\build\\Debug\\TinyEngine.exe --selftest 120 --no-ssao
.\\build\\Debug\\TinyEngine.exe --selftest 120 --no-bloom --no-ssao
```

改画面相关 shader 时，还要按 [acceptance](../50-validation/acceptance.md) 做截图对比。
