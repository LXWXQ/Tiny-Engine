---
id: build/troubleshooting
title: 构建与运行故障排查
type: build
scope: 空（症状 -> 原因 -> 修法）
read-when: 构建失败、启动失败、或回归莫名失败时
size: ~180 行
updated: 2026-10-30
---

# 故障排查

按症状查。每条给"现象 / 原因 / 修法"。

## 1. CMake 配置阶段

### 1.1 `Could NOT find Vulkan (missing: Vulkan_LIBRARY Vulkan_INCLUDE_DIR)`

- **原因**：`VULKAN_SDK` 没设，或设成了 `...\Bin` 而不是 SDK 根目录。
- **修法**：`$env:VULKAN_SDK = "D:\work\vulkan"`（根目录，含 `Include/` 与 `Lib/`）。

### 1.2 `缺少第三方依赖文件：.../3rdParty/entt/src/entt/entt.hpp`

- **原因**：本地 EnTT 是 v4 布局或 single_include 布局。
- **修法**：`powershell -File tools/SetupDependencies.ps1` 重装 v3.16.0。
  详见 [dependencies](dependencies.md) §2。

### 1.3 `找不到 glslangValidator`

- **原因**：Vulkan SDK 未安装，或 SDK 里没有 glslang。
- **修法**：装完整 Vulkan SDK；或 `-DGLSL_VALIDATOR=<绝对路径>`。

## 2. 编译 / 链接阶段

### 2.1 shader 编译报 `'#' : unexpected token` 或 `ES shaders for SPIR-V require version 310 or higher`

- **原因**：shader 文件带 **UTF-8 BOM**，`#version` 没被识别。
- **修法**：去掉前 3 个字节。检查脚本见 [shaders](../20-pipeline/shaders.md) §4。
- **预防**：不要用 PowerShell `Set-Content` / `Out-File` 写 shader。

### 2.2 链接期 `unresolved external symbol`（新加了 `.cpp`）

- **原因**：没重新 configure，源文件 glob 没更新。
- **修法**：重跑 `cmake -S . -B build ...`。

### 2.3 一屏看不懂的 C++ 错误（中文注释处）

- **原因**：源码被按 ANSI 重编码，中文变乱码导致从乱码处开始一路语法错。
- **修法**：`git checkout` 恢复该文件，用编辑器（UTF-8 无 BOM）重新修改。

## 3. 运行期

### 3.1 `GPU 不支持 maintenance4；无法使用 SPIR-V 1.6 的 LocalSizeId 执行模式`

- **原因**：GPU / 驱动过老，不支持 Vulkan 1.3 核心的 `maintenance4`。
- **修法**：换驱动；或把 compute shader 的编译目标降到 `--target-env vulkan1.2`
  （会退回 `WorkgroupSize` 内建，但 best-practices 会报 2 条弃用 warning）。

### 3.2 VMA 断言 `Some allocations were not freed before destruction of this memory block!`

- **原因**：销毁顺序错，有资源在 `vmaDestroyAllocator` 之后才释放。
- **修法**：检查 `~FirstApp` 的显式销毁顺序，见
  [resource-lifetime](../10-architecture/resource-lifetime.md) §3。
- **定位**：`-DTINY_VMA_LEAK_CHECK=ON` 重新构建，日志会打印未释放分配的名字。

### 3.3 `[Config] config selfTest FAIL: cannot create temp directory`

- **原因**：进程无法写入 `%TEMP%`（受限沙箱 / 权限）。
- **修法**：把 `TEMP` 与 `TMP` 指到可写目录再跑 `--config-test`。

### 3.4 窗口起来但黑屏 / 花屏

按顺序查：

1. `--validation-features sync` 有没有 hazard；
2. `--validation-jsonl out.jsonl` 看验证层具体报什么；
3. 确认 `Resources/` 里的模型与 HDR 存在且路径对（`AssetPaths.h` 是 configure 时写死的）；
4. RenderDoc 抓帧看绑定与附件（见 [tools](../60-debug-profiling/tools.md)）。

### 3.5 回归 "stderr 不为 0 字节"

- **原因**：某处 `std::cerr` 输出了。验证层回调**已不再写 stderr**，
  所以多半是 `GLTFLoader` 警告、`GeometrySystem` 的"没有有效材质槽位"之类。
- **修法**：看 `build/validation/<ts>/tests/<name>.stderr.log` 的具体内容。

## 4. 回归脚本

### 4.1 `p3-baseline-diff` / `p3-debug-bundle` 报 `Processing -File 'C:\Users\Admin\Desktop\Vs' failed`

- **原因**：仓库路径含空格，而 `Start-Process -ArgumentList` **不会**给数组元素加引号。
- **修法**：已修 —— 6 个工具脚本都有 `Quote-NativeArg` 辅助函数，
  传路径参数前必须过它。新增脚本时照做。

### 4.2 `no baseline '_selftest' at ...`

- **原因**：`Baseline.ps1 -Action Diff` 之前没跑过 `-Action Save`，或 Save 步骤失败了。
- **修法**：先看 `p3-baseline-save.err` 的内容。

### 4.3 双击启动闪退，什么都看不到


- **先看**：可执行文件旁边的 `startup-error.log`（每次启动失败追加一段，含时间、阶段、
  消息、工作目录与提示）。日志里拿去和 `build/validation/<时间戳>/` 对照。
- **已覆盖**：缺 Vulkan 1.3 / 必需特性、创建窗口失败、创建设备失败等一切从 `main` 抛出的致命错误；
  设备丢失且驱动未暴露 `VK_EXT_device_fault` 时也会写这个文件并打印原因与下一步。
- **仍未覆盖**：没有原生 MessageBox —— 用户必须自己知道去翻这个文件。

### 4.4 窗口比屏幕大，状态栏/关闭按钮够不着


- **行为**：启动时按主显示器工作区的 92% 收敛窗口尺寸，日志里有
  `[Window] sizing | workarea=... scale=... -> WxH (fits|clamped)`。
- **1366x768 笔记本** → 窗口收敛到 1177x662；**150% 缩放**时工作区只有 1280x720，同样收敛。
- **仍 open**：字体固定 16px，不随 DPI 缩放（界面字偏小）。

## 5. 排查顺序（推荐）

1. `--build-info` 确认二进制里到底编译进了什么；
2. `tools/DebugBundle.ps1` 拿一份全量证据；
3. 按 [debug/tools](../60-debug-profiling/tools.md) 补专项证据；
4. 改完跑 `tools/Validate.ps1`；
5. 涉及性能/画面时跑 `tools/Baseline.ps1 -Action Diff`。
