---
id: meta/doc-map
title: 代码与文档反查表
type: meta
scope: src/, shaders/, tools/, CMakeLists.txt, main.cpp
read-when: 改动任何源文件 / shader / 脚本之前
size: ~150 行
updated: 2026-11-01
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
| `GeometryPass.*` / `GeometrySystem.*` / `GeometrySystemGpu.cpp` / `terrain_lod.comp` | GPU terrain selection、indirect count、写 G-Buffer、Set0/1/2 绑定、地形 push 参数 | [pass-order](../20-pipeline/pass-order.md)、[descriptors](../10-architecture/descriptors.md) | pass-order、descriptors |
| `TerrainMaterial.*` | 优先加载外部 16 张 PNG，否则生成程序化源纹理；打包 height/roughness 后更新 12-sampler Set 2 |
| `SubMeshCulling.*` | 纯 CPU frustum-AABB、世界 bounds 与地形 LOD 距离选择 | [terrain](../30-systems/terrain.md)、[descriptors](../10-architecture/descriptors.md) | terrain、descriptors |
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
| `DescriptorManager.h/.cpp` | Set0 全局 + Set1 bindless 材质槽位 + Set2 地形纹理 + 延迟回收 | [descriptors](../10-architecture/descriptors.md) | descriptors |
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
| `Scene.h/.cpp` | `entt::registry`、setParent、世界变换传播、visibleEntities、地形异步请求/主线程上传 | [scene-ecs](../30-systems/scene-ecs.md)、[terrain](../30-systems/terrain.md) | scene-ecs、terrain |
| `Components.h` | 全部 component 定义（含 `TerrainComponent::generating` / `isTerrain`） | [scene-ecs](../30-systems/scene-ecs.md)、[terrain](../30-systems/terrain.md) | scene-ecs、terrain |
| `SpatialIndex.h/.cpp` | 均匀网格 broad phase，entity cell 记录与增量 upsert/remove | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |
| `GLTFLoader.h/.cpp` | glTF 导入（几何烘焙到世界空间） | [scene-ecs](../30-systems/scene-ecs.md)、[known-limits](../70-status/known-limits.md) | scene-ecs |
| `Model.h/.cpp` | 顶点/索引上传、子网格、OBJ | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |
| `Camera*` / `CameraController.*` | 相机与输入 | [scene-ecs](../30-systems/scene-ecs.md) | scene-ecs |

### Terrain 层（`src/Terrain/`）

| 源文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `TerrainConfig.h` | 纯数据参数与校验 | [terrain](../30-systems/terrain.md) | terrain |
| `TerrainNoise.*` | 确定性 2D Perlin + fBm | [terrain](../30-systems/terrain.md) | terrain |
| `TerrainHeightField.*` | 规则高度场、世界采样、边缘衰减 | [terrain](../30-systems/terrain.md) | terrain |
| `TerrainMeshBuilder.*` | 顶点/索引/法线/UV/顶点色 | [terrain](../30-systems/terrain.md) | terrain |
| `TerrainTextureGenerator.*` | 确定性可平铺 4 层 albedo/normal/height/roughness 纹素 | [terrain](../30-systems/terrain.md) | terrain |
| `TerrainSelfTest.*` | 高度场/网格/纹理缓冲不变量自检 | [terrain](../30-systems/terrain.md) | terrain |

## 5. Shader（`shaders/`）

| 文件 | 用途 | 必读文档 |
|---|---|---|
| `mrt.vert/.frag`、`grid_mrt.*`、`skybox_mrt.*` | 写 G-Buffer 的几何路径；`mrt.*` 含地形四层 PBR 纹理、Surface Style、Set2 法线与 Normal.w SSAO mask | [gbuffer](../20-pipeline/gbuffer.md)、[terrain](../30-systems/terrain.md) |
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
| `tools/Validate.ps1` | 主回归（21 项 + 文档检查） | [test-matrix](../50-validation/test-matrix.md) | test-matrix |
| `tools/SetupDependencies.ps1` | 8 个依赖的固定版本安装 | [dependencies](../40-build/dependencies.md) | dependencies |
| `tools/DocCheck.ps1` | 本文档系统的自动校验 | [doc-rules](doc-rules.md) | doc-rules |
| `tools/Baseline.ps1` / `DebugBundle.ps1` / `DeepValidation.ps1` / `Screenshot.ps1` / `Capture.ps1` / `Profile.ps1` | 剖析与证据收集 | [tools](../60-debug-profiling/tools.md) | tools |
| `config/engine.json` | 运行时配置（version 2） | [config](../30-systems/config.md) | config |
| `CMakeLists.txt` | 两个可执行目标（`VulkanEngine` / `SentinelEditor`）+ `sentinel_core` 对象库 + `SentinelShaders` | [build](../40-build/build.md) | build |
| `main.cpp` | 唯一被编译两次的文件；`SENTINEL_DEFAULT_EDITOR` 决定默认模式 | [build](../40-build/build.md)、[editor](../90-editor/README.md) | build、editor |

