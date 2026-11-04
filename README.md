# Tiny Engine

一个面向**学习与工程验证**的 Vulkan 1.3 渲染器（Windows x64 / C++17 / MSVC）。
重点不是堆功能，而是把**渲染同步、资源生命周期、自动化回归**做成可验证的工程基础。

- 渲染主线：`G-Buffer -> SSAO -> Lighting(线性 HDR) -> Bloom -> Present(ACES+gamma+ImGui)`
- 一条命令完成：依赖准备 -> 全新 configure/build -> 67 项回归
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

| 能力 | 说明 | 文档 |
|---|---|---|
| 线性 RenderGraph | 访问声明驱动自动 image/buffer barrier；动态渲染；无 `VkRenderPass` | [rendergraph](docs/10-architecture/rendergraph.md) |
| 子资源语义校验 | 按 mip/layer 跟踪布局；buffer 按 offset/size 区间；写覆盖校验 | [rendergraph](docs/10-architecture/rendergraph.md) |
| 瞬时资源"必须有人写"硬校验 | 下游读的图本帧没人写 -> `endFrame` 直接抛异常并点名 | [rendergraph](docs/10-architecture/rendergraph.md) |
| 2 帧在飞 + timeline semaphore | 命令缓冲 / UBO / 瞬时图 / 描述符集按 frame slot 隔离 | [synchronization](docs/10-architecture/synchronization.md) |
| 异步 compute 双队列 | graphics/compute 独立队列与 timeline；跨队列单向依赖推导 + 环检测 | [synchronization](docs/10-architecture/synchronization.md) |
| Bindless 材质 | 1 个 set 4 个纹理数组 + push constant 索引；`desc binds` 104 -> 1 | [descriptors](docs/10-architecture/descriptors.md) |
| HDR + Bloom + ACES | 光照输出线性 HDR，tonemap 独立成 PresentPass；Bloom 关闭时真跳过 7 个 pass | [pass-order](docs/20-pipeline/pass-order.md) |
| IBL split-sum | irradiance / prefiltered / BRDF LUT 一次性 GPU 烘焙 | [ibl](docs/20-pipeline/ibl.md) |
| EnTT ECS + Transform 层级 | parent/children、world/normal matrix 传播 | [scene-ecs](docs/30-systems/scene-ecs.md) |
| 空间索引 + 视锥剔除 | 均匀网格 broad phase + Transform/Bounds 私有字段/dirty + 增量 AABB upsert + XZ region bucket + frustum-AABB | [scene-ecs](docs/30-systems/scene-ecs.md) |
| CPU JobSystem | worker pool + `submit()`/`parallelFor()` + 异常传播 | [jobsystem](docs/30-systems/jobsystem.md) |
| 窗口缩放 + 动态分辨率 | swapchain 原地重建；中间目标按 renderScale 缩放 | [synchronization](docs/10-architecture/synchronization.md) |
| 版本化配置 v2 | 显式 v0->v1->v2 迁移、备份、损坏/未来版本保护 | [config](docs/30-systems/config.md) |
| **场景编辑器** | Hierarchy / Inspector / Console 面板，菜单栏 + 播放状态机；多选（Ctrl/Shift）、`Ctrl+D` 复制、`Delete` 批量删除、`Ctrl+A` 全选、`F` 聚焦、多选一起拖拽（E2）、层级搜索、材质参数可撤销（E3）、未保存提示与文档标题（E4）、偏好持久化与最近场景（E4-b）、快捷键速查与中文字形（E4-c）、工具栏/状态栏与 Gizmo 吸附（E5-a）、渲染设置面板（E5-b）、空场景三处引导（B2）、错误三处可见 + 打开失败重试（B3）、菜单补齐四项 + 快捷键单一数据源 + 面板可拖动（B4）、界面中文统一 + Console 时间列/过滤/复制/搜索 + 状态栏三段 + 诊断复制（B5/B6）、地形生成两阶段提示与 DPI 缩放（B6）、资产拖拽入视口 + 材质贴图选择 + Inspector 数值行重置（E6）、地形异步生成（E7/TD-24）、帮助 → 示例预设一键加载（E10）（`--editor`） | [editor](docs/90-editor/README.md) |
| **撤销 / 重做** | 命令栈 + 拖拽合并窗口；删除实体可完整撤销（含子树与父子关系） | [commands](docs/90-editor/commands.md) |
| **场景存读** | `*.scene.json`；存语义（路径/参数）不存 GPU 句柄 | [serialization](docs/90-editor/serialization.md) |
| **离屏视口** | 3D 渲染到纹理后作为 ImGui 图片嵌入面板；相机纵横比按视口算 | [viewport](docs/90-editor/viewport.md) |
| **程序化地形** | 257² 默认精度；单实体/独立 chunk 流送；chunk 区域索引查询；4 层 albedo/normal、height/roughness 打包纹理、最多 4 个 Set2 材质集（按需外部 16 PNG/程序化回退、materialSetIndex 序列化、按批次重绑）、Surface Style、height/slope 混合、Set2 材质；chunk/LOD 子网格、逐块剔除、滞回选层、抖动 cross-fade、可选裙边回退、稳定帧 indirect 合批、GPU compute 高度场、拓扑级 LOD 缝合、GPU selection/indirect count；单 chunk GPU 顶点/索引组装 + device-local Model 接管 + 4 槽异步 GPU mesh submit/poll + worker picking mirror，多 chunk CPU fallback；CPU 高度采样/射线查询、kinematic 角色控制器（贴地/重力/跳跃/step/slope）、区域限定运行期雕刻（Alt+LMB；不写 JSON）+ GPU/JobSystem 异步 Generate + 可撤销重建 + 场景存读 | [terrain](docs/30-systems/terrain.md) |
| **级联阴影** | 3 级 CSM（每级 2048²，2D 数组 3 层）、纹素吸附、AABB 8 角点剔除、3x3 PCF、法线偏移 + 斜率 bias；`--no-shadows` 关掉。**默认场景阴影偏淡**：IBL 占主导，方向光只占约 5% 亮度（室内/压低环境光后非常明显） | [shadow](docs/20-pipeline/shadow.md) |
| **方块体素** | 32³ chunk、面剔除 + 贪心合并 + 顶点 AO、程序化贴图集（UV 编码图块 id）、Amanatides-Woo DDA 拾取、挖/放（Alt 挖 / Alt+Shift 放；Ctrl+左键仍是多选）与逐格撤销、`queryGroundAtWorld` 叠加体素分支让角色站上方块（运行模式接管角色：`WASD` 走、`Space` 跳、中键拖动看）；`--voxel-world [r]` 启动世界（半径 1..12，几何在 JobSystem 后台生成、逐帧上传）；编辑/运行模式分离（`F8`，运行模式左键挖、右键放）；场景存档只记 seed + 差分区（改动格）；玻璃走独立 `GlassPass`（Lighting 后、Bloom 前，按面质心排序混合，glass-only chunk 免 Model），`--voxel-glass-demo` 摆叠放场景 | [voxel](docs/30-systems/voxel.md) |
| 调试剖析基础设施 | `--build-info` / `--gpu-timing` / `--screenshot` / 验证层增强 / RenderDoc / Tracy | [debug](docs/60-debug-profiling/README.md) |

