---
id: build/dependencies
title: 第三方依赖
type: build
scope: tools/SetupDependencies.ps1, CMakeLists.txt, 3rdParty/
read-when: 换机器、加依赖、或 CMake 报"缺少第三方依赖文件"
size: ~120 行
updated: 2026-10-30
---

# 第三方依赖

## 1. 固定版本表

`3rdParty/` **不入库**（体积大）。`tools/SetupDependencies.ps1` 按固定版本下载安装。

| 依赖 | 版本 | 目标目录 | 关键文件 |
|---|---|---|---|
| GLFW | 3.4 | `glfw/` | `CMakeLists.txt`, `include/GLFW/glfw3.h` |
| GLM | 1.0.3 | `glm/` | `glm/glm.hpp` |
| ImGui | 1.92.6-docking | `imgui/` | `imgui.cpp`, `imgui_internal.h`, `backends/imgui_impl_vulkan.cpp` |
| stb | `f4a71b1` | `stb/` | `stb_image.h` |
| tinyobjloader | v2.0.0 | `tinyobjloader/` | `tiny_obj_loader.h` |
| VulkanMemoryAllocator | v3.4.0 | `VulkanMemoryAllocator/` | `vk_mem_alloc.h` |
| tinygltf | `b3e0d78` | `tinygltf/` | `tiny_gltf.h`, `json.hpp` |
| EnTT | **v3.16.0** | `entt/` | `src/entt/entt.hpp` |

可选（默认**不**装、不编译）：

| 依赖 | 用途 | 获取方式 |
|---|---|---|
| Tracy | CPU/GPU 时间线 | `tools/SetupProfiling.ps1` + `-DTINY_ENABLE_TRACY=ON` |
| `renderdoc_app.h` | in-app 抓帧 | `tools/SetupProfiling.ps1` + `-DTINY_ENABLE_RENDERDOC=ON` |
| spdlog | 日志库（**当前未使用**） | 已配 include，但一行未用 |

## 2. 目录布局的坑（EnTT）

CMake 期望 **EnTT v3.16.0 的源码布局**：

```
3rdParty/entt/src/entt/entt.hpp     <- 必需
```

而 EnTT 的 GitHub 归档里同时有 `src/entt/` 和 `single_include/entt/`。
如果不小心把 `single_include` 的内容铺到了 `3rdParty/entt/`（变成
`3rdParty/entt/entt/entt.hpp`），CMake 会在配置阶段报：

```
缺少第三方依赖文件：.../3rdParty/entt/src/entt/entt.hpp
```

修法：重跑 `tools/SetupDependencies.ps1`（它会按 `SourceEntry = "src/entt/entt.hpp"`
定位正确的子目录）。

**顺带提醒**：EnTT v4 的目录布局与 v3 不同，且 API 有破坏性改动，不要随手升。

## 3. CMake 的依赖检查

`CMakeLists.txt` 在 configure 阶段逐个检查关键文件是否存在，缺失时给出**可直接执行**的修复命令：

```
缺少第三方依赖文件：${DEPENDENCY_FILE}
请从仓库根目录执行：
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/SetupDependencies.ps1
```

这样避免等到编译期才报 `cannot open include file`。

## 4. Vulkan SDK

通过 `find_package(Vulkan REQUIRED)` 定位，**不硬编码路径**。

找不到时设置环境变量：

```powershell
$env:VULKAN_SDK = "D:\work\vulkan"     # 注意：是 SDK 根目录，不是 ...\Bin
```

CMake 还会从 SDK 的 CMake 包里取 `glslangValidator`，并在同目录找 `spirv-val`。

## 5. Resources

`Resources/` 同样不入库，需单独准备：

| 内容 | 用途 |
|---|---|
| `Models/Sponza/glTF/Sponza.gltf` | 主场景 |
| `Models/DamagedHelmet/` | 备用模型 |
| `Texture/environment.hdr` | 环境光照 |
| `Models/sphere.obj` 等 | 测试几何 |

缺失时引擎启动会失败或退化——具体行为见 [troubleshooting](troubleshooting.md)。
