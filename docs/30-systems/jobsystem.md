---
id: systems/jobsystem
title: CPU 作业系统
type: system
scope: src/Core/JobSystem.h, src/Core/JobSystem.cpp
read-when: 需要并行 CPU 任务，或要改线程池调度 / 依赖 / 取消
size: ~120 行
updated: 2026-11-13
---

# CPU 作业系统

## 1. 接口

```cpp
JobSystem jobs;                                  // 默认 hardware_concurrency - 1 个 worker

JobHandle h = jobs.submit([]{ /* work */ });     // 提交单个任务
h.wait();                                        // 等待；任务抛出的异常会在这里重新抛出

// 依赖：A、B 都完成后才执行 C（依赖用引用计数，减到 0 才入队）
JobHandle a = jobs.submit(workA);
JobHandle b = jobs.submit(workB);
JobHandle c = jobs.submitAfter({ a, b }, workC);

// 并行区间：按 grain 切块，内部等全部完成
jobs.parallelFor(0, 100000, 1024, [](uint32_t begin, uint32_t end){ /* ... */ });

// 取消：未开始的任务直接跳过，已开始的任务自己轮询
auto token = std::make_shared<JobSystem::CancelToken>();
JobHandle d = jobs.submit(longWork, token);
token->cancel();

JobSystem::Stats s = jobs.stats();               // submitted/executed/skippedCancelled/stealAttempts/stolen/maxQueueDepth
```

## 2. 当前实现（M1-C2 起）

| 特性 | 状态 |
|---|---|
| worker pool | 有（`std::thread`，默认 `hardware_concurrency - 1`） |
| per-worker 双端队列 | 有（自己产生的任务 `push_front`/`pop_front`，LIFO） |
| 外部提交 injection 队列 | 有（FIFO，保持提交顺序） |
| work stealing | 有（空闲 worker 从别人队列**尾部**偷） |
| 任务依赖图 | 有（`submitAfter`，引用计数 + dependents 列表） |
| 取消 | 有（`CancelToken`，协作式） |
| `parallelFor` | 有（按 grain 切块；worker 内调用退化为顺序执行） |
| 异常传播 | 有（`wait()` 重新抛回调用线程；`parallelFor` 等全部 chunk 结束再抛） |
| 优先级 | 无 |

调度细节：

- **LIFO 取自己的任务**：递归分块时"刚切出来的子块"优先跑完，数据还在缓存里。
- **从别人队尾偷**：偷到的是最早产生、粒度最大的任务，负载均衡效果最好。
- **等待也干活**：`JobHandle::wait()` 与 `waitIdle()` 在等待期间会执行就绪任务，
  这是 worker 里 `submit` + `wait` 不死锁的关键（否则所有 worker 可能同时在等）。
- **取消是协作式的**：worker 在取到任务后、执行前检查一次 token；已经跑起来的长任务
  必须自己轮询 `token->cancelled()`。`parallelFor` 在提交下一块前检查，取消后不再提交新块。

## 3. 为什么现在才上 work stealing

地形/体素接入前，任务粒度大且独立，全局队列足够；上 work stealing 与依赖图会引入
新的一类并发 bug（steal 的竞态、依赖环、取消时的生命周期），在没有 profile 数据前不值得。

**升级触发条件已满足**：体素 chunk 递归细分 + 地形流送产生大量短任务，且主线程
在 `Scene::update()` 里轮询句柄时全程空转。C2 因此把调度器换成
"per-worker deque + injection + steal"，并保留 `parallelFor` 的对外语义不变。

## 4. 用法约定

- 任务里**不要碰 Vulkan 对象**。Vulkan 调用必须发生在提交线程或显式加锁。
- 任务里不要依赖执行顺序——`parallelFor` 的 chunk 顺序与完成顺序都不保证。
- `JobHandle` 必须 `wait()` 或让它析构（析构会等待），否则可能在工作线程里访问已销毁的捕获对象。
- 依赖不能成环：`submitAfter` 不会检测环，成环的作业永远不会入队。
- `CancelToken` 只保证"未开始的不再开始"，不保证正在执行的任务立刻停下。

### 4.1 地形异步约定（D-23）

`Scene::requestRegenerateTerrain()` 是第一个正式使用方：

- worker 只执行 `HeightField` + `TerrainMeshBuilder` 的纯 CPU 构建；
- `Scene::update()` 在主线程用 `JobHandle::isComplete()` 轮询，完成后才做
  `Model` 的 Vulkan staging 上传；
- 每次请求带全局递增 request serial，新请求 supersede 旧请求，并把旧 task 的
  `cancelRequested` atomic 置位；`HeightField`/`MeshBuilder` 每行轮询后尽早退出；
  即使旧任务跑完，结果与回调也会被丢弃；
- 回调永远在主线程执行，编辑器据此构造可撤销的 `RegenerateTerrainCommand`；
- Scene 清理会先等待所有未完成任务，避免 worker 写已销毁的 task 对象。

当前 Scene 仍用自己的 `cancelRequested` 做请求级取消；`JobSystem::CancelToken`
是通用化的下一步，迁移时只要把 `cancelRequested` 换成 token 即可，轮询点不变。

## 5. 自检

```powershell
.\build\Debug\TinyEngine.exe --jobtest
```

输出示例：

```text
[JobSystem] 15 worker threads ready (hardware_concurrency=16, scheduling=work-stealing)
[JobSystem] 吞吐 串行 9.1576ms | 并行 1.4825ms | 加速 6.17713x | workers=15
[JobSystem] selfTest PASS | workers=15 cases=8 executed=665 stolen=1 skipped=1
```

覆盖 8 组用例：

1. 512 个独立 `submit` 全部执行且句柄有效；
2. 依赖链 A → B → C 的执行顺序；
3. 菱形依赖 A → {B,C} → D（汇合点必须在两个依赖都完成后才跑）；
4. 提交前取消（不执行、标记 `cancelled`）；
5. 运行中取消的延迟 < 500ms；
6. 异常传播（`submit` 与 `parallelFor` 两条路径）；
7. `parallelFor(0, 100000, 1024)` 逐元素恰好写一次（无重叠/遗漏）；
8. 吞吐对比（同样的 CPU 工作量串行 vs 并行，`workerCount >= 2` 时要求 ≥1.5×）。

验收基线（本机 16 线程 / Debug）：串行 9–12ms、并行 1.5ms，加速 6–8×。