**已知边界**：无 SSR / 自动曝光 / 点光源阴影 / 动画 / 音频；体素存档只记 seed + 差分区（改动格），无 LOD（面剔除已升级为贪心合并 + 顶点 AO，贴图集为程序化生成）；跨队列资源走 `CONCURRENT` sharing；
glTF 内部节点层级在导入时烘焙到世界空间。完整清单见 [`docs/70-status/known-limits.md`](docs/70-status/known-limits.md)。

---

## 3. 命令行

| 参数 | 用途 |
|---|---|
| 无参数 | 正常窗口运行 |
| `--selftest N` | 无交互跑 N 帧后退出 |
| `--no-bloom` / `--no-ssao` | 关闭对应 Pass（自检与普通运行均可） |
| `--render-scale F` | 中间目标倍率 0.25~1.0 |
| `--resize-test` | 连续缩放 / 动态分辨率 / 最小化恢复 |
| `--jobtest` | 只测 CPU JobSystem（不建窗口） |
| `--compute-test` | compute probe 的 GPU 写 + CPU 读回回归 |
| `--cross-queue-test` | 跨队列双方向探针 |
| `--subresource-test` / `--subresource-error-test` | 子资源正例 / 反例 |
| `--material-stress` | 材质槽位延迟回收压测 |
| `--config-test` | 配置迁移 / 损坏保护自检 |
| `--editor` | 以场景编辑器模式启动（默认关闭，运行时行为完全不变） |
| `--voxel-world [r]` / `--voxel-glass-demo` | 生成方块体素世界（半径 1..12）/ 摆交叉玻璃墙做透明排序目测 |
| `--terrain-test` | 启动时创建持久地形 + 流送/雕刻/角色 smoke，校验 GPU 高度场、GPU mesh parity、GPU selection 过渡、区域限定重建与截图路径（常配合 `--empty-scene --selftest`） |

