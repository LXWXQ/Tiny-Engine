---
id: architecture/layering
title: 分层与依赖方向
type: architecture
scope: src/Core, src/RHI, src/Render, src/Scene
read-when: 不确定新代码该放哪一层，或要判断某个改动的影响范围
size: ~140 行
updated: 2026-10-30
---

# 分层与依赖方向

## 1. 四层

```
src/
├── Core/     应用外壳：生命周期、配置、描述符管理、JobSystem、ImGui、诊断
├── RHI/      图形后端：Device / Renderer / Swapchain / RenderGraph / Pipeline / Texture
├── Render/   渲染实现：各个 Pass、IBL、渲染资源表
└── Scene/    场景数据：ECS、相机、模型加载、空间索引
```

| 层 | 职责 | 不该做的事 |
|---|---|---|
| `Core` | 组装、帧循环、用户输入、配置持久化 | 直接写 `vkCmd*` 渲染命令（除诊断/截图这类横切能力） |
| `RHI` | Vulkan 对象封装、同步、资源生命周期 | 知道"什么是 Sponza"、不知道材质语义 |
| `Render` | 具体渲染算法与 Pass 编排 | 直接创建 `VkDevice` 级对象（走 RHI） |
| `Scene` | 世界数据与 CPU 侧计算 | 触碰任何 Vulkan 对象 |

## 2. 依赖方向

```
        Core
       /    \
      v      v
   Render -> RHI
      |
      v
    Scene        （Scene 不依赖任何 Vulkan 类型）
```

规则：

- **`Scene` 零 Vulkan 依赖**。它只产出数据（`Model` 里的 GPU 句柄除外——那是 Resource 层
  妥协的结果，见 `Model.h` 的注释）。
- **`Render` 可以依赖 `RHI` 和 `Scene`**，反过来不行。
- **`RHI` 不认识 `Render`/`Scene` 的具体类型**（`RenderGraph` 只认句柄和访问声明）。
- **`Core` 是唯一的组装者**，`Application` 构造函数按固定顺序把四层连起来。

## 3. `Application` 的创建顺序（硬依赖）

`FirstApp` 构造函数有 10 个步骤，其中至少 4 条是**顺序错了立刻崩或静默失效**的：

| 顺序 | 步骤 | 为什么必须是这个位置 |
|---|---|---|
| 1 | `vulkanDevice` | 一切的前提 |
| 2 | `renderGraph` | Pass 与资源都挂在它上面 |
| 3 | `renderPipeline` + 各 Pass | Pass 在 `init` 里创建自己的资源和管线 |
| 4 | `descriptorManager->init()` | 池容量按材质数 + Bloom 级数算 |
| 5 | `scene` 加载 | 产生材质，材质要分配 bindless 槽位 |
| 6 | `IBLBaker` 烘焙 | 产生 irradiance/prefilter/BRDF LUT 三张图 |
| 7 | `buildGlobalSet()` | **必须在 3/4/6 之后**：Set0 要写 SSAO 输出、G-Buffer 视图、IBL 三张图 |
| 8 | `PresentPass` 与 `ImGuiSystem` | ImGui 后端要拿到交换链格式与渲染上下文 |
| 9 | `screenshotCapture.init()` | 需要设备与 VMA |
| 10 | 首次 `loadConfig` + 应用相机位姿 | 配置在资源就绪后应用，避免半初始化状态 |

> 已知问题：构造函数长约 120 行，`Application.h` 因此包含了一堆头。
> 改进方向在 [weak-points](weak-points.md) 里登记，但**改动前请保持顺序不变**。

## 4. 横切能力放在哪

| 能力 | 位置 | 理由 |
|---|---|---|
| 验证层回调 | `RHI/Device.cpp` -> `Core/Diagnostics.cpp` | 回调在 Device 创建时注册，但**绝不写 stderr**，只交给 diag 计数 |
| GPU 计时 | `RHI/GpuTimer` + `RenderGraph` 挂钩 | 计时是同步点，必须由渲染图在正确位置插 timestamp |
| 截图 | `RHI/ScreenshotCapture`（由 `Core/Application` 驱动） | 要读交换链，属 RHI 能力；触发时机属应用逻辑 |
| 配置 | `Core/EngineConfig` | 纯 CPU 侧，与 Vulkan 无关 |
| JobSystem | `Core/JobSystem` | 与图形无关的通用 CPU 池 |

## 5. 判断"我的代码该放哪"的流程

1. 需要 `vkCmd*` / `VkImage` / 同步 -> `RHI`（若是渲染算法的具体编排 -> `Render` 的 Pass）
2. 需要 `entt` / 变换 / 模型数据 -> `Scene`
3. 只处理"什么时候做、按什么顺序做" -> `Core`
4. 以上都不是（纯工具）-> `src/Core/` 下的独立头文件（如 `Profiler.h`）
