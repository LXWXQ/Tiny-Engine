---
id: build/compile-speed
title: 编译速度：瓶颈、优化开关与踩坑
type: build
scope: CMakeLists.txt, CMakePresets.json, src/pch.h, tools/build.ps1
read-when: 觉得编译慢、要调并行度、改 src/pch.h、或引入新的第三方头
size: ~150 行
updated: 2026-11-26
---

# 编译速度：瓶颈、优化开关与踩坑

这份文档回答一个问题：**为什么慢、慢在哪、动了什么、以后怎么保持快**。
用法层面的东西（跑哪条命令、产物在哪）在 [build](build.md)，本文只讲"速度"。

## 1. 一张表看现状

Ryzen 7 9700X（8 核 16 线程），MSVC 14.41，同一台机器、同一天、冷热缓存尽量对齐：

| 场景 | 改前（串行） | VS + `/MP` | Ninja | Ninja + PCH |
|---|---|---|---|---|
| 全量 Debug | 103.5s | 26.2s | 16.5s | **14.1s** |
| 无改动（空构建） | 0.8s | 0.8s | 0.06s | 0.06s |
| 改 `main.cpp` | 5.0s | 5.0s | 1.8s | 1.8s |
| 改 `src/Core/Diagnostics.h`（78 个 TU） | 75.7s | 16.6s | 11.4s | **9.1s** |
| 改 `src/pch.h`（141 个 obj） | — | 全量 | 全量 | 11.1s |
| 54 个 shader 全量 | 4.6s | 4.6s | 0.9s | 0.9s |
| 单改一个 shader | 4.6s | 4.6s | 0.16s | 0.16s |

Release 的对照：全新 build，VS + `/MP` 24.9s、Ninja 27.3s —— `/O2` 把时间吃掉了，
生成器的差异被摊薄，这也是"优化只对 Debug 效果明显"的原因。

一句话：**Debug 全量 103.5s → 14.1s（7.3×），日常改核心头 ~9s、改一个 .cpp 1.8s**。

## 2. 慢在哪（诊断结论）

四条，按贡献从大到小：

1. **一个项目装 140 个 `.cpp`**。全部引擎源码都在 `tiny_core` 这一个 OBJECT 库里，
   生成的是一个 vcxproj；MSBuild 的 `/m`（工程级并行）因此完全用不上，只剩文件级串行。
2. **没有 `/MP`**。改前 cl 一次只吃一个源文件，16 个逻辑核只有 1 个在动。
3. **头文件很重**。`src/Scene/Scene.cpp` 6295 行，预处理后 294,663 行（46.8×）；
   `VmaConfig.h` / `Core/Diagnostics.h` / `DeviceQuirks.h` 各被 76–77 个 TU 包含，
   `RHI/Device.h` 75 个（它直接拉进 `vulkan/vulkan.h`、`GLFW/glfw3.h`、`vk_mem_alloc.h`）。
4. **没有 PCH**。entt / glm / GLFW / vulkan / vk_mem_alloc / imgui 在每个 TU 里各解析一遍；
   实测一个"只含这些头 + 空函数"的 TU 要 0.64s，而空的 TU 只要 0.05s —— 平均每 TU 的
   0.6s 里绝大部分是第三方头。

顺带纠正一个直觉：**Debug 本身并不比 Release 慢**。改前全量 Debug 103.5s、Release 124.2s
（`/O2` 更贵）。"Debug 特别慢"的体感来自"改一行 → 重编几十个 TU → 串行跑完"的放大效应。

## 3. 三个开关

### 3.1 `/MP`：VS 生成器的文件级并行（默认开）

`CMakeLists.txt` 里：

```cmake
# Ninja 自己就会并行，不需要 /MP
if(MSVC AND NOT CMAKE_GENERATOR MATCHES "Ninja")
    add_compile_options(/MP)
endif()
```

CMake 会把它翻译成 vcxproj 的 `<MultiProcessorCompilation>true</MultiProcessorCompilation>`。
注意：`cmake --build . -- /p:MultiProcessorCompilation=true` 这种**全局属性传不进** item 定义，
实测无效（93.3s vs 改前 93.3s），别浪费时间。