## 6.5 编辑器层（`src/Editor/`）

> 编辑器是 `--editor` 才启用的独立层。关掉时这条路径完全不存在。

| 源文件 | 内容 | 必读文档 | 回写 |
|---|---|---|---|
| `EditorContext.h` | `EngineServices` / `Selection` / `PlayState` —— 编辑器与引擎的唯一接缝 | [editor/architecture](../90-editor/architecture.md) | architecture |
| `EditorState.h` | 选中、命令栈、日志、文档路径、播放快照 | [editor/architecture](../90-editor/architecture.md) | architecture |
| `EditorLayer.h/.cpp` | 菜单栏、布局、快捷键、播放状态机、场景存读 | [editor/architecture](../90-editor/architecture.md)、[editor/panels](../90-editor/panels.md) | architecture、panels |
| `CommandStack.h/.cpp` | 撤销/重做栈、合并窗口、历史上限 | [editor/commands](../90-editor/commands.md) | commands |
| `Commands.h/.cpp` | 变换 / 重命名 / 删除 / 创建网格 / 地形三命令 | [editor/commands](../90-editor/commands.md)、[terrain](../30-systems/terrain.md) | commands、terrain |
| `SceneSerializer.h/.cpp` | `SceneDescription` <-> JSON | [editor/serialization](../90-editor/serialization.md) | serialization |
| `panels/HierarchyPanel.*` | 实体树、拖拽重挂、右键删除 | [editor/panels](../90-editor/panels.md) | panels |
| `panels/InspectorPanel.*` | 组件编辑；变换/地形参数走命令栈，Generate 走 JobSystem，成功回调入重建命令 | [editor/panels](../90-editor/panels.md)、[editor/commands](../90-editor/commands.md)、[terrain](../30-systems/terrain.md) | panels、commands、terrain |
| `panels/ConsolePanel.*` | 日志过滤 | [editor/panels](../90-editor/panels.md) | panels |
| `panels/ViewportPanel.*` | 3D 视口：离屏纹理 + 射线拾取 | [editor/viewport](../90-editor/viewport.md)、[editor/gizmo](../90-editor/gizmo.md) | viewport、gizmo |
| `Gizmo.h/.cpp` | 三轴手柄：投影、命中测试、拖拽换算 | [editor/gizmo](../90-editor/gizmo.md) | gizmo |
| `src/Render/EditorOverlayPass.*` | 选中包围盒 OBB 的 3D 线框（复用 G-Buffer 深度做遮挡） | [editor/gizmo](../90-editor/gizmo.md) | gizmo |
| `shaders/mrt.frag` 棋盘格 | 无 albedo 贴图时的占位着色（完整 UV 走 2D 棋盘，否则 3D 棋盘） | [editor/materials](../90-editor/materials.md) | materials |
| `panels/AssetBrowserPanel.*` | 扫描 Models 目录，双击创建实体（走命令栈） | [editor/panels](../90-editor/panels.md) | panels |

配套的引擎侧改动（改这些要同时看编辑器文档）：

| 源文件 | 改动 | 必读文档 |
|---|---|---|
| `src/Core/FrameInfo.h` | `onBuildEditorUI` / `isEditorMode` 两个回调 | [editor/architecture](../90-editor/architecture.md) |
| `src/Core/ImGuiSystem.cpp` | 编辑器模式下跳过内置面板；`io.IniFilename` 指向 `config/imgui.ini` | [editor/panels](../90-editor/panels.md) |
| `src/Scene/SceneDescription.h` | 纯数据场景描述（序列化边界） | [editor/serialization](../90-editor/serialization.md) |
| `src/Scene/Scene.cpp` `captureDescription/buildFromDescription` | registry <-> 描述 | [editor/serialization](../90-editor/serialization.md) |
| `src/Render/UiCompositePass.*` | 编辑器模式最后一步：只画 ImGui 到交换链 | [editor/viewport](../90-editor/viewport.md) |
| `src/Render/PresentPass.*` | 输出目标二选一（视口纹理 / 交换链） | [editor/viewport](../90-editor/viewport.md) |
| `src/Core/ImGuiSystem.cpp` 描述符池 | 容量 64，供 `AddTexture` 注册视口纹理 | [editor/viewport](../90-editor/viewport.md) |
| `src/Scene/Components.h` `NameComponent` | 实体名（层级面板 + 序列化） | [scene-ecs](../30-systems/scene-ecs.md)、[editor/serialization](../90-editor/serialization.md) |

## 7. 新会话最短路径

1. 读根 [`README.md`](../../README.md)（约 150 行）——知道项目是什么、怎么跑。
2. 读 [`reading-paths.md`](reading-paths.md)，按任务选 1~3 篇。
3. 只读选中那几篇的 front-matter + 正文，不要去读整个 `docs/`。

**不要**在没有具体任务时通读 `10-architecture/` 全部文档——那是 1500+ 行，
只有做架构级改动时才需要。
