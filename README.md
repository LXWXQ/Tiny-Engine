# Tiny Engine

一个面向**学习与工程验证**的 Vulkan 1.3 渲染器（Windows x64 / C++17 / MSVC）。
重点不是堆功能，而是把**渲染同步、资源生命周期、自动化回归**做成可验证的工程基础。

- 渲染主线：`G-Buffer -> Vegetation -> SSAO -> Lighting(线性 HDR) -> Cloud -> FogVolume -> PlanarReflection -> SSR -> Glass -> Water -> Particle -> TAA -> Bloom -> Present(ACES+gamma+ImGui)`
- 一条命令完成：依赖准备 -> 全新 configure/build -> 96 项回归
- 验收基线：**退出码 0** + **stderr 0 字节** + `[Validation] errors=0 warnings=0`

> 本文件是仓库根目录**唯一**的文档。所有细节在 [`docs/`](docs/README.md) 下，按类型分层。
> 需要先了解"文档怎么读/怎么维护"，直接看 [`docs/README.md`](docs/README.md)。

---

## 1. 快速开始

```powershell
# 一条命令：准备依赖 + 全新构建 + 全部回归
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1
```

日志落在 `build/validation/<timestamp>/`。

手动构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/SetupDependencies.ps1

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug

.\build\Debug\TinyEngine.exe
```

**环境要求**

| 项 | 要求 |
|---|---|
| 系统 | Windows 10/11 x64 |
| GPU | Vulkan 1.3：dynamicRendering / synchronization2 / timelineSemaphore / descriptorIndexing / maintenance4 |
| Vulkan SDK | 1.3+（含 `glslangValidator`、`spirv-val`） |
| CMake | 3.15+（推荐 3.21+） |
| 编译器 | Visual Studio 2022 |
| PowerShell | Windows PowerShell 5.1+ |

`find_package(Vulkan REQUIRED)` 自动定位 SDK；找不到时设置环境变量 **`VULKAN_SDK` 为 SDK 根目录**（例如 `D:\work\vulkan`，不是 `.../Bin`）。

> 详细依赖版本、目录布局与常见故障见 [`docs/40-build/`](docs/40-build/README.md)。

---

## 2. 特性一览

本表是**能力索引**：每行只说「这是什么 + 一处关键数字」，机制、参数表与调参细节在各篇文档里，
不在这里重复（同一事实只有一个出处，避免改一处漏一处）。

| 能力 | 一句话 | 文档 |
|---|---|---|
| 线性 RenderGraph | 访问声明驱动 image/buffer 屏障；动态渲染，无 `VkRenderPass` | [rendergraph](docs/10-architecture/rendergraph.md) |
| 子资源语义校验 | 按 mip/layer 跟踪布局；buffer 按 offset/size 区间；写覆盖校验 | [rendergraph](docs/10-architecture/rendergraph.md) |
| 瞬时资源硬校验 | 下游读的图本帧没人写 -> `endFrame` 直接抛异常并点名 | [rendergraph](docs/10-architecture/rendergraph.md) |
| 2 帧在飞 + timeline semaphore | 命令缓冲 / UBO / 瞬时图 / 描述符集按 frame slot 隔离 | [synchronization](docs/10-architecture/synchronization.md) |
| 异步 compute 双队列 | graphics/compute 独立队列与 timeline；跨队列单向依赖推导 + 环检测 | [synchronization](docs/10-architecture/synchronization.md) |
| Bindless 材质 | 1 个 set 4 个纹理数组 + push constant 索引；`desc binds` 104 -> 1 | [descriptors](docs/10-architecture/descriptors.md) |
| HDR + Bloom + ACES | 光照输出线性 HDR，tonemap 独立成 PresentPass；Bloom 关闭时真跳过 7 个 pass | [pass-order](docs/20-pipeline/pass-order.md) |
| TAA | Halton(2,3) 抖动 + 未抖动 VP 反投影 + 3x3 邻域 AABB clamp；`--no-taa` | [pass-order](docs/20-pipeline/pass-order.md) |
| **水面** | 相机中心径向网格（160 圈 × 192 段）+ FFT 法线细节 + Fresnel + 半分辨率平面反射与近场 SSR；`--no-water` / `--no-ssr` | [water](docs/30-systems/water.md) |
| **体积云** | `scene.clouds`：单层全屏 raymarch，64³ 可平铺噪声 + 4 步自阴影，预乘 alpha 混进 HDR，默认关闭 | [pass-order](docs/20-pipeline/pass-order.md) |
| **局部体积雾** | `effects.fogVolume`：局部雾盒（最多 8 个）+ 32³ 3D 噪声 + 高度衰减 + HG 相函数 | [fog-volume](docs/30-systems/fog-volume.md) |
| **粒子** | `effects.particles`：CPU 模拟（每发射器 512、全局 16384），透明 billboard 写 HDR，排在 Water 后、TAA 前 | [scene-ecs](docs/30-systems/scene-ecs.md) |
| **距离雾** | 指数距离雾（80m 起、密度 0.00045），雾色取环境图地平线；天空不参与 | [water](docs/30-systems/water.md) |
| **太阳与镜头光晕** | `scene.sun`：日盘 + 两段日冕 + 5 点深度遮挡判定 + 屏幕空间鬼影，TAA 后 Bloom 前加色 | [pass-order](docs/20-pipeline/pass-order.md) |
| IBL split-sum | irradiance / prefiltered / BRDF LUT 一次性 GPU 烘焙 | [ibl](docs/20-pipeline/ibl.md) |
| EnTT ECS + Transform 层级 | parent/children + world/normal matrix 传播；元数据组件注册表 + Simulation/PostTransform 调度 | [scene-ecs](docs/30-systems/scene-ecs.md) |
| 空间索引 + 视锥剔除 | 均匀网格 broad phase + 增量 AABB upsert + XZ region bucket + frustum-AABB | [scene-ecs](docs/30-systems/scene-ecs.md) |
| CPU JobSystem | worker pool + `submit()`/`parallelFor()` + 依赖图 + 异常传播 | [jobsystem](docs/30-systems/jobsystem.md) |
| 窗口缩放 + 动态分辨率 | swapchain 原地重建；中间目标按 renderScale 缩放 | [synchronization](docs/10-architecture/synchronization.md) |
| 版本化配置 v2 | 显式 v0->v1->v2 迁移、备份、损坏/未来版本保护 | [config](docs/30-systems/config.md) |
| **场景编辑器** | Hierarchy / Inspector / Console + 菜单栏 + 播放状态机 + 多选/复制/撤销/预制体 + 地形与体素编辑 | [editor](docs/90-editor/README.md) |
| **撤销 / 重做** | 命令栈 + 拖拽合并窗口；删除实体可完整撤销（体素专用组件快照未覆盖） | [commands](docs/90-editor/commands.md) |
| **场景存读** | `*.scene.json` v4（实体 + 12 个场景功能组件）、`*.prefab.json`、本机偏好 | [serialization](docs/90-editor/serialization.md) / [prefabs](docs/90-editor/prefabs.md) |
| **离屏视口** | 场景画到 render target 再作为 ImGui 图片贴进面板；相机纵横比按视口算 | [viewport](docs/90-editor/viewport.md) |
| **程序化地形** | 257² 默认精度；单实体或独立 chunk 流送；分层材质 + LOD 子网格 + GPU compute 高度场 + 异步 mesh submit | [terrain](docs/30-systems/terrain.md) |
| **级联阴影** | 3 级 CSM（每级 2048²）、纹素吸附、AABB 8 角点剔除、5x5 PCF 或 blocker-search PCSS 可选；`--no-shadows` | [shadow](docs/20-pipeline/shadow.md) |
| **运行模式角色** | 第三人称 + 骨骼动画（4 蒙皮网格 / 29 骨骼 / 18 动画），CPU 蒙皮 + 3 帧环形顶点缓冲；`--runtime-mode` | [character](docs/30-systems/character.md) |
| **地形植被** | `scene.vegetation`：程序化草簇与针叶/阔叶树，按地形高度筛选，风摆在顶点着色器，树投 CSM | [vegetation](docs/30-systems/vegetation.md) |
| **方块体素** | 32³ chunk、面剔除 + 贪心合并 + 顶点 AO、DDA 拾取、挖/放与逐格撤销；`--voxel-world` | [voxel](docs/30-systems/voxel.md) |
| 调试剖析基础设施 | `--build-info` / `--gpu-timing` / `--screenshot` / 验证层增强 / RenderDoc / Tracy | [debug](docs/60-debug-profiling/README.md) |

**已知边界**（只列最容易被误解的四类，完整清单见 [`docs/70-status/known-limits.md`](docs/70-status/known-limits.md)）：

- 光照与后效：SSR 只服务水面近场；折射 / 水下后处理 / 自动曝光 / 点光源阴影 / 音频未做；距离雾是均匀指数雾（无高度衰减、天空不参与）。
- 体积效果：云、雾、太阳光晕都只有主 HDR 通道，不写深度、不进平面反射与阴影；云与雾默认关闭，雾盒最多 8 个。
- 剔除与几何：TAA 只做相机重投影（无运动向量）；体素无 LOD（存档只记 seed + 差分区）；植被无 LOD、草不投自身阴影；角色是单角色 CPU 蒙皮（无混合 / 事件 / IK）。
- 编辑器与序列化：预制体只支持单根普通实体子树（无嵌套 / 变体）；场景组件仍编译期注册，无脚本 / 热插拔；跨队列资源走 `CONCURRENT` sharing。


---

## 3. 命令行

| 参数 | 用途 |
|---|---|
| 无参数 | 正常窗口运行 |
| `--selftest N` | 无交互跑 N 帧后退出 |
| `--no-bloom` / `--no-ssao` / `--no-taa` / `--no-water` | 关闭对应 Pass（自检与普通运行均可） |
| `--render-scale F` | 中间目标倍率 0.25~1.0 |
| `--resize-test` | 连续缩放 / 动态分辨率 / 最小化恢复 |
| `--jobtest` | 只测 CPU JobSystem（不建窗口） |
| `--compute-test` | compute probe 的 GPU 写 + CPU 读回回归 |
| `--cross-queue-test` | 跨队列双方向探针 |
| `--subresource-test` / `--subresource-error-test` | 子资源正例 / 反例 |
| `--material-stress` | 材质槽位延迟回收压测 |
| `--config-test` | 配置迁移 / 损坏保护自检 |
| `--editor` | 以场景编辑器模式启动（默认关闭，运行时行为完全不变） |
| `--runtime-mode` | 编辑器启动后直接进入运行模式（第三人称角色 + 动画；仅编辑器构建有效） |
| `--voxel-world [r]` / `--voxel-glass-demo` | 生成方块体素世界（半径 1..12）/ 摆交叉玻璃墙做透明排序目测 |
| `--particle-demo` | 空场景创建一个粒子喷泉（rate=220、max=1500、Kenney `fire_01` sprite），用于粒子截图/回归 |
| `--cloud-demo` | 空场景创建/启用 `scene.clouds` 并把相机放到云层取景位；配合 `--empty-scene --selftest` / `--screenshot` 做云截图与回归 |
| `--fog-demo` | 空场景创建一个局部雾盒（density=0.045、steps=24、盒体 30x10x36）并把相机放到地形上方；建议配合 `--streaming-world 3` |
| `--sun-demo` | 空场景创建/启用 `scene.sun`，把方向光转到相机前方右上并放到水面取景位；建议配合 `--streaming-world` / `--screenshot` 做太阳与光晕截图 |
| `--vegetation-demo` | 空场景创建/启用 `scene.vegetation` 并把相机放到地形上方；建议配合 `--streaming-world 3`，否则没有地形可长 |
| `--terrain-test` | 启动时创建持久地形 + 流送/雕刻/角色 smoke，校验 GPU 高度场、GPU mesh parity、GPU selection 过渡、区域限定重建与截图路径（常配合 `--empty-scene --selftest`） |

| `--build-info` | 打印编译进 & 本次开启的调试能力后退出 |
| `--gpu-timing [--gpu-timing-csv P]` | 每 pass GPU 计时 |
| `--validation-features bp,sync,printf,gpuav` | 验证层增强 |
| `--validation-jsonl P` | 验证层消息落 JSONL（默认不落盘、不写 stderr） |
| `--screenshot P [--screenshot-frame N]` | 无头截图 |
| `--capture-frame N` | RenderDoc in-app 抓帧（需注入） |
| `--device-fault-test` | `VK_EXT_device_fault` 能力自检 |

构建同时产出 `build/<Config>/TinyEditor.exe`，双击即进编辑器，无需敲参数。

完整说明与组合示例见 [`docs/50-validation/cli-reference.md`](docs/50-validation/cli-reference.md)。

---

## 4. 文档索引

> 文档系统本身怎么组织、怎么写、改动后要同步什么，全部在 [`docs/README.md`](docs/README.md)。

| 目录 | 内容 | 什么时候读 |
|---|---|---|
| [`docs/00-meta/`](docs/00-meta/README.md) | 文档规则、代码↔文档反查表、任务导向阅读路径 | **每次都建议先扫一眼 reading-paths** |
| [`docs/10-architecture/`](docs/10-architecture/README.md) | 分层、渲染图、资源生命周期、同步、描述符、设计决策 | 改 `src/RHI/` 或同步相关逻辑 |
| [`docs/20-pipeline/`](docs/20-pipeline/README.md) | Pass 顺序、G-Buffer/中间目标、IBL、Shader 清单 | 改 `src/Render/` 或 `shaders/` |
| [`docs/30-systems/`](docs/30-systems/README.md) | ECS/场景、JobSystem、配置、ImGui | 改 `src/Scene/`、`src/Core/` 对应模块 |
| [`docs/40-build/`](docs/40-build/README.md) | 依赖版本、构建运行、排障 | 构建失败 / 换机器 |
| [`docs/50-validation/`](docs/50-validation/README.md) | CLI 全量参数、96 项回归矩阵、验收标准 | 加测试 / 定位回归 |
| [`docs/60-debug-profiling/`](docs/60-debug-profiling/README.md) | 零开销开关模型、工具速查、诊断实现、**坑清单** | 加调试能力 / **改任何代码前** |
| [`docs/70-status/`](docs/70-status/README.md) | 里程碑、技术债、已知边界 | 决定"下一步做什么" |
| [`docs/80-roadmap/`](docs/80-roadmap/README.md) | 主功能路线 F1~F7、可借鉴技术清单 TM、UE 借鉴改造总纲 | 开新主功能前 |
| [`docs/90-editor/`](docs/90-editor/README.md) | 编辑器架构、撤销栈、场景序列化、面板布局、UX 判据与改造协议 | **改 `src/Editor/` 之前** |

---

## 5. 文档管理规则（摘要）

完整规则见 [`docs/00-meta/doc-rules.md`](docs/00-meta/doc-rules.md)。三条硬规则：

1. **根目录只允许存在 `README.md` 一个文档**。任何新文档都必须落到 `docs/<分类>/`，并在该分类的
   `README.md` 与本文件第 4 节的索引里登记。
2. **子文档改动必须回写父索引**。新增 / 重命名 / 删除 / 改变职责范围时，父目录 `README.md` 与
   `docs/README.md` 的索引、以及根 `README.md` 第 4 节，必须在**同一次改动**里同步更新；
   仅改内容而不改索引是可以的，但改了索引必须同时改根 `README.md`。
3. **代码行为变更必须同步"登记过该行为的文档"**。`docs/00-meta/doc-map.md` 是代码↔文档的反查表，
   改代码前先查它；改完按表回写。校验用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/DocCheck.ps1
```

