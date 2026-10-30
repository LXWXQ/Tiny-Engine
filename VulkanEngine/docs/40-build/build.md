---
id: build/build
title: 构建流程与隐形规则
type: build
scope: CMakeLists.txt, cmake/AssetPaths.h.in
read-when: 每次构建；或新增源文件、改 CMake 选项
size: ~170 行
updated: 2026-10-30
---

# 构建流程与隐形规则

## 1. 标准命令

```powershell
# 依赖（只需一次）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/SetupDependencies.ps1

# 配置 + 构建
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug

# 运行
.\\build\\Debug\\VulkanEngine.exe
```

## 2. 构建期发生的事

1. **依赖检查**：8 个必需文件逐个 `EXISTS` 判断。
2. **shader 编译**：每个 `shaders/*.{vert,frag,comp}` 编译成
   `build/shaders/<name>.spv`，随后 `spirv-val` 静态校验。
3. **资源路径注入**：`cmake/AssetPaths.h.in` -> `build/generated/AssetPaths.h`，
   把 `Resources/` 的**绝对路径**写进头文件。
4. **全局编译选项**：`/utf-8`（源码与执行字符集都是 UTF-8）、
   `TINYGLTF_NO_STB_IMAGE` / `TINYGLTF_NO_STB_IMAGE_WRITE`。

## 3. 三条隐形规则（都会导致"看起来莫名其妙的错误"）

### 3.1 新增 `src/*.cpp` 必须重新 configure

```cmake
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
```

虽然用了 `CONFIGURE_DEPENDS`，但**最稳的做法**是新增 `.cpp` 后重跑一次 configure。
不这么做会在**链接期**才报 undefined symbol（而不是编译期），很难联想到是没 configure。
这是本工程**最高频**的构建坑。

**新增 shader 不需要**重新 configure（源文件 glob + PRE_BUILD）。

### 3.2 资源路径不依赖工作目录

引擎通过生成的 `AssetPaths.h` 拿绝对路径，
所以从任何目录启动 `VulkanEngine.exe` 都能找到 `Resources/`。

这也意味着：**移动/重命名仓库目录后必须重新 configure**（绝对路径变了）。

### 3.3 源码必须 UTF-8 无 BOM

- MSVC 用 `/utf-8` 编译，中文注释/字符串没问题。
- **带 BOM 的 shader 会编译失败**（见 [shaders](../20-pipeline/shaders.md) §4）。
- 不要用 PowerShell 的 `Set-Content` / `Out-File` 改源码——会按 ANSI 重编码或加 BOM。

## 4. CMake 选项

| 选项 | 默认 | 作用 |
|---|---|---|
| `SENTINEL_VMA_LEAK_CHECK` | OFF | VMA 销毁时逐条打印未释放的分配（排查泄漏用，会变慢） |
| `SENTINEL_ENABLE_GPU_TIMING` | ON | 编译入 `--gpu-timing` |
| `SENTINEL_ENABLE_VALIDATION_FEATURES` | ON | 编译入 `--validation-features` |
| `SENTINEL_ENABLE_BUILD_INFO` | ON | 编译入 `--build-info` |
| `SENTINEL_ENABLE_SCREENSHOT` | ON | 编译入 `--screenshot` |
| `SENTINEL_ENABLE_TRACY` | **OFF** | 编译入 Tracy（需 `3rdParty/tracy`） |
| `SENTINEL_ENABLE_RENDERDOC` | **OFF** | 编译入 RenderDoc 抓帧（需 `renderdoc_app.h`） |

重量级 / 带外部依赖的能力默认 OFF，缺失时 CMake 只 warning 并自动禁用，不影响默认构建。

## 5. 构建验收

- **零警告**。`tools/Validate.ps1` 会检查输出里是否出现
  `warning C####` / `warning LNK####` / `warning MSB####`。
- `git` 提示 `LF will be replaced by CRLF` **不是问题**（Windows checkout 的正常行为）。

## 6. 一键回归

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1
```

| 参数 | 作用 |
|---|---|
| `-Incremental` | 不删 `build/`（默认会删掉全新构建） |
| `-SkipSetup` | 跳过依赖检查 |
| `-EnableProfiling` | 用 `-DSENTINEL_ENABLE_TRACY=ON -DSENTINEL_ENABLE_RENDERDOC=ON` 配置，多跑一项 |
| `-Frames N` | selftest 帧数（默认 120） |
| `-BuildDir` / `-Config` / `-CMakePath` | 覆盖路径 |
