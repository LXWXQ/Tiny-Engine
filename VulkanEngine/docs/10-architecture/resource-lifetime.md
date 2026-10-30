---
id: architecture/resource-lifetime
title: 资源生命周期
type: architecture
scope: src/RHI/RenderGraph.cpp, src/Core/Application.cpp, src/Core/DescriptorManager.cpp, src/RHI/CommandUtils.h
read-when: 新增任何 GPU 资源、修改销毁逻辑、处理延迟释放或 resize
size: ~230 行
updated: 2026-10-30
---

# 资源生命周期

## 1. 三类资源

| 类别 | 例子 | 谁创建 | 谁销毁 | 生命周期 |
|---|---|---|---|---|
| **图自有瞬时** | G-Buffer、SSAO、hdrScene、Bloom 金字塔 | `RenderGraph::createTexture` | `RenderGraph` | 每帧重写内容；resize 时重建 |
| **导入常驻** | 交换链图像 | `Swapchain` | `Swapchain` | 交换链重建时重建 |
| **外部常驻** | 纹理、cubemap、IBL 结果 | `Texture` / `CubemapTexture` / `IBLBaker` | 各自持有者 | 整个运行期或到场景卸载 |

## 2. 帧槽位隔离（2 帧在飞）

`kFramesInFlight = 2`（`src/Core/FrameInFlight.h`）。凡是被 GPU 跨帧读取的东西都**每槽一份**：

| 资源 | 数量 | 位置 |
|---|---|---|
| 命令缓冲（graphics / compute） | 2 × 2 | `Renderer` |
| timeline semaphore | 2 × 2 | `Renderer` |
| 全局 UBO | 2 | `Application` |
| G-Buffer（5 附件） | 5 × 2 | `RenderGraph` |
| SSAO UBO + 输出 | 2 × 2 | `SSAOPass` / 图 |
| hdrScene | 2 | 图 |
| Bloom 金字塔（4 级） | 4 × 2 | 图 |
| 描述符集（global / ssao / bloom / present） | 各自 × 2 | 各 Pass |

**新增任何"每帧写、GPU 读"的资源，必须开两份**，否则 CPU 会在 GPU 还在读上一帧时覆写。

## 3. 销毁顺序（必须显式）

### 3.1 历史 bug（坑）

原来 `renderPipeline` 是 `FirstApp` 的值成员，声明得比 `vulkanDevice` **晚** ——
成员逆序析构时它排在 `vulkanDevice` **之后**销毁，于是 Pass 持有的 VMA 分配
在 `vmaDestroyAllocator` 之后才释放，直接触发 VMA 断言崩溃。

**它藏了很久的原因**：验证脚本用 `TerminateProcess` 杀进程，而它**根本不跑析构函数**。
所以"验证层 0 字节"这个结论**从未覆盖过退出路径**。

### 3.2 现在的做法

`~FirstApp` 把顺序全部显式写出来：

```
Pass（RenderPipeline::clearAll）
  -> 依赖窗口的（ImGuiSystem、Swapchain）
  -> 其余 GPU 资源（IBLBaker、Texture、DescriptorManager）
  -> Device（VMA 在这里销毁）
  -> 窗口 / Instance
```

**规则**：不要靠成员声明顺序表达销毁依赖。有人插一个新成员就会悄悄破坏它，编译器一声不吭。
排序时看"依赖谁"，而不是"看起来顺不顺眼"。

## 4. 延迟释放

### 4.1 为什么需要

CPU 录制完命令缓冲后资源还不能立刻销毁——命令还没提交，GPU 可能还没执行到。
录制完就销毁会让 `vkQueueSubmit` 指向失效对象，验证层刷屏，而且往往刷在几十帧之后。

### 4.2 两种已有机制

| 机制 | 用于 | 实现 |
|---|---|---|
| `cmdEndSingleTime()` 内部 `vkQueueWaitIdle` | IBL 烘焙这类一次性初始化 | `src/RHI/CommandUtils.h` |
| graphics timeline 延迟回收队列 | 材质 bindless 槽位 | `DescriptorManager::releaseMaterialSet()` |

### 4.3 材质槽位回收（TD-04）

```cpp
releaseMaterialSet(slot);   // 不立即归还，记录"归还时的最后一个已提交 graphics frame"
                            // 每帧查询 timeline：safeAfterFrame <= completedFrame 时
                            // 才真正 push 回 freeSlots
```

- 启用 `descriptorBindingSampledImageUpdateAfterBind` + `UPDATE_AFTER_BIND_POOL`，
  允许向已绑定的数组继续写新槽位。
- 空闲槽不足时只等待"最老的、可能仍引用该槽位"的那次提交，**不 `vkDeviceWaitIdle`**。

验收：`--selftest 150 --material-stress`，输出
`used_before=26 used_after=26 max_pending=2 capacity=256`。

> 注意：`releaseMaterialSet()` 目前只有压测路径在调，生产路径还没有"运行期加载/卸载材质"的调用方。

## 5. resize 时的重建

`--resize-test` 与 `--render-scale` 触发。顺序是固定的：

```
帧边界等 GPU 完成
  -> Swapchain 原地 recreate
  -> render-finished semaphore 与 ImGui 后端重建
  -> RenderGraph owned texture 原位 resize（按 Pass 顺序：Geometry -> SSAO -> Lighting -> Bloom -> Present）
  -> 各 Pass 重写依赖 extent 的描述符视图
```

**管线不重建**：所有图形管线都用动态 viewport/scissor，分辨率变化只改动态状态。

## 6. 内存（VMA）

- 分配器在 `Device` 里创建，`vmaDestroyAllocator` 在 Device 析构时调用。
- 所有分配通过 `VmaNaming.h` 附名字，便于泄漏日志定位。
- 排查泄漏：

```powershell
cmake -S . -B build -DSENTINEL_VMA_LEAK_CHECK=ON
```

打开后 VMA 销毁分配器时逐条打印未释放的分配及其名字（默认只有一句断言）。

## 7. 检查清单

新增资源时：

- [ ] 是"每帧重写"还是"常驻"？前者按帧槽开两份。
- [ ] 所有者是谁？在 `~FirstApp` 的销毁顺序里位置对吗？
- [ ] 若延迟释放，用的是 timeline 还是 `vkQueueWaitIdle`？为什么？
- [ ] resize 时会被重建吗？依赖它 view 的描述符会重写吗？
- [ ] 跑 `--selftest 60 --resize-test` 与 `--selftest 150 --material-stress`
