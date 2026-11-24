---
id: debug-profiling/diagnostics
title: 诊断基础设施实现
type: debug
scope: src/RHI/RenderGraph.cpp（准备 / 录制拆分与耗时）、src/Core/Diagnostics.h, src/Core/Diagnostics.cpp, src/RHI/GpuTimer.*, src/RHI/ScreenshotCapture.*, src/RHI/RenderDocHook.*, src/RHI/Breadcrumbs.*, src/RHI/PipelineStats.*, src/RHI/RenderGraphAlias.*
read-when: 修改验证层回调、GPU 计时、截图、抓帧、排查 device lost，或看管线统计与别名报告
size: ~329 行
updated: 2026-11-24
---

# 诊断基础设施实现

## 1. 验证层结构化（P0-1）

### 1.1 为什么改

原本 `Device` 的 `debugCallback` 直接写 `std::cerr`，与"stderr 0 字节"的验收基线
天然冲突——只要验证层报一条 info，基线就假失败。

### 1.2 现在

```
Device::debugCallback
   -> diag::recordValidationMessage(severity, messageId, message)
        -> 原子计数 error/warning
        -> 可选写 JSONL（惰性打开，每条 flush）
退出时 diag::printValidationSummary() -> "[Validation] errors=N warnings=M"
```

- **绝不写 stderr**。
- JSONL 格式：`{"severity":"ERROR","id":"VUID-...","msg":"..."}`。
- `--validation-jsonl <path>` 空则完全不落盘（不创建文件）。

### 1.3 验证层特性链

`--validation-features` 的值通过 `VkValidationFeaturesEXT` 挂到
`VkInstanceCreateInfo::pNext`。**必须真的传到 `Device`**：

> 历史 bug：曾因 `std::make_unique<Device>(window)` 没传 options 而**静默失效**——
> 加了 flag 却没有任何效果。现在用 `--build-info` 和
> `[Diag] validation features enabled:` 双重确认。

## 2. GPU 计时（P0-4）

- 每个 Pass 前后 `vkCmdWriteTimestamp2`（需要 `synchronization2` + `VK_EXT_calibrated_timestamps`
  给的 `timestampPeriod`）。
- 按 frame slot 各一份 query pool（避免跨帧覆盖）。
- 退出写 `gpu_timing.csv`：`frame,pass,gpu_ms`。
- **关闭时**：不创建 query pool、不插 timestamp。
- 守卫计数：`[Diag] zero-overhead guard ... queryPool=N`。

剔除预热帧很重要：冷启动 GPU 升频，头几十帧明显偏慢。

## 3. 无头截图（P1-4）

- 目标帧把交换链图像读回（`vkCmdCopyImageToBuffer`）并写 PNG（用已有 `stb_image_write`）。
- 该模式下交换链额外带 `TRANSFER_SRC` usage。
- 默认目标帧：selftest 时取"总帧数的约一半且不超过 60"；交互模式取第 60 帧。
- **关闭时**：不建 readback buffer（守卫计数 `screenshotBuffer=0`）。

## 4. RenderDoc in-app 抓帧（P1-2）

- 编译期 `TINY_ENABLE_RENDERDOC`，默认 OFF。
- 开启后在指定帧调 `TriggerCapture()`。
- **未注入 RenderDoc 时是空操作**，输出 `[Diag] RenderDoc not injected`。
- 关闭时连 `RENDERDOC_GetAPI` 都不调用。

## 5. Tracy（P1-1）

- `src/Core/Profiler.h` 提供 `TINY_ZONE_SCOPED(name)` / `TINY_PROFILE_FRAME_MARK()`。
- 关闭时展开为 `((void)0)`。
- `src/RHI/GpuProfiler.*` 提供每 pass 的 GPU zone，已集成进 RenderGraph。
- 默认 OFF；ON 路径已用 `Validate.ps1 -EnableProfiling` 验证过。

## 6. 能力自报（P0-6）

`diag::printBuildInfo(options)` 输出三类信息：

```
[BuildInfo] compiled: gpuTiming=1 validationFeatures=1 buildInfo=1 screenshot=1 tracy=0 renderdoc=0
[BuildInfo] runtime : gpuTiming=0 validationBestPractices=0 validationSync=0 ...
[BuildInfo] policy: debug capabilities are off by default; when off they allocate nothing and record no GPU commands
```

