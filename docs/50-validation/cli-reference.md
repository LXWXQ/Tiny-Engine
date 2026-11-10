---
id: validation/cli-reference
title: 命令行参数全表
type: validation
scope: main.cpp
read-when: 加/改命令行参数、写自动化脚本、或不确定某个 flag 的行为
size: ~180 行
updated: 2026-11-10
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
| `--editor` | 以场景编辑器模式启动；`TinyEditor.exe` 默认开启，`TinyEngine.exe` 需显式传 |
| `--runtime-mode` | 编辑器启动后直接进入运行模式（第三人称角色 + 动画）；仅编辑器构建有效，普通渲染开关不变 |

## 2. Pass 开关与场景

| 参数 | 作用 |
|---|---|
| `--no-bloom` | 关闭 Bloom（自检与普通运行均可） |
| `--no-ssao` | 关闭 SSAO |
| `--no-shadows` | 关闭方向光级联阴影（排查 / 对比用；默认开，强度见 `lighting.shadows.strength`） |
| `--no-water` | 关闭水面；Water / PlanarReflection / SSR Pass 都不注册（只在场景有地形/体素块时才可能注册） |
| `--no-ssr` | 关闭水面近场屏幕空间反射；SSR Pass 不注册，水面回退平面反射/环境图，日志 `ssr=0` |
| `--voxel-world [r]` | 启动时生成方块体素世界（32³ chunk，半径 r = 1..12，默认 1（异步构建，逐帧上传）），相机自动摆到上方 |
| `--voxel-glass-demo` | 配合 `--voxel-world`：在体素世界里摆交叉玻璃墙 + 近景相机（V-01 透明排序目测/回归）；单独使用会被忽略并打印提示 |
| `--particle-demo` | 空场景创建一个粒子喷泉（rate=220、max=1500，贴图 `Particles/Kenney/fire_01.png`），相机自动取景；配合 `--empty-scene --selftest` / `--screenshot` 做粒子截图与回归 |
| `--gpu-scene` | 忽略实例阈值，强制走 GPU 驱动提交（配合 `gpuSceneMinInstances` 默认 256） |
| `--no-gpu-scene` | 关闭 GPU 驱动提交，完全回退 CPU 逐子网格路径 |
| `--no-merged-buffers` | 整条 GPU 批退回 CPU 逐子网格路径（跨模型 single draw 依赖合并缓冲） |
| `--no-hiz` | 关闭 Hi-Z 遮挡剔除，只留 6 平面视锥剔除（对照 `hizEnabled=0`） |
| `--no-terrain-batch` | 关闭地形多 chunk 合批，回退到逐 chunk 绑定 + 逐 chunk `drawIndexedIndirectCount` |
| `--gpu-scene-min-px <px>` | 屏幕空间误差剔除阈值（像素）；投影包围球半径小于它就直接剔除，0 = 关闭 |
| `--gpu-scene-external <n>` | 调试注入：复制第一个可批子网格 n 份当外部实例（粒子/体素替身），验证共用分组与单条提交 |
| `--streaming-world [radius]` | 创建流送世界（chunk 32 / 33x33 / 3 级 LOD），半径 1~24（F2-h 起 int64 chunk 坐标），每帧最多 4 个 chunk |
| `--streaming-fly [speed]` | 相机沿 +X 直线巡航（默认 12 单位/秒，`--streaming-fly-radius` 控制摆幅），用来验证 chunk 的加载与卸载 |
| `--streaming-fly-radius <r>` | 巡航摆幅半径，默认 220；`--origin-threshold` 小于它时更容易触发 rebase |
| `--origin-threshold <units>` | 浮动原点阈值（默认 1024）：相机局部水平距离超过它就 rebase 一次 |
| `--no-origin-rebase` | 关闭浮动原点；相机走远后局部坐标会一直变大（用来做对照） |
| `--streaming-budget <ms>` | 每帧流送生成预算（默认 2 ms）；超预算时本帧不再发起新 chunk，避免慢帧雪崩 |
| `--update-profile` | 让 `Scene::update()` 分段计时，每 60 帧打印 `[Update] avg us | genPoll= streaming= world= spatial= visibility= | maxTotal=`；关闭时零开销（只在自检/调优时开） |
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
.\\build\\Debug\\TinyEngine.exe

# 基础自检
.\\build\\Debug\\TinyEngine.exe --selftest 120

# 关 Bloom + 动态分辨率 0.5
.\\build\\Debug\\TinyEngine.exe --selftest 120 --no-bloom --render-scale 0.5

# 同步校验（必须 0 errors / 0 warnings）
.\\build\\Debug\\TinyEngine.exe --selftest 30 --validation-features sync

# GPU 计时 + 截图 + 验证层落盘
.\\build\\Debug\\TinyEngine.exe --selftest 90 --gpu-timing --screenshot shot.png --validation-jsonl val.jsonl

# 地形 G-Buffer 端到端 + 截图
.\\build\\Debug\\TinyEngine.exe --selftest 30 --empty-scene --terrain-test `
  --screenshot terrain.png --screenshot-frame 20

# 独立 compute 回归
.\\build\\Debug\\TinyEngine.exe --selftest 20 --compute-test
```

## 5. 加新参数的约定

1. 参数名用正向命名（`--gpu-timing` 而不是 `--no-gpu-timing`），**默认关闭**；
2. 解析放在 `main.cpp` 的参数循环里，状态放 `diag::Options` 或 `FirstApp` 的 setter；
3. 若属调试能力，必须遵守 [zero-overhead](../60-debug-profiling/zero-overhead.md)；
4. 更新本文件 §1~§3；
5. 若它开启一条新分支，在 [test-matrix](test-matrix.md) 里加一项回归。