### 3.2 Ninja：日常首选

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Target TinyEditor -Jobs 12
```

为什么快：

- 一条编译命令一个边，调度没有工程开销；`ZERO_CHECK` + `VerifyGlobs` 那 0.8s 的固定税也没了。
- shader 的 54 条 `add_custom_command` 天然并行，全量 4.6s → 0.9s。
- 增量判定的粒度是文件，不是项目。

脚本多做三件事：`chcp 65001`、自适应找 ninja/cl、**每次构建前校验 `/showIncludes` 前缀**
（见第 5 节第 1 条）。等价的手动命令是 `cmake --preset ninja` + `cmake --build --preset ninja-debug`。

### 3.3 PCH：压全量与"改核心头"这两条最痛的路径（默认开）

`src/pch.h` 预编译的是**跨项目公共头**：20 个 std 头 + `vulkan/vulkan.h` + glm 4 个头。
收益：全量 16.5s → 14.1s，改 `Core/Diagnostics.h` 11.4s → 9.1s。

三条排除（这些头**不能**进 PCH，否则实现体在 140 个 TU 里重复定义）：

- `stb_image.h` —— `Application.cpp` 里定义了 `STB_IMAGE_IMPLEMENTATION`
- `tiny_gltf.h` —— `GLTFLoader.cpp` 里定义了 `TINYGLTF_IMPLEMENTATION`
- `vk_mem_alloc.h` —— `VMA_IMPLEMENTATION`，且对 Vulkan 头的包含顺序敏感

**为什么 pch.h 里要先 define 两个 GLM 宏**：`GLM_FORCE_RADIANS` /
`GLM_FORCE_DEPTH_ZERO_TO_ONE` 目前散落在 `src/Editor/Camera.h`、`src/Scene/Model.h`、
`src/Scene/ViewModel.h` 里各自 `#define`。PCH 先于这些头展开，若不在这里先设好，
glm 会按默认的"深度 -1..1"实例化，与工程其余部分的 0..1 约定冲突 —— 症状是投影矩阵、
阴影深度整体错位，且**只有在用了 PCH 的构建里才出错**。

关掉：`cmake -S . -B build-ninja -DTINY_PCH=OFF`（或给 `tools/build.ps1` 传 `-Reconfigure` 前手动 configure）。
改 `src/pch.h` 会让 141 个 obj 全量重编（11.1s），所以只往里放稳定的第三方头。

### 3.4 还没做的

- **拆库**：把 `tiny_core` 拆成 render / scene / rhi 几个静态库，MSBuild 就能工程级并行；
  代价是所有 include 路径和链接顺序要重排，属于结构性改动。
- **头文件瘦身**：`RHI/Device.h` 同时导出 vulkan / GLFW / VMA 三套类型，任何包含它的 TU
  都得吃全量；可行方向是拆出 `DeviceFwd.h`，或对 75 个使用者做前置声明。
- **IWYU**：`Scene/Components.h`（entt + glm）被 53 个 TU 包含，多数只需要其中的 POD 结构。

## 4. 自己怎么测

```powershell
# 全量（先删目录，避免把增量算成全量）
Remove-Item build-ninja -Recurse -Force
Measure-Command { powershell -NoProfile -File tools/build.ps1 }

# 增量：改 mtime 就够，不用真改内容
(Get-Item src\Core\Diagnostics.h).LastWriteTime = Get-Date
Measure-Command { powershell -NoProfile -File tools/build.ps1 }
```

- 单 TU 的时间用 `cl /c` 直接量（`cl /nologo /c /showIncludes x.cpp`），比过一遍 msbuild 干净。
- 记录时写清机器、配置、是否冷缓存；Defender 实时保护开着时结果会抖 ±10%。
- 不要混用 `--clean-first` 测增量；VS 生成器的 `--clean-first` 不保证清掉 PCH 中间件，
  要大改时直接删目录。

## 5. 踩过的坑

1. **CP936 下 `/showIncludes` 前缀被弄坏 → 改头文件不重编**。cl 输出的是 UTF-8 的
   「注意: 包含文件: 」，CMake 3.31 探测前缀时按控制台代码页解码，写进 `rules.ninja`
   就成了乱码，ninja 一条依赖都匹配不上（`ninja -C build-ninja -t deps` 全是 `#deps 0`）。
   现在 `CMakeLists.txt` 在配置阶段自己探测并修正，`tools/build.ps1` 每次构建前再校验一次。
   完整机理见 [build](build.md) 第 3 节第 5 条。
2. **`rules.ninja` 会消失**。CMake 3.31 把它生成在 `build-ninja/CMakeFiles/rules.ninja`，
   某些 ninja 清理动作会删掉它，随后报 `loading 'CMakeFiles\rules.ninja': 系统找不到指定的文件`
   —— 重跑一次 configure 即可。
3. **PCH 会掩盖依赖**。开了 PCH 后单个 obj 的 `#deps` 从几十条掉到个位数（第三方头被 PCH 吸收），
   这是正常的；但也意味着新代码可能"偷偷"依赖 PCH 里的头，`-DTINY_PCH=OFF` 时才暴露。
   所以 pch.h 只放跨项目公共头，别放工程自己的头。
4. **PowerShell 5.1 没有 `[Text.Encoding]::Latin1`**。按字节读 `rules.ninja` 必须用
   `[System.Text.Encoding]::GetEncoding(28591)`，否则整条命令静默失败。
5. **别整文件打印 `.ninja`**。`impl-Debug.ninja` 有 350KB，一次输出几十万字符会把自己淹掉，
   要查就 `Select-String` 定位行号后再取那几行。

## 6. 什么时候该回来重测

- 增删一批 `.cpp`（TU 数变了）；改 `CMakeLists.txt` 的编译选项；
- 换机器 / 升 VS / 升 CMake（前缀探测行为和 ninja 布局都可能变）；
- `src/pch.h` 大改；引入新的重量级第三方头（比如换掉 entt、加 Jolt）。

测完把第 1 节的表更新掉，别让文档比现实旧。