构建同时产出 `build/<Config>/TinyEditor.exe`，双击即进编辑器，无需敲参数。
| `--build-info` | 打印编译进 & 本次开启的调试能力后退出 |
| `--gpu-timing [--gpu-timing-csv P]` | 每 pass GPU 计时 |
| `--validation-features bp,sync,printf,gpuav` | 验证层增强 |
| `--validation-jsonl P` | 验证层消息落 JSONL（默认不落盘、不写 stderr） |
| `--screenshot P [--screenshot-frame N]` | 无头截图 |
| `--capture-frame N` | RenderDoc in-app 抓帧（需注入） |
| `--device-fault-test` | `VK_EXT_device_fault` 能力自检 |

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
| [`docs/50-validation/`](docs/50-validation/README.md) | CLI 全量参数、67 项回归矩阵、验收标准 | 加测试 / 定位回归 |
| [`docs/60-debug-profiling/`](docs/60-debug-profiling/README.md) | 零开销开关模型、工具速查、诊断实现、**坑清单** | 加调试能力 / **改任何代码前** |
| [`docs/70-status/`](docs/70-status/README.md) | 里程碑、技术债、已知边界 | 决定"下一步做什么" |
| [`docs/80-roadmap/`](docs/80-roadmap/README.md) | 主功能路线 F1~F7 | 开新主功能前 |
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

`DocCheck.ps1` 会检查：根目录文档数量、每个 `.md` 的 front-matter 完整性、父子索引覆盖、
相对链接是否可达、单文档体量是否超限。它已接入 `tools/Validate.ps1` 作为回归项。

---

## 6. 当前基线（实测）

| 项 | 值 |
|---|---|
| 回归 | `tools/Validate.ps1`：**67 项全 PASS**，stderr 全 0 字节 |
| 启动排错 | 双击启动失败看可执行文件旁的 `startup-error.log`；窗口尺寸按显示器工作区 92% 收敛 |
| 快捷键 | 只有一份 `kShortcutEntries`：速查表、`handleShortcuts()` 分发都读它；自检扫键位冲突（`entries=20 conflicts=0`，B4） |
| 验证层 | `[Validation] errors=0 warnings=0`（含 `--validation-features sync`） |
| 场景 | Sponza：103 个组 + 天空盒；CPU 路径 `draw calls 104`，合并模式 `draw calls 2`（相机朝向 Sponza 时） |
| 绑定 | `desc binds 2`（Set0/1/2 + Set3 实例 SSBO；bindless 生效） |
| 帧率 | Debug / RTX 2070：`--selftest 300` 约 240~260 FPS（仅作同机相对比较） |
| 流送世界 | `--streaming-world [radius]`：半径 1~24（F2-h 起 chunk 坐标 int64），半径 2 → `chunks=25`、`maxUpdateMs≈0.7`；`--streaming-fly 60` 飞 150 帧 → `loaded=54 unloaded=19`；Release 半径 24（2401 块）= 208 FPS、`Scene::update()` 约 62 µs/帧（Debug 只有 17 FPS，FPS 数字必须标构建类型） |
| 地形合批 | F2-b/F2-d/F2-g：可见 chunk 惰拷贝进 `TerrainMeshArena`（按实际尺寸分配，F2-h 起容量按环尺寸算：半径 12 约 46+22 MB、半径 24 约 171+82 MB），set 3 放 chunk 实例表，一条 `vkCmdDrawIndexedIndirect` 覆盖 16384 条命令；`--streaming-world 2` 的 draw calls 从 13 降到 2 |
| GPU 批 | F1-a~e：实例数 ≥ 256 才走 GPU 驱动提交（当前 103 实例默认走 CPU 路径，`--gpu-scene` 可强制）。按 `(模型, 子网格)` 分组实例化，合并顶点/索引缓冲后一条 `vkCmdDrawIndexedIndirect` 画完所有组（跨模型 single draw，`--no-merged-buffers` 整条退回 CPU），组按材质排序；结构签名命中时跳过摊平与实例上传（稳定帧 439µs → 9µs，只写 256B `frameData`）；`vec4 screen` 做屏幕空间误差剔除（`--gpu-scene-min-px`）；`ExternalInstance` 让粒子/体素共用同一提交路径（`--gpu-scene-external` 调试注入）；Hi-Z 遮挡剔除当前场景 `hizCulled=0`（Sponza 子网格太大，组级保守剔除不命中），`--no-hiz` 可关。 |

> 这些数字随硬件与代码变化。**更新基线时请成对更新本表与
> [`docs/70-status/milestones.md`](docs/70-status/milestones.md)。**
