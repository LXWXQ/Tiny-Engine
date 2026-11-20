---
id: validation/cli-reference
title: 命令行参数全表
type: validation
scope: main.cpp、src/Core/CliCatalog.cpp
read-when: 加/改命令行参数、写自动化脚本、或不确定某个 flag 的行为
size: ~180 行
updated: 2026-11-20
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
| `--handle-test` | 实体句柄契约自检（失效、索引复用升版本），不建窗口 |
| `--trace-test` | 射线契约（TraceRay/TraceResult/TraceFilter）自检，不建窗口 |
| `--gltf-node-test` | glTF 节点树（父子链合成、TRS 顺序）自检，不建窗口 |
| `--pack-test` | `.tpack` 资产包契约自检（格式、16B 对齐、后缀查找、挂载），不建窗口 |
| `--pack-build <dir> <out>` | 把目录递归打成 `.tpack` 资产包后退出 |
| `--material-json-test` | 材质 JSON 只存非默认字段的契约自检，不建窗口 |
| `--terrain-sculpt-test` | 地形雕刻差分与存档往返自检，不建窗口 |
| `--texture-budget-test` | 纹理预算降级策略自检，不建窗口 |
| `--tick-test` | 固定步长模拟时钟契约（60Hz、护栏、跳跃锁存、重放一致性）自检，不建窗口 |
| `--render-interp-test` | 渲染插值（TRS 分解 + slerp + 退化回退）契约自检，不建窗口 |
| `--particle-bench` | 粒子上限（16384）下的模拟耗时测量，打印 `[Particle] bench PASS`，不建窗口 |
| `--audio-test` | 音频混音器契约自检（声部池、抢占无爆音、限幅、WAV 往返），不建窗口、不需要声卡 |
| `--audio-play` | 打开声卡播 2s/440Hz 测试音并校验 `played/active/peak`；**需要音频设备**，不放进 Validate |
| `--audio-spatial-test` | 3D 音源空间参数（距离衰减、边界淡出、声像、远处低通）契约自检，不建窗口、不需要声卡 |
| `--replay-test` | 角色步进纯函数（60/144Hz 逐 tick 哈希一致）与输入录制/回放一致性自检，不建窗口 |
| `--viewmodel-test` | 手持物投影/偏移矩阵契约自检（6 组，含 +z 朝前约定与 TRS 顺序），不建窗口 |
| `--sky3d-test` | 3D 天空盒的相机/缩微矩阵自检（不需要 Vulkan / 窗口） |
| `--character-collision-test` | 角色扫掠盒 / 滑动 / 上台阶契约自检（不需要 Vulkan / 窗口） |
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
| `--cloud-demo` | 空场景创建或启用 `scene.clouds`，设 coverage=.55、base=200、thickness=120、steps=56，相机放到 (0,35,80)、pitch=-7°；配合 `--empty-scene --selftest` / `--screenshot` 做体积云截图与回归 |
| `--fog-demo` | 空场景创建一个局部雾盒（`effects.fogVolume`，density=0.045、steps=24、组件 center=(-6,10,-10)、size=30x10x36、rotation=0），相机放到 (0,18,28)、pitch=-8°；建议配合 `--streaming-world 3`，否则没有地形可衬托。演示实体的 Transform 保持单位变换，盒体完全由组件 placement 定义 |
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
| `--texture-budget <MB>` | 纹理显存预算（0 = 不限制，默认不限制）。超限时该张纹理先丢 mip 链（-25%），仍放不下就逐级减半分辨率（每级 -75%，最小 64×64），日志 `[Texture] 预算降级 A -> B`；`--selftest` 结束会打印 `[Texture] budget used=... peak=... limit=... textures=N` |
| `--render-scale F` | 动态分辨率倍率 0.25~1.0 |
| `--resize-test` | 自检中连续缩放、切动态分辨率、最小化/恢复 |
| `--material-stress` | 反复分配/延迟归还材质槽位 |
| `--compute-test` | 启用 compute probe（默认关闭） |
| `--cross-queue-test` | 启用跨队列双方向探针 |
| `--subresource-test` | 启用子资源正向探针 |
| `--sun-demo` | 启用太阳盘与镜头光晕（建议配 `--streaming-world`） |
| `--vegetation-demo` | 在地形上种草与树，相机贴近地面（建议配 `--streaming-world 3`） |

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
| `--stats-pipelines` | 退出时打印管线数量、建管线耗时与状态去重 | 0（关时只累加整数、不建表） |
| `--stats-scratch` | 退出时打印每帧暂存区的分配次数 / 字节 / 扩容 / 刷新 | 0 |
| `--stats-rendergraph` | 退出时打印渲染图准备与录制的每帧耗时 | 0 |
| `--parallel-prepare` | Pass 准备阶段丢进 JobSystem 并行（实测更慢，见 TM-10） | 0（默认串行） |
| `--alias-report` | 退出时打印资源别名候选与预估可省显存（只读报告） | 0（关时 endFrame 整段不执行） |
| `--light-stress <n>` | 铺 n 盏点光源做压力场（配合 `--clustered-lighting` 看聚簇收益） | 0 |
| `--light-stress-radius <m>` | 压力场铺开半径（米，默认 60，夹到 1~5000）。两个作用：① 决定灯铺多开；② 决定铺多远 —— 灯的中心放在相机前方 `max(22, 半径)` 米处，半径越大铺得越远。单灯半径固定 12 米、强度 2；铺开半径与单灯半径故意解耦：spread 管"密度"、单灯半径管"每盏灯影响多大"，绑在一起会让聚簇对比失真（半径跟着 spread 走时 512 灯聚簇掉到 39 FPS）。灯会按体素地形高度贴地（抬 1.2 米），否则铺在半空的灯离可见片元二三十米，看起来就是"加了灯画面不变" | 0 |
| `--clustered-lighting` | 点光源按 16×9×24 簇取灯（默认关闭，见决策 32） | 0 |
| `--clustered-gpu-build` | 簇表改由 `cluster_build.comp` 每帧一次 dispatch 生成（需同时给 `--clustered-lighting`）；定长槽位占 3.4 MiB/帧槽，见决策 40 | 0 |
| `--fireflies <n>` | 区域内点亮 n 个萤火虫式光源（形状 + 颜色 + 照亮地面），夹到 0~256。**默认 0 = 关闭**，不影响默认画面；形状由 `ParticlePass` 的 billboard 画（程序化圆形 + 加法感的软边），光照由 `LightSources` 喂给光照 Pass | 0 |
| `--fireflies-radius <m>` | 萤火虫群铺开的半径（米，默认 12，夹到 1~500）。群的中心沿视线方向摆在相机前方 25 米处，屏幕上就落在画面中段；单盏灯的照亮半径是 6~9 米，与发光点的直径（0.4~0.85 米）是两件事 | 12 |
| `--auto-exposure` | 自动曝光：每帧统计 HDR 平均对数亮度并缓慢适应（目标中灰 0.18，速度 1.5/s，夹到 0.05~20）。**默认关**；曝光值不写入 `config/engine.json`，只走 `FrameInfo::exposureOverride`，见决策 42 | 0 |
| `--stats-clusters` | 退出时打印簇构建次数 / 使用簇数 / 最大簇 / 丢弃灯数 / 半径 | 0 |
| `--list-options` | 打印选项表后退出（与编辑器「帮助 → 命令行选项」同源） | 0 |
| `--print-formats` | 打印本机对引擎用到的十个格式的能力（字节/颜色/深度/采样/存储/blit/线性）后退出 | 0 |
| `--bloom` | 强制打开 Bloom（默认跟随场景组件；量测与演示用） | 0 |
| `--print-assets` | 打印可执行目录与 shader / 资源 / 配置三个根的解析结果（继续跑，配 `--selftest N` 看） | 0 |
| `--print-events` | 打印 latched 事件的当前值与订阅者数量（交换链重建 / 渲染目标尺寸） | 0 |
| `--hot-reload` | 每帧检查 `.spv` 的修改时间，变了就重建对应管线（改 binding 数量需重启） | 0 |
| `--probe-transient` | 打印 6 个候选中间目标的惰性分配（transient attachment）能力后退出；只读，不改分配策略 | 0 |

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
2. 解析放在 `main.cpp` 的参数循环里，状态放 `diag::Options` 或 `FirstApp` 的 setter；**同时把这一行加进 `src/Core/CliCatalog.cpp` 的表** —— `--help` / `--list-options` / 编辑器的「帮助 → 命令行选项」面板都渲染同一份表；
3. 若属调试能力，必须遵守 [zero-overhead](../60-debug-profiling/zero-overhead.md)；
4. 更新本文件对应小节（§1~§3 的表格与 `CliCatalog.cpp` 是两份必须同时改的地方：表面向使用者，本文件补"为什么"与关闭时的成本）；
5. 若它开启一条新分支，在 [test-matrix](test-matrix.md) 里加一项回归。

## 6. 选项表的单一来源

三处显示的是同一份数据（`src/Core/CliCatalog.cpp` 的 `kCatalog`）：

| 入口 | 用途 |
|---|---|
| `TinyEngine.exe --help`（或 `-h`） | 命令行查看，带分组与说明 |
| `TinyEngine.exe --list-options` | 同上，语义更明确（脚本里可读性更好） |
| 编辑器「帮助 → 命令行选项」 | 搜索框 + 分组下拉筛选，边跑边查 |

因此"加参数忘写帮助"这件事在结构上不存在；但**反过来会漏**：只加进表、没更新本文件时，
文档会缺"为什么这么设计"和"关闭时成本"。`Chore` 提交里补上这一段即可。

`CliCatalog` 的自检（`--selftest` 会跑）：选项数不少于 70、每行三个字段非空、开关不重复、
必含 `--light-stress-radius` 与 `--list-options`。面板的自检另外验搜索的大小写折叠与分组去重。
