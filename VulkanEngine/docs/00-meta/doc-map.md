---
id: meta/doc-map
title: 代码与文档反查表
type: meta
scope: src/, shaders/, tools/, CMakeLists.txt, main.cpp
read-when: 改动任何源文件 / shader / 脚本之前
size: ~150 行
updated: 2026-10-30
---

# 代码 <-> 文档反查表

**用法**：改代码前扫一眼"源文件"列，找到你的文件，把"必读文档"读一遍；改完按"回写"列更新。

## 1. RHI 层（`src/RHI/`）

| 源文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `RenderGraph.h/.cpp` (49KB) | 栅栏推导、子资源状态、buffer 区间、语义校验、跨队列依赖 | [rendergraph](../10-architecture/rendergraph.md) | rendergraph |
| `Renderer.h/.cpp` | frame slot、timeline、submit/present 顺序、resize | [synchronization](../10-architecture/synchronization.md) | synchronization |
| `Device.h/.cpp` | instance/device/queue/feature/VMA、验证层回调 | [layering](../10-architecture/layering.md)、[diagnostics](../60-debug-profiling/diagnostics.md) | layering、diagnostics |
| `Swapchain.h/.cpp` | 交换链、重建、格式 | [synchronization](../10-architecture/synchronization.md) | synchronization |
| `Pipeline.h/.cpp` | Graphics/Compute pipeline 封装、动态状态 | [pass-order](../20-pipeline/pass-order.md) 的"管线约定"节 | pass-order |
| `GBuffer.h/.cpp` | 5 个附件与格式常量 `GBufferFormat` | [gbuffer](../20-pipeline/gbuffer.md) | gbuffer |
| `Texture.h/.cpp` / `CubemapTexture.*` | 普通纹理 / cubemap 上传 | [ibl](../20-pipeline/ibl.md) | ibl |
| `GpuTimer.h/.cpp` | 每 pass timestamp | [diagnostics](../60-debug-profiling/diagnostics.md) | diagnostics |
| `GpuProfiler.h/.cpp` | Tracy GPU zone | [tools](../60-debug-profiling/tools.md) | tools |
| `ScreenshotCapture.h/.cpp` | 交换链读回写 PNG | [diagnostics](../60-debug-profiling/diagnostics.md) | diagnostics |
| `RenderDocHook.h/.cpp` | in-app 抓帧 | [tools](../60-debug-profiling/tools.md) | tools |
| `CommandUtils.h` / `VmaConfig.h` / `VmaNaming.h` | 命令缓冲单次提交、VMA 配置、分配命名 | [resource-lifetime](../10-architecture/resource-lifetime.md) | resource-lifetime |

## 2. Render 层（`src/Render/`）

| 源文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `RenderPipeline.h` / `IRenderPass.h` | Pass 注册容器 / Pass 抽象（`read`/`writeColor`/`setEnabled`） | [rendergraph](../10-architecture/rendergraph.md) | rendergraph |
| `RenderResources.h` | 帧内资源登记表（句柄集合） | [rendergraph](../10-architecture/rendergraph.md) | rendergraph |
| `GeometryPass.*` / `GeometrySystem.*` | 写 G-Buffer、材质绑定、剔除后遍历 | [pass-order](../20-pipeline/pass-order.md)、[scene-ecs](../30-systems/scene-ecs.md) | pass-order、scene-ecs |
| `SSAOPass.*` | SSAO 输出 + 模糊 | [pass-order](../20-pipeline/pass-order.md) | pass-order |
| `LightingPass.*` / `LightingSystem.*` | HDR 光照输出、导入交换链 | [pass-order](../20-pipeline/pass-order.md) | pass-order |
| `BloomPass.*` | Bright + 3 down + 3 up 金字塔 | [intermediate](../20-pipeline/intermediate.md) | intermediate |
| `PresentPass.*` | 双路径（Bloom/NoBloom）、ACES、ImGui 叠加 | [pass-order](../20-pipeline/pass-order.md) | pass-order |
| `IBLBaker.*` | irradiance / prefilter / BRDF LUT 烘焙 | [ibl](../20-pipeline/ibl.md) | ibl |
| `ComputeProbePass.*` | D-5 compute 验收探针 | [test-matrix](../50-validation/test-matrix.md) | test-matrix |
| `CrossQueueProbePass.*` | TD-02 跨队列双方向探针 | [test-matrix](../50-validation/test-matrix.md) | test-matrix |
| `SubresourceProbePass.*` | TD-05 子资源正/反例探针 | [rendergraph](../10-architecture/rendergraph.md) | rendergraph |

## 3. Core 层（`src/Core/`）