**为什么需要**：杜绝"以为开了其实没编译进去"。

## 7. 诊断自检（不需要 Vulkan）

```powershell
.\\build\\Debug\\TinyEngine.exe --diagnostics-test out.jsonl
# [Diag] diagnostics selfTest PASS | errors=1 warnings=1 jsonl=written
```

写入合成消息，校验计数与 JSONL 落盘。这样在没有真实验证层消息时，
"结构化输出 + 落盘"这条链路也有回归覆盖。

## 8. device fault（P2-4）

- 设备支持 `VK_EXT_device_fault` 时启用，`Renderer` 在 `VK_ERROR_DEVICE_LOST`
  时调 `vkGetDeviceFaultInfoEXT` 输出 `[DeviceFault]`。
- **限制**：该查询**只能在设备已 lost 时调用**
  （`VUID-vkGetDeviceFaultInfoEXT-device-07336`）。所以健康设备上只能校验
  "扩展已启用 + 入口点已解析"，真实故障路径靠 Renderer 的 device-lost 分支覆盖。

## 9. GPU breadcrumbs（P2-5）

`VK_EXT_device_fault` 只回答"哪条指令踩了哪块内存"，回答不了"崩在第几帧的哪个
Pass"。`Breadcrumbs`（`src/RHI/Breadcrumbs.*, src/RHI/PipelineStats.*`）补的就是这一层：每个 Pass 前后往
一个 4 字节游标写序号，CPU 侧留一张"序号 -> 帧号/Pass 名"的表，device lost 之后
读游标查表。核心洞察是**真相在 CPU 侧，GPU 只带一个游标**。

三种落地方式，按可用性自动降级：

1. `VK_NV_device_diagnostic_checkpoints`：`vkCmdSetCheckpointNV` 写
   `(slot << 32) | counter`，崩溃后 `vkGetQueueCheckpointData2NV` 逐队列取回。
   硬件缓冲，无命令开销，渲染通道内外都能用。取回顺序不保证，但 counter 单调，
   所以"每槽取最大"就是最后执行到的那条。
2. `VK_AMD_buffer_marker`：`vkCmdWriteBufferMarkerAMD(TOP_OF_PIPE, ...)` 写游标缓冲。
   用 `TOP_OF_PIPE` 是刻意的 —— 语义是"命令流到了这里"，不是"这个 Pass 做完了"。
3. 都没有时退化成 `vkCmdUpdateBuffer`：它在渲染通道内非法，所以只能按 Pass 粒度。

- 槽位 = 帧槽 × {graphics, compute}，共 4 个（`Breadcrumbs::kSlotCount`），
  两帧在飞各有独立游标，不会互相覆盖。
- 游标缓冲是 **HOST_VISIBLE 常驻映射**的。device lost 之后不能再提交任何命令，
  只能直接读 CPU 映射的那块内存 —— 这是设计前提，不是优化。
- 触发点：`Renderer::endFrame` 三处 `VK_ERROR_DEVICE_LOST` 分支调
  `Device::reportDeviceFault(queue)`，函数**第一件事**就是跑 breadcrumbs，
  所以没有 `VK_EXT_device_fault` 的机器上也能拿到"崩在哪个 Pass"。
- 输出：`[Breadcrumbs] 崩溃点 slot=N frame=M "PassName"` + 之前 8 条 trail，
  并走 `tiny::writeStartupErrorLog` 落盘（`main.cpp` 的错误对话框会指向该文件）。
- 纯逻辑自检：`Breadcrumbs::selfTest()`（槽位映射、标记编解码往返、名字截断），
  挂在 `runHeadlessSelfTest` 里，不需要 GPU 也能跑。
- **坑**：`Breadcrumbs::shutdown()` 必须在析构里调。`Device` 只是
  `breadcrumbs.reset()`，隐式析构不会调用它，游标缓冲会活到
  `vmaDestroyAllocator` 之后，触发 VMA 的 `Some allocations were not freed` 断言
  （见 [troubleshooting §3.2](../40-build/troubleshooting.md)）。

## 10. 管线统计（TM-06）

`--stats-pipelines` 在退出时打印本次运行的管线账本：

