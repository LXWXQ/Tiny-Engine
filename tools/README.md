# Tiny Engine 依赖与验证工具

## 一条命令

从仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1
```

默认行为：

1. 运行 `tools/SetupDependencies.ps1`，检查/下载固定版本的第三方依赖；
2. 执行 TD-08 dead-file guard（废弃 shader / Descriptor 旧系统 / 源目录 `.spv` 不得回归）；
   `tools/DocCheck.ps1` 检查文档系统一致性（根目录只留 `README.md`、front-matter、父索引覆盖、相对链接、体量上限）。规则见 `docs/00-meta/doc-rules.md`；
3. 删除旧 `build/`，执行全新 configure + Debug build；
4. 跑 `--jobtest`；
5. 跑 `--config-test`（TD-09 配置 v0->v2 迁移、备份、损坏/未来版本保护）；
6. 跑四种 `--selftest 120` 组合（Bloom / SSAO 开关）；
7. 跑 `--selftest 120 --cross-queue-test`（TD-02 跨队列回归，至少 20 帧）；
8. 跑 `--selftest 20 --compute-test`（TD-07 compute probe 独立回归）；
9. 跑 `--selftest 120 --resize-test`（TD-03 窗口缩放 / 动态分辨率 / 最小化恢复）；
10. 跑 `--selftest 120 --render-scale 0.5`（TD-03 启动即降分辨率）；
11. 跑 `--selftest 150 --material-stress`（TD-04 材质槽位延迟回收压力测试）；
12. 跑 `--selftest 20 --subresource-test`（TD-05 mip/layer + buffer 区间正向探针）；
13. 跑 `--subresource-error-test`（TD-05 写 mip0 后读 mip1 的反例检查）；
14. 每次运行都要求退出码为 0、stderr 字节数为 0。

任意一步失败都会返回非零退出码。日志在 `build/validation/<timestamp>/`。

## P0 调试 / 剖析基础设施

Validate.ps1 额外覆盖 3 项 P0 用例；并会对所有会创建 Vulkan 设备的用例追加一条全局断言：
stdout 必须出现 `[Validation] errors=0 warnings=0`（验证层消息不再写 stderr，改由 diag 模块统计）。

| 测试 | 目的 |
|---|---|
| `p0-build-info` | `--build-info`：报告本二进制编译进的 & 本次开启的调试能力 |
| `p0-diagnostics-selftest` | 诊断管线自检（合成消息 -> 计数 + JSONL 落盘） |
| `p0-gpu-timing` | 每 pass GPU 计时 -> `gpu_timing.csv` |

### 调试包：tools/Profile.ps1

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Profile.ps1 -Frames 300
```

一次调用产出 `build/profiles/<timestamp>/`：

- `env.txt`：`vulkaninfo --summary` 环境快照（优先 SDK 版 `vulkaninfoSDK.exe`）
- `gpu_timing.csv`：每 pass GPU 时间
- `run.stdout.log` / `run.stderr.log`（stderr 必须 0 字节）
- `manifest.json`：会话元数据 + 每 pass 平均 GPU 时间

可选开关：`-ValidationSync`（本次额外开同步校验）、`-SkipGpuTiming`（关闭 GPU 计时）。

### 引擎侧调试 flag（随用随开，默认关）

| flag | 作用 |
|---|---|
| `--build-info` | 打印能力清单后退出（不创建窗口）|
| `--gpu-timing` / `--gpu-timing-csv <path>` | 每 pass GPU 计时并写 CSV |
| `--validation-features bp,sync` | 启用验证层 best-practices / 同步校验 |
| `--validation-jsonl <path>` | 验证层消息写 JSONL（默认不落盘）|
| `--diagnostics-test [path]` | 诊断管线自检（合成消息）|

编译期开关见根 `CMakeLists.txt` 的 `TINY_ENABLE_*`；设计原则见 `DEBUG_PROFILE_PLAN.md` §1.5。

## P1 调试 / 剖析基础设施

Validate.ps1 现已覆盖 17 项（新增 `p1-screenshot`）。

