---
id: build/build
title: 构建流程与隐形规则
type: build
scope: CMakeLists.txt, cmake/AssetPaths.h.in, tools/SpirvReflect.cpp
read-when: 每次构建；或新增源文件、改 CMake 选项
size: ~190 行
updated: 2026-11-26
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
.\\build\\Debug\\TinyEngine.exe
```

## 1.2 Ninja 构建（推荐：全量与增量都更快）

VS 生成器把 130+ 个 `.cpp` 全塞进 `tiny_core` 一个项目，MSBuild 只能逐文件串行
（`CMakeLists.txt` 里的 `/MP` 让 cl 在项目内部并行，只是缓解）。Ninja 对每条编译命令
建一个边，调度没有工程级开销，增量与无改动构建都快得多。

```powershell
# 一键：configure + /showIncludes 前缀自检 + 构建（默认 Debug）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Target TinyEditor

# 手动等价（CMakePresets.json 提供 ninja / vs2022 两套预设）
cmake --preset ninja
cmake --build --preset ninja-debug
```

产物在 `build-ninja/Debug/`（`TinyEngine.exe` / `TinyEditor.exe`），运行方式不变。
VS 生成器的 `build/` 继续可用，两者互不干扰；`build-*/` 已在 `.gitignore`。
Ninja 预设还开了 `CMAKE_EXPORT_COMPILE_COMMANDS`，`build-ninja/compile_commands.json`
可给 clangd / clang-tidy 用。

各生成器、各开关的实测数据（全量 / 无改动 / 改核心头 / shader）统一记在 [compile-speed](compile-speed.md)，这里不再重复。
PCH 默认开启（`src/pch.h`，`-DTINY_PCH=OFF` 关闭）。

**中文区域设置（CP936）下的坑**：cl 的 `/showIncludes` 输出是 UTF-8 的
「注意: 包含文件: 」，CMake 3.31 探测该前缀时按控制台代码页解码，在 CP936 控制台里
会写成乱码进 `rules.ninja`，Ninja 便一条依赖都匹配不上 —— 症状是**改头文件后不重编**、
`ninja -C build-ninja -t deps` 里全是 `#deps 0`。`CMakeLists.txt` 的配置阶段已自行
探测并修正（见第 3 节第 5 条），`tools/build.ps1` 每次构建前还会再校验一次。

## 1.5 两个构建产物

| 目标 | 产物 | 用途 |
|---|---|---|
| `TinyEngine` | `build/<Config>/TinyEngine.exe` | 运行时模式 |
| `TinyEditor` | `build/<Config>/TinyEditor.exe` | 编辑器模式（默认等价于 `--editor`） |

共享源码编进 `tiny_core` 对象库，两个 exe 复用同一批 `.obj`，
只有 `main.cpp` 各编一次 —— 构建时间不会翻倍。

Shader 由 `TinyShaders` 自定义目标统一编译一次。它用的是 **OUTPUT 依赖**
而不是原来的 `PRE_BUILD`：后者每次构建都会重编全部 shader，前者只重编改动过的那个。

只构建其中一个：

```
cmake --build build --config Debug --target TinyEditor
```


## 2. 构建期发生的事

1. **依赖检查**：8 个必需文件逐个 `EXISTS` 判断。
2. **shader 编译**：每个 `shaders/*.{vert,frag,comp}` 编译成
   `build/shaders/<name>.spv`，随后 `spirv-val` 静态校验。
3. **反射头生成**：`tools/SpirvReflect` 读同一个 `.spv`，写出
   `build/shaders/<name>.reflect.h`（set / binding / 块大小与成员偏移）。
   `build/shaders` 在 `tiny_core` 的 include 路径上，引擎源码直接 `#include`；
   `add_dependencies(tiny_core TinyShaders)` 保证先生成再编译。详见
   [shaders](../20-pipeline/shaders.md) 第 6 节。
4. **资源路径注入**：`cmake/AssetPaths.h.in` -> `build/generated/AssetPaths.h`，
   把 `Resources/` 的**绝对路径**写进头文件。
5. **全局编译选项**：`/utf-8`（源码与执行字符集都是 UTF-8）、
   `TINYGLTF_NO_STB_IMAGE` / `TINYGLTF_NO_STB_IMAGE_WRITE`。

## 3. 五条隐形规则（都会导致"看起来莫名其妙的错误"）

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
所以从任何目录启动 `TinyEngine.exe` 都能找到 `Resources/`。