`DocCheck.ps1` 会检查：根目录文档数量、每个 `.md` 的 front-matter 完整性、父子索引覆盖、相对链接是否可达、代码反查表是否覆盖新增源文件。**它不检查单篇行数**（2026-11-04 起取消按行数硬拆）。它已接入 `tools/Validate.ps1` 作为回归项。

---

## 6. 当前基线（实测）

只列「需要一眼看到」的数字；**完整基线表、各项的测法与历史对照在
[`docs/70-status/milestones.md`](docs/70-status/milestones.md) §2/§3**，不在这里重复。

| 项 | 值 |
|---|---|
| 回归 | `tools/Validate.ps1`：**96 项全 PASS**（`-EnableProfiling` 时 97 项），stderr 全 0 字节 |
| 验证层 | `[Validation] errors=0 warnings=0`（含 `--validation-features sync`） |
| 场景 | Sponza：103 个组 + 天空盒；CPU 路径 `draw calls 104`，合并模式 `draw calls 2` |
| 绑定 | `desc binds 2`（Set0/1/2 + Set3 实例 SSBO；bindless 生效） |
| 帧率 | Debug / RTX 2070：`--selftest 300` 约 240~260 FPS（仅作同机相对比较） |
| 流送世界 | 半径 2 -> `chunks=25`、`maxUpdateMs≈0.7`；Release 半径 24（2401 块）= 208 FPS、`Scene::update()` 约 62 µs/帧（Debug 只有 17 FPS，FPS 必须标构建类型） |
| 地形合批 | arena 半径 12 约 46+22 MB、半径 24 约 171+82 MB；`--streaming-world 2` 的 draw calls 13 -> 2 |
| GPU 批 | 实例数 ≥ 256 才走 GPU 提交（当前 103 实例默认走 CPU 路径，`--gpu-scene` 可强制）；稳定帧 439µs -> 9µs；当前场景 `hizCulled=0` |

> 启动失败的原生 MessageBox 与窗口收敛、快捷键单一数据源（`entries=20 conflicts=0`）不属基线，
> 分别见 [`docs/40-build/troubleshooting.md`](docs/40-build/troubleshooting.md) 与
> [`docs/90-editor/commands.md`](docs/90-editor/commands.md)。合批与 GPU 驱动的机制见
> [`docs/10-architecture/gpu-driven-submission.md`](docs/10-architecture/gpu-driven-submission.md)。

> 更新基线时**成对更新**本表与 [`docs/70-status/milestones.md`](docs/70-status/milestones.md) §2；
> 只在其中一处改数字视为文档缺陷（`DocCheck` 查不出，靠评审）。