- 图形 / compute 各建了几条，shader 模块几个、读了多少 MB SPIR-V；
- 建管线总耗时与均值（计时覆盖"读 SPIR-V + 建模块 + 建管线"，真正慢的是驱动编译）；
- **状态种类**与**完全相同的管线**是两个数：前者是 `GraphicsStateKey` 的去重结果，
  后者还要两个 shader 路径也一致。状态相同但 shader 不同完全正常（IBL 的几条只有
  片元不同），混成一个数会得出"重复很多"的错误结论。
- `GraphicsStateKey.pipelineLayout` 必须参与哈希：不同布局的管线不能互换，漏算就会
  把真正不同的管线判成重复。

本机实测（RTX 2070，`--selftest 30`）：图形 30 条 / compute 8 条 / 68 个模块 /
0.41 MB SPIR-V；冷启动无缓存 total=50.5ms，热缓存 total=24.7ms（平均 0.65ms/条）；
30 条图形管线里状态只有 26 种，完全相同的管线 0 条。

结论：桌面有驱动磁盘缓存时，启动期建管线不过几十毫秒，**没有值得做的按需建**；
这项的价值在 Android / 冷启动 / 管线爆炸（成百上千条）的场景。零开销路径同样验证过：
不带 `--stats-pipelines` 时只累加整数，不建表，输出与基线逐字节一致。

## 11. 资源别名报告（TM-08）

`--alias-report` 在退出时打印本帧中间目标的显存账本。它只读：不改任何分配、不绑定
`VK_IMAGE_CREATE_ALIAS_BIT`，目的就是先让"能不能省"这个问题的答案变成数字。

- **生命周期**：每个纹理记两个 Pass 序号 —— `firstWrite`（第一次被打上写访问的那个 Pass）
  与 `lastUse`（最后一次读或写）。序号取 `lastFrameExecutedPasses.size() - 1`，所以被
  `setEnabled(false)` 跳过的 Pass 不占号，与调试面板看到的顺序一致。
- **装箱**：按 `firstWrite` 升序线性扫描，`best fit` 放进"已经空出来"的块里；合并前提是
  格式 / 尺寸 / mip / 层数全同（比 Vulkan 允许的更保守，松一档就要顺带验证 image view 兼容性）。
- **三类目标分开报**：渲染图自己持有的（可谈别名）、每帧重建但导入的（显存不归渲染图管）、
  跨帧常驻的（本就不该参与）。混在一起报会给出误导性的节省数字。

本机实测（RTX 2070，1920x1080，`--selftest 30 --alias-report`）：

```
[Alias] 一帧 8 个瞬时目标，合计 40.8 MiB
[Alias] 线性扫描后 8 块（最大 15.8 MiB），可省 0（0%）
[Alias]   SSAO.Raw.Frame1 [Pass 6..7] 2.0 MiB
[Alias]   SSAO.Frame1 [Pass 7..8] 2.0 MiB
[Alias]   HDR.Scene.Frame1 [Pass 8..11] 15.8 MiB
[Alias]   TAA.Output.Frame1 [Pass 11..19] 15.8 MiB
[Alias]   Bloom.Frame1.Level0 [Pass 12..19] 4.0 MiB
[Alias] 另有 13 个每帧重建的导入目标（合计 181.9 MiB）：它们才是有价值的目标，但显存不归渲染图管
[Alias] 常驻导入目标 4 个（合计 229.4 MiB，跨帧复用同一块，不该参与别名）
```

为什么是 0：TAA 输出覆盖了整条 bloom 链（`[11..19]` 对 `[12..19]`），HDR 与 TAA 在 Pass 11
上首尾相接（同一 Pass 读 HDR、写 TAA），SSAO 的两张图在 Pass 7 上重叠。三个方向都被堵死，
所以本结构下别名化省不出显存 —— 详细取舍见 [decisions](../10-architecture/decisions.md) 决策 27。

零开销路径：不带 `--alias-report` 时 `RenderGraph::endFrame` 里那一段整个不执行（连 vector
都不建），`--selftest 30` 的输出与基线一致，`[Diag] zero-overhead guard PASS` 仍然通过。
## 12. 渲染图准备 / 录制耗时（TM-10）

`RenderGraph::endFrame` 把每个 Pass 的工作拆成两段：

