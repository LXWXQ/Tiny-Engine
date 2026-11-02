---
id: validation/cli-reference
title: 命令行参数全表
type: validation
scope: main.cpp
read-when: 加/改命令行参数、写自动化脚本、或不确定某个 flag 的行为
size: ~180 行
updated: 2026-11-02
---

# 命令行参数全表

## 1. 运行模式

| 参数 | 行为 |
|---|---|
| （无） | 正常窗口运行，关窗退出 |
| `--selftest [N]` | 无交互跑 N 帧（默认 600）后退出 |
| `--jobtest` | 只测 CPU JobSystem，**不创建 Vulkan 窗口** |
| `--config-test` | 配置迁移/保护自检，不建窗口 |
| `--build-info` | 打印能力清单后退出，不建窗口 |
| `--diagnostics-test [path]` | 诊断管线自检（合成消息 -> 计数 + JSONL），不建窗口 |
| `--subresource-error-test` | 期望 RenderGraph **拒绝**写 mip0 后读 mip1 |
| `--device-fault-test` | `VK_EXT_device_fault` 能力自检 |
| `--hidden` | 不创建可见窗口；自检/截图/回归这类无人值守运行**默认开启**。窗口仍有真实客户区尺寸，交换链、渲染与截图不受影响 |
| `--minimized` | 启动后最小化窗口（先隐藏创建，渲染器就绪后再 `glfwShowWindow` + `glfwIconifyWindow`，避免启动瞬间抢焦点）；最小化会命中 `handleWindowMaintenance()` 的跳过分支、不出帧，只适合看日志。与自检/截图等无人值守运行组合时自动退化为 `--hidden` |
| `--visible` | 强制显示窗口，覆盖 `--hidden` 的默认行为（交互调试 / RenderDoc 抓帧时用） |

## 2. Pass 开关与场景

| 参数 | 作用 |
|---|---|
| `--no-bloom` | 关闭 Bloom（自检与普通运行均可） |
| `--no-ssao` | 关闭 SSAO |
| `--render-scale F` | 动态分辨率倍率 0.25~1.0 |
| `--resize-test` | 自检中连续缩放、切动态分辨率、最小化/恢复 |
| `--material-stress` | 反复分配/延迟归还材质槽位 |
| `--compute-test` | 启用 compute probe（默认关闭） |
| `--cross-queue-test` | 启用跨队列双方向探针 |
| `--subresource-test` | 启用子资源正向探针 |

## 3. 调试 / 剖析（默认全关）

| 参数 | 作用 | 关闭时成本 |
|---|---|---|
| `--gpu-timing` | 每 pass GPU 计时 | 0 分配、0 命令 |
| `--gpu-timing-csv <path>` | 指定 CSV 路径（隐含 `--gpu-timing`，默认 `gpu_timing.csv`） | 同上 |
| `--validation-features <list>` | `bp` / `sync` / `printf` / `gpuav`，逗号分隔或 `all` | 0 |
| `--validation-jsonl <path>` | 验证层消息写 JSONL（默认不落盘） | 0 |
| `--screenshot <path>` | 目标帧截图写 PNG | 0（不建 readback buffer） |
| `--screenshot-frame <n>` | 指定帧号（隐含 `--screenshot`） | 同上 |
| `--screenshot-viewport` | 截图拍编辑器离屏视口（纯 3D，不含 UI）；需配合 `--editor` | 0 |
| `--empty-scene` | 不加载 Sponza / 天空盒，只留相机（调试单物体交互） | 0 |
| `--terrain-test` | 启动时在相机前方创建一块持久地形，用于端到端渲染/截图回归；常与 `--empty-scene --selftest` 配合 | 0（不传时不生成、不分摊） |
| `--capture-frame <n>` | RenderDoc 在第 n 帧抓帧（需注入） | 0（不调 `RENDERDOC_GetAPI`） |

`--validation-features` 的取值：

| 值 | 等价别名 | 说明 |
|---|---|---|
| `bp` | `best-practices` | 性能反模式提示（噪声较多，仅报告） |
| `sync` | `synchronization`, `syncval` | 同步 hazard（**门禁项**） |
| `printf` | `debug-printf` | 着色器内 `printf` |
| `gpuav` | `gpu-assisted` | GPU 侧越界（bindless 专项，重，且**不并入 `all`**） |
| `all` | — | bp + sync + printf（**不含 gpuav**） |

## 4. 常用组合

```powershell
# 正常窗口
.\\build\\Debug\\VulkanEngine.exe

# 基础自检
.\\build\\Debug\\VulkanEngine.exe --selftest 120

# 关 Bloom + 动态分辨率 0.5
.\\build\\Debug\\VulkanEngine.exe --selftest 120 --no-bloom --render-scale 0.5

# 同步校验（必须 0 errors / 0 warnings）
.\\build\\Debug\\VulkanEngine.exe --selftest 30 --validation-features sync

# GPU 计时 + 截图 + 验证层落盘
.\\build\\Debug\\VulkanEngine.exe --selftest 90 --gpu-timing --screenshot shot.png --validation-jsonl val.jsonl

# 地形 G-Buffer 端到端 + 截图
.\\build\\Debug\\VulkanEngine.exe --selftest 30 --empty-scene --terrain-test `
  --screenshot terrain.png --screenshot-frame 20

# 独立 compute 回归
.\\build\\Debug\\VulkanEngine.exe --selftest 20 --compute-test
```

## 5. 加新参数的约定

1. 参数名用正向命名（`--gpu-timing` 而不是 `--no-gpu-timing`），**默认关闭**；
2. 解析放在 `main.cpp` 的参数循环里，状态放 `diag::Options` 或 `FirstApp` 的 setter；
3. 若属调试能力，必须遵守 [zero-overhead](../60-debug-profiling/zero-overhead.md)；
4. 更新本文件 §1~§3；
5. 若它开启一条新分支，在 [test-matrix](test-matrix.md) 里加一项回归。