### 无头截图 + 视觉回归（即可用，零外部依赖）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Screenshot.ps1 -Frame 60
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Screenshot.ps1 -Frame 60 -Golden build/screenshots/ref.png
```

产出 PNG；带 `-Golden` 时与金标准图比较（逐通道 mean 差），超过 `-MaxMeanDiff`（默认 2.0）退出非 0。

引擎侧：`--screenshot <path>` + `--screenshot-frame <n>`（不指定帧则自检取 `min(frames,60)`）。

### 一次性准备剖析依赖

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/SetupProfiling.ps1
```

下载 Tracy（源码 + 工具二进制）到 `3rdParty/tracy`，RenderDoc 的 `renderdoc_app.h` 到 `3rdParty/renderdoc`。
两个能力都默认编译期 OFF，不放置依赖也不影响默认构建（CMake 只 warning 并自动禁用）。

### Tracy 时间线

```powershell
cmake -S . -B build -DTINY_ENABLE_TRACY=ON
cmake --build build --config Debug
.\build\Debug\TinyEngine.exe --build-info    # 应显示 tracy=1
```

无人值守抓取：后台跑引擎，另一个终端 `3rdParty/tracy/bin/tracy-capture.exe -o s.tracy -s 6`；
再 `tracy-csvexport.exe -g s.tracy` 导出每个 GPU zone 事件（**`-g` 才是 GPU；默认只导 CPU 统计**）。

已实现：`src/Core/Profiler.h`（CPU zone / 帧标记）+ `src/RHI/GpuProfiler.*` + RenderGraph 里每 pass 的
GPU zone；配合 `TRACY_ON_DEMAND`，未连接 server 时不记录。

### RenderDoc 抓帧

```powershell
cmake -S . -B build -DTINY_ENABLE_RENDERDOC=ON
cmake --build build --config Debug
.\build\Debug\TinyEngine.exe --build-info    # 应显示 renderdoc=1
```

抓帧：`tools/Capture.ps1 -Frame 120`（内部 `renderdoccmd capture ... --capture-frame 120`）。
未注入 RenderDoc 时 `--capture-frame` 是安全空操作（已验证，stderr 0 字节）。

> **厂商提示**：NVIDIA 的 `nsys` / `ncu` 属于厂商专用工具，已降级为可选补充（见 `DEBUG_PROFILE_PLAN.md` §3 G 组），不进默认回归。默认调试链路（Tracy / RenderDoc / 验证层 / SPIRV-Tools / 截图）**厂商中立**，换 AMD/Intel 同样可用。

### 深度校验作业（厂商中立）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/DeepValidation.ps1 -Frames 60
```

一次跑四种验证层模式并汇总到 `build/deepvalidation/<timestamp>/summary.json`：

| mode | 内容 | 是否门禁 |
|---|---|---|
| `sync` | 同步校验（hazard）| **是**（errors 必须为 0）|
| `bp` | Best Practices（性能提示）| 否（只报告）|
| `gpuav` | GPU-AV（着色器越界）| 否（有 `WARNING-Setting-Limit-Adjusted` 信息性提示）|
| `printf` | Debug Printf 着色器日志 | 否 |

**独立作业**：不改变默认 `Validate.ps1` 基线（18 项），也不参与发布验收。

引擎侧 flag：`--validation-features gpuav` / `--device-fault-test`（`VK_EXT_device_fault` 能力自检）。

### 一键证据包（P3-1）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/DebugBundle.ps1 -Frames 150
```

产出 `build/bundles/<timestamp>/`（或 `-Name <x>`），把一次排查需要的证据全部打包：

| 内容 | 说明 |
|---|---|
| `manifest.json` | 机器可读索引：每步状态、验证层计数、每 pass GPU 均值、全部 artifact 列表 |
| `summary.md` | 人 / 大模型可读的概览 |
| `build_info.txt` | `--build-info`（本二进制编译进了哪些调试能力）|
| `env.txt` | `vulkaninfo --summary` 环境快照 |
| `run/` | 主运行：stdout / stderr / `gpu_timing.csv` / `screenshot.png` |
| `validation/` | `validation.jsonl`（仅当验证层真的产生消息时才创建）|
| `deep/` | 深度校验作业报告（sync / bp / gpuav / printf）|
| `baseline_diff/` | 与 `build/baseline/default` 的回归 diff |