- **准备**：访问去重（同一个 Pass 里对同一张图声明多次时只保留最后一次）、声明合法性校验（句柄越界、usage 不匹配）、组装 `VkRenderingInfo` 与附件描述。纯数据，不碰 `VkCommandBuffer`。
- **录制**：屏障推导（依赖上一个 Pass 留下的布局）+ `vkCmdBeginRendering` + 用户回调 + `vkCmdEndRendering`。必须串行。

两段各自的耗时常年累加，`--stats-rendergraph` 在退出时打印每帧平均值：

```
build\Debug\TinyEngine.exe --selftest 60 --stats-rendergraph
[RenderGraph] 准备 35.92 us/帧 | 录制 1464.78 us/帧 | 并行=关 | 本帧 21 个 Pass
```

### 12.1 为什么不把准备阶段并行

`--parallel-prepare` 会让准备阶段走 `JobSystem::parallelFor`（一个 Pass 一个任务）。实测（RTX 2070，1920x1080，各 3 次）准备从 35.5 / 35.9 / 36.9 us 变成 176.9 / 203.2 / 207.7 us —— 慢 4.9~5.8 倍，录制时间不变。原因是 21 个 Pass 的准备合计只有 36 us，而把任务丢进工作窃取池再等它们做完是 160 us 量级：线程唤醒的延迟远大于被并行的工作量。

这条测量结果就是结论：准备阶段保持串行，开关留着只为可复现对照（默认关闭）。

### 12.2 错误为什么在串行阶段统一抛

并行时如果从 worker 里抛异常，报错顺序会随线程调度变化 —— 排查时看到的第一个错不一定是第一个错。所以 `PreparedPass` 带一个 `error` 字段，准备阶段只记录，回到串行阶段按 Pass 序号依次检查，报错文案与串行路径逐字一致（`RenderGraph: Pass "X" 引用了越界的纹理句柄` 这类）。

### 12.3 与其它统计开关的关系

`--stats-pipelines`（第 10 节）看的是管线构建，`--stats-scratch`（第 7 节）看的是暂存区分配，`--alias-report`（第 11 节）看的是资源生命周期；本节看的是每帧渲染图的 CPU 开销。四个开关互不影响，可以同时打开。

## 13. 帧级度量四件套（T-10 的后续）

`--stats-frame` 现在退出时打四段，彼此独立：

```
[FrameStats] frames=600 | passes/frame=21.0 skipped=0.0
[FrameStats] barriers/frame=38.0 calls 45.2 entries | skipped: sameState=18.8 crossQueue=0.0
[FrameStats] pipelineBinds/frame=23.5 redundant=10.5 (44.7%)
[FrameTime] frames=600 | median=7.88ms p95=9.12ms max=88.02ms
[Resources] frames=600 | images/frame=0.27 total=160 peakFrame=160 | buffers/frame=0.03 total=16 peakFrame=16
[Memory] heap0 used first=1220.7 last=1220.7 peak=1220.7 MiB | allocs first=185 last=185 peak=185
[SceneGpu] cull summary | frames=598 visibleRatio=0.61 | groups/frame=103.0 visible=63.0 culled=40.0 | instances/frame=63.0 submitted/frame=103.0 | hizCulled/frame=0.0 screenCulled/frame=0.0
```

- **`[FrameTime]`**：`FrameStats::recordFrameTime(double ms)` 把每帧耗时落进 0.25 ms 一格的直方图（0~250 ms，另有一个溢出桶），`percentile()` 按桶累积。中位 / p95 是稳态抖动，`max` 基本等于启动帧（建管线 + 上传），所以它衡量首帧 hitch。注意交互循环和 `--selftest` 循环是两条路径，两处都要记一次，否则 selftest 跑完一行都不打。
- **`[Resources]`**：`ResourceStats` 在 `RenderGraph::createOwnedTexture/createOwnedBuffer` 与 `ScratchArena::createSlotBuffer` 之后计数。**帧首不清零**：G-Buffer、渲染图自持目标、暂存区都在第一帧 `beginFrame` 之前建好，清掉就再也算不进来。清零放在 `endFrame` 累加之后，于是"第 1 帧"自然带上启动期的创建量，`peakFrame` 就是启动那一下的创建量——T-04 要压的正是它。
- **`[Memory]`**：`vmaGetMemoryProperties` + `vmaGetHeapBudgets`，10 Hz 采样，`report()` 之前再补一次。不补的话 60 帧的短跑只在第 1 帧采到一次，那还在加载期，`first` 与 `last` 差太远会被误读成"在增长"。
- **`[SceneGpu] cull summary`**：`SceneGpuBatch::readStats` 累计、`logCullSummary()` 归一化成每帧值。只在 `visible + culled > 0` 的帧算样本（回读滞后两帧，前两帧是 0）。`visibleRatio` 是 P2 `visible/submitted ≤ 0.6` 的直接读数。