这也意味着：**移动/重命名仓库目录后必须重新 configure**（绝对路径变了）。

### 3.3 源码必须 UTF-8 无 BOM

- MSVC 用 `/utf-8` 编译，中文注释/字符串没问题。
- **带 BOM 的 shader 会编译失败**（见 [shaders](../20-pipeline/shaders.md) §4）。
- 不要用 PowerShell 的 `Set-Content` / `Out-File` 改源码——会按 ANSI 重编码或加 BOM。

### 3.4 改过 `RHI/Device.h` 这类"到处被 include"的头之后，必须 `--clean-first`

增量构建只看时间戳。往 `Device` 里加一个成员（例如 `std::unique_ptr`）会让**所有
包含该头的翻译单元**`sizeof(Device)` 变化；只要有一个 `.obj` 没重编，链接出来的
程序里就存在两套布局。

现象是最难查的那一类：不是编译错误，而是**运行时崩在第一个 Vulkan 调用**
（`checkValidationLayerSupport()` 里的 `vkEnumerateInstanceLayerProperties`），
退出码在 `0xC0000374`（堆损坏）和 `0xC0000005`（访问违例）之间摇摆，
而 `git stash` 回改动前一切正常 —— 很容易误判成「新写的代码有 bug」。

```powershell
cmake --build build --config Debug --clean-first
```

改过 `src/RHI/Device.h` / `src/RHI/VmaConfig.h`（VMA 配置相关）之后无条件走这条。
### 3.5 Ninja 下改头文件却不重编（`/showIncludes` 前缀被代码页弄坏）

判据：改了一个头文件，构建 0.1s 就结束、没有任何重编；`ninja -C build-ninja -t deps`
里所有 `.obj` 都是 `#deps 0`。此时二进制是旧的，但日志显示"成功"。

原因与修法见 1.2 节末尾：CMake 在 CP936 控制台里把 cl 的 UTF-8「注意: 包含文件: 」
按错误代码页解码后写进 `rules.ninja`。`CMakeLists.txt` 现在在 configure 阶段自己探测
真实字节再写入，`tools/build.ps1` 每次构建前还会核对一次；两者都没生效时，删掉
`build-ninja/` 重新 configure 即可恢复。

### 3.6 换目录与热重载（都不需要重新 configure）

`AssetPaths.h` 里的绝对路径仍然是**第一优先**：文件在，行为和以前完全一样。
只有在它不存在时，`tiny::assetpath::resolve()` 才按 `/shaders/`、`/Resources/`、
`/config/` 这些锚点把前缀剥掉，拼到「exe 目录 → exe/../ → exe/../../ → cwd」探到的根上。
所以：

- 换了机器或挪了构建产物：直接跑，会自己回溯；用 `--print-assets` 看它到底找了哪三个目录。
- 改了 `.frag` / `.comp`：正常增量构建会重编出新的 `.spv`；引擎带 `--hot-reload` 跑时，
  下一帧发现 mtime 变了就重建对应管线，不用重启。**改了 binding 数量或附件格式要重启**
  （管线布局不在重建范围内）。

## 4. CMake 选项

| 选项 | 默认 | 作用 |
|---|---|---|
| `TINY_VMA_LEAK_CHECK` | OFF | VMA 销毁时逐条打印未释放的分配（排查泄漏用，会变慢） |
| `TINY_ENABLE_GPU_TIMING` | ON | 编译入 `--gpu-timing` |
| `TINY_ENABLE_VALIDATION_FEATURES` | ON | 编译入 `--validation-features` |
| `TINY_ENABLE_BUILD_INFO` | ON | 编译入 `--build-info` |
| `TINY_ENABLE_SCREENSHOT` | ON | 编译入 `--screenshot` |
| `TINY_ENABLE_TRACY` | **OFF** | 编译入 Tracy（需 `3rdParty/tracy`） |
| `TINY_ENABLE_RENDERDOC` | **OFF** | 编译入 RenderDoc 抓帧（需 `renderdoc_app.h`） |
| `TINY_PCH` | **ON** | 预编译 `src/pch.h`（std + glm + vulkan），压全量与改核心头的重编；`-DTINY_PCH=OFF` 关闭 |

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
| `-EnableProfiling` | 用 `-DTINY_ENABLE_TRACY=ON -DTINY_ENABLE_RENDERDOC=ON` 配置，多跑一项 |
| `-Frames N` | selftest 帧数（默认 120） |
| `-BuildDir` / `-Config` / `-CMakePath` | 覆盖路径 |