可选开关：`-SkipDeepValidation`、`-SkipBaselineDiff`、`-SkipScreenshot`、`-DeepFrames <n>`。
`Profile.ps1` 是这个包的**轻量版**（只有 env + gpu timing）；要全量证据就用 `DebugBundle.ps1`。

### 基线 + 回归 diff（厂商中立）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Baseline.ps1 -Action Save -Frames 150
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Baseline.ps1 -Action Diff -Frames 150
```

一次采集同时覆盖三个维度，并写入 `build/baseline/<Name>/baseline.json` 与
`build/baseline/_current_<ts>/diff.json`：

| 维度 | 来源 | 判定 |
|---|---|---|
| GPU 时间 | `--gpu-timing` -> 每 pass 均值 | 超过 `max(MaxPassRegressionMs, base * MaxPassRegressionPct%)` 判回归 |
| 验证层 | stdout `[Validation] errors/warnings` | errors 增加、或 warnings 超过 `MaxWarningsIncrease` 判回归 |
| 视觉 | `--screenshot` -> 与基线 PNG 逐通道均值差 | 超过 `MaxMeanDiff` 判回归 |

要点：
- 前 `-WarmupFrames`（默认 30）帧会被剔除，否则冷启动升频会让基线系统性偏慢；
- 截图帧会自动钳制到 `-Frames` 以内；
- Debug 构建的绝对 ms 噪声大，这是**相对回归检测**，阈值故意放宽；
- `Validate.ps1` 末尾会跑一次 `Save`+`Diff` 自测（`p3-baseline-diff`）。

### 剖析构建回归

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1 -EnableProfiling
```

`-EnableProfiling` 把 `-DTINY_ENABLE_TRACY=ON -DTINY_ENABLE_RENDERDOC=ON` 传给 configure，
并额外跑一项 `p1-profiling-enabled`（共 18 项）。不加该开关即默认构建，17 项。

Bloom 关闭的 selftest 还会检查 `[TD06] Bloom disabled: skipping Bright + Down + Up passes`；
普通 selftest 会检查 `[TD07] ComputeProbe disabled: no probe dispatches or CPU readback`，
只有 `--compute-test` 才允许出现 D-5 probe 的 GPU 写/CPU 读回。

## Vulkan SDK

CMake 通过 `find_package(Vulkan REQUIRED)` 自动定位。推荐设置：

```powershell
$env:VULKAN_SDK = "D:\work\vulkan"
```

也可以显式传：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1 -VulkanSdk "D:\work\vulkan"
```

## 快速迭代

依赖已经准备好、只想增量构建时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1 -Incremental -SkipSetup -Frames 20
```

## 固定依赖版本

TD-03 的动态分辨率也可以在运行时通过 ImGui 的 `Render Scale` 滑杆调整，
或使用命令行 `--selftest N --render-scale 0.5`。

`SetupDependencies.ps1` 的 manifest 固定了下面这些版本/提交：

| 依赖 | 版本 |
|---|---|
| GLFW | 3.4 |
| GLM | 1.0.3 |
| Dear ImGui | 1.92.6 |
| stb | `f4a71b1` |
| tinyobjloader | v2.0.0 |
| VulkanMemoryAllocator | v3.4.0 |
| tinygltf | `b3e0d78` |
| EnTT | v3.16.0（C++17 兼容） |

`3rdParty/` 不入库，下载缓存放在 `3rdParty/.cache/`。可以用
`-DependenciesRoot <path>` 指向别的目录。`spdlog` 是可选依赖，当前引擎未启用。

## CMake 约定

- `SOURCES` 使用 `CONFIGURE_DEPENDS`，新增 `src/*.cpp` 后不再需要手动重新 configure。
- 所有必需头文件在 configure 阶段检查，缺依赖时会直接提示运行本工具。
- Shader 仍由 Vulkan SDK 的 `glslangValidator` 在构建前编译。

> **脚本约定**：`tools/*.ps1` 一律纯 ASCII。Windows PowerShell 5.1 读无 BOM 的 .ps1 时按 ANSI 解码，中文注释会吞掉下一行代码（本项目已踩过两次）。