### 13.1 相机必须能钉住

`--camera <px,py,pz,pitch,yaw>` 覆盖 `config/engine.json` 里的机位，`bench.ps1` 三个场景都带 `Scene::init` 的 Sponza 默认机位。
不加这个开关时，基线相机会跟着"上次谁动过"变：本机 config 里存的是 `(-66.06, 15.92, -76.19)`，站在场景外 100 米看向天空，
`default` 场景于是只有 1 draw call / 8192 三角形、`visibleRatio = 0.00`。同一份代码两次跑出的截图不同，剔除读数也没有意义。

### 13.2 与其它开关的关系

`--stats-frame` 关掉时这些计数器仍然累加（整数加法，无分配），只是不打印；它们和 `--gpu-timing`（第 2 节）、`--stats-pipelines`（第 10 节）、`--stats-rendergraph`（第 12 节）可以同时开。

### 13.3 `[RTPool]`：池的命中账本（T-04）

只在 `--stats-frame` 且开了 `--rt-pool` 时才有意义，但**池关时也会打**：`enabled=0`
那行是回退证据 —— `created` 等于这一轮所有 `acquire`，一条命中都没有。

```
[RTPool] enabled=0 | hits=0 misses=0 created=230 destroyed=192 evicted=0 | hitRate=0.0% | idle=0 images 0.0 MiB
[RTPool] enabled=1 | hits=57 misses=173 created=173 destroyed=80 evicted=80 | hitRate=24.8% | idle=55 images 126.4 MiB
[RTPool] top creators:
  SSAO.Raw.Frame0              6
  SSAO.Raw.Frame1              6
  TAA.History0                 6
```

读法：`created` 是真正 `vmaCreateImage` 的次数；`hits` 是省下来的；`evicted > 0` 说明
预算不够，池在超预算淘汰 —— 被淘汰的往往正是后面要复用的那张。`top creators` 按名字
统计，是「谁在 resize 里重建得最多」的直接答案：`Frame0/Frame1` 成对出现，说明一半
创建量来自 `kFramesInFlight=2` 的复制，见决策 46。
### 13.4 `[StateCache]`：绑定缓存的账本（T-09）

```
[StateCache] frames=60 | bindCalls/frame=23.5 issued=13.0 unmanaged=0.0 skipped=10.5 | saved=44.7%
```

- `bindCalls` 是调用方请求绑定几次（图形 + 计算），`issued` 是真正发出去的 `vkCmdBindPipeline`，`skipped` 是因为"这条命令缓冲已经绑着同一个管线"而省掉的。
- `unmanaged` 是落在**没登记过**的命令缓冲上的绑定：这类缓冲可能刚被 reset，缓存不敢跳过，所以每次都发。正常配置下它应当是 0 —— 若不是 0，说明有新的一次性命令缓冲没有调 `statecache::trackCommandBuffer`。
- 不变量：`issued + skipped == bindCalls`。`--no-pipeline-cache` 是整体回退开关，关掉后 `unmanaged == bindCalls`、`saved=0%`。
- **`bindCalls` 必须等于 `[FrameStats] pipelineBinds/frame`**，`skipped` 必须等于 `redundant`。两表的窗口都在 `RenderGraph::beginFrame` 开，一旦不等就说明记账或窗口错了 —— 这是这条缓存唯一的自检手段。
- `saved` 就是 P3 里「状态切换次数下降 ≥ 40%」的分母：当前 default 44.7%、lights-512 49.4%。省掉的是驱动调用，不是 GPU 时间 —— 帧时间不因此变化。
- 缓存按 `VkCommandBuffer` 分别记账（图形 / 计算两条流不共享），`Pipeline` / `ComputePipeline` 析构时整体失效（句柄值可能被复用）。`--no-single-frame-targets` 之类开关不影响它。