| 源文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `Application.h/.cpp` (39KB) | 唯一创建顺序、帧循环、自检入口、resize | [layering](../10-architecture/layering.md)、[resource-lifetime](../10-architecture/resource-lifetime.md) | layering、resource-lifetime |
| `DescriptorManager.h/.cpp` | Set0 全局 + Set1 bindless 材质槽位 + 延迟回收 | [descriptors](../10-architecture/descriptors.md) | descriptors |
| `EngineConfig.h/.cpp` | v2 配置、迁移、备份、原子写 | [config](../30-systems/config.md) | config |
| `Diagnostics.h/.cpp` | 验证层结构化计数、JSONL、`--build-info`、自检 | [diagnostics](../60-debug-profiling/diagnostics.md) | diagnostics |
| `JobSystem.h/.cpp` | worker pool、`submit`、`parallelFor` | [jobsystem](../30-systems/jobsystem.md) | jobsystem |
| `ImGuiSystem.h/.cpp` | 后端初始化、面板、ASCII 约束 | [imgui](../30-systems/imgui.md) | imgui |
| `FrameInfo.h` / `FrameInFlight.h` | 每帧数据总线、`kFramesInFlight` | [synchronization](../10-architecture/synchronization.md) | synchronization |
| `Material.h` / `MaterialSystem.*` / `MaterialSlot.h` | 材质数据、槽位哨兵 `kInvalidMaterialSlot` | [descriptors](../10-architecture/descriptors.md) | descriptors |
| `Profiler.h` | Tracy CPU zone 宏（关闭时展开为 `(void)0`） | [zero-overhead](../60-debug-profiling/zero-overhead.md) | zero-overhead |

## 4. Scene 层（`src/Scene/`）

| 源文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `Scene.h/.cpp` | `entt::registry`、setParent、世界变换传播、visibleEntities | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |
| `Components.h` | 全部 component 定义 | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |
| `SpatialIndex.h/.cpp` | 均匀网格 broad phase | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |
| `GLTFLoader.h/.cpp` | glTF 导入（几何烘焙到世界空间） | [scene-ecs](../30-systems/scene-ecs.md)、[known-limits](../70-status/known-limits.md) | scene-ecs |
| `Model.h/.cpp` | 顶点/索引上传、子网格、OBJ | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |
| `Camera*` / `CameraController.*` | 相机与输入 | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |

## 5. Shader（`shaders/`）

| 文件 | 用途 | 必读文档 |
|---|---|---|
| `mrt.vert/.frag`、`grid_mrt.*`、`skybox_mrt.*` | 写 G-Buffer 的三条几何路径 | [gbuffer](../20-pipeline/gbuffer.md) |
| `ssao.frag`、`ssao_blur.frag` | SSAO 与模糊 | [pass-order](../20-pipeline/pass-order.md) |
| `lighting.frag/.vert` | HDR 光照 | [pass-order](../20-pipeline/pass-order.md) |
| `bloom_bright/down/up.frag` | Bloom 金字塔 | [intermediate](../20-pipeline/intermediate.md) |
| `present.frag`、`present_no_bloom.frag` | tonemap + ImGui 前合成 | [pass-order](../20-pipeline/pass-order.md) |
| `ibl_*.frag/.vert` | IBL 烘焙 | [ibl](../20-pipeline/ibl.md) |
| `fullscreen.vert` | 全屏三角 | [shaders](../20-pipeline/shaders.md) |
| `compute_probe.comp`、`cross_queue_probe.*` | 测试探针 | [test-matrix](../50-validation/test-matrix.md) |

任何 shader 增删都要回写 [shaders.md](../20-pipeline/shaders.md) 的清单表。

## 6. 构建与脚本

| 文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `CMakeLists.txt` | 依赖检查、shader 编译 + `spirv-val`、`SENTINEL_ENABLE_*` 开关 | [build](../40-build/build.md)、[dependencies](../40-build/dependencies.md) | build、dependencies |
| `cmake/AssetPaths.h.in` | 资源绝对路径注入 | [build](../40-build/build.md) | build |
| `main.cpp` | 全部 CLI 参数解析 | [cli-reference](../50-validation/cli-reference.md) | cli-reference |
| `tools/Validate.ps1` | 主回归（20 项 + 文档检查） | [test-matrix](../50-validation/test-matrix.md) | test-matrix |
| `tools/SetupDependencies.ps1` | 8 个依赖的固定版本安装 | [dependencies](../40-build/dependencies.md) | dependencies |
| `tools/DocCheck.ps1` | 本文档系统的自动校验 | [doc-rules](doc-rules.md) | doc-rules |
| `tools/Baseline.ps1` / `DebugBundle.ps1` / `DeepValidation.ps1` / `Screenshot.ps1` / `Capture.ps1` / `Profile.ps1` | 剖析与证据收集 | [tools](../60-debug-profiling/tools.md) | tools |
| `config/engine.json` | 运行时配置（version 2） | [config](../30-systems/config.md) | config |

## 7. 新会话最短路径

1. 读根 [`README.md`](../../README.md)（约 150 行）——知道项目是什么、怎么跑。
2. 读 [`reading-paths.md`](reading-paths.md)，按任务选 1~3 篇。
3. 只读选中那几篇的 front-matter + 正文，不要去读整个 `docs/`。

**不要**在没有具体任务时通读 `10-architecture/` 全部文档——那是 1500+ 行，
只有做架构级改动时才需要。