### 13.5 [Cull]：Pass 剔除剔掉了谁（T-05）

默认关，`--pass-cull` 打开。退出时一行：

```
[Cull] enabled=1 dryRun=0 | frames=0 culled/frame=0.0 | 最近一帧: (none)
```

`frames` 只统计「这一帧至少剔掉一个 Pass」的帧数，`culled/frame` 是在这些帧上的平均；`--pass-cull-dry-run` 时数字照样累计，但 Pass 不会真的被跳过，`[FrameStats] passes/frame` 不变。

读法：

- `culled/frame=0.0` 且 `最近一帧: (none)` 是**本引擎的正常结果** —— 特性关掉时 Pass 直接不进图，没有「进了图却没人读输出」的对象。参见决策 49。
- 一旦某个配置下出现名字，先看它写的那张图被谁读：`read(...)` 声明缺失是最常见的原因（含 `writeColor(..., LOAD_OP_LOAD)` 这种读改写），而不是「真的该剔」。
### 13.6 [Queue]：两条队列到底有没有重叠（T-07 第一步）

默认关，`--stats-queue` 打开。每条命令缓冲头尾各写一枚 timestamp，下一轮复用到这个 slot 时读回，退出时一行：

```
[Queue] frames=58 | makespan=8.65 ms | graphics busy=8.36 ms (96.7%) | compute busy=0.25 ms (2.8%) | overlap=0.00 ms (0.0% of compute, max 0.00 ms)
[Queue] dedicatedCompute=1 sameClockDomain=1 | compute 被图形遮住 0.0%
```

四个量的定义：

- `makespan`：两条队列从最早开始到最晚结束的跨度（同一帧的两条队列窗口的并集）。
- `graphics busy` / `compute busy`：各自窗口长度，除以 `makespan` 得到占比。
- `overlap`：两个窗口的交集长度。`overlap / compute busy` 就是「计算被动藏在图形后面」的比例 —— 这是 T-07 唯一要抬的指标。
- `max overlap`：单帧最大重叠，用来发现「偶尔重叠」被平均掩盖掉的情况。

两个前提必须一起看：

- `sameClockDomain=0` 时两条队列的时间戳不在同一时间域，`overlap` 一律按 0 处理，只保留各自 busy 时长。
- 同一帧内只要存在 c2g 依赖（compute 写、graphics 读），图形流整条都要等 compute 完成 —— 因为现在每帧的图形命令只有**一次**提交。实测 512 灯场景 `compute busy=0.25 ms` 而 `overlap=0.00 ms`，正是因为这一点。要抬 `overlap`，必须把图形拆成多个提交点，这正是 T-07 的主体。

关掉时零开销：`Application` 在没有 `--stats-queue` 时直接 `queuestats::shutdown()`，命令缓冲里一枚 timestamp 都不写（默认场景 `--stats-frame` 的三行数字与打开时逐位相同）。

### 13.6.1 拆分提交前后（T-07）

512 灯场景、60 帧、`--validation-features sync`，同一构建加 `--no-async-split` 对照：

```
[Queue] frames=58 | makespan=10.06 ms | graphics busy=9.73 ms (96.8%) | compute busy=0.27 ms (2.7%) | overlap=0.00 ms (0.0% of compute, max 0.00 ms)
[Queue] dedicatedCompute=1 sameClockDomain=1 | compute 被图形遮住 0.0%
[Queue] frames=58 | makespan=9.03 ms | graphics busy=9.03 ms (100.0%) | compute busy=0.51 ms (5.7%) | overlap=0.51 ms (100.0% of compute, max 3.52 ms)
[Queue] dedicatedCompute=1 sameClockDomain=1 | compute 被图形遮住 100.0%
```

第一段是单次提交：compute 的 0.27 ms 完全串行躺在关键路径上，`overlap=0.00`。第二段是拆分后：`overlap` 等于 compute 的全部忙碌时间（100%），`makespan` 少 1.03 ms。

读法：`overlap / compute busy` 是队列窗口层面的上限，不要直接换算成帧率收益 —— 600 帧 × 3 轮交替实测只 +1.05%，因为 compute 本来就只占帧时间的 2.5%。
