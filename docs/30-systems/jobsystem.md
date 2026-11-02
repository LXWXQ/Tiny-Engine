---
id: systems/jobsystem
title: CPU 作业系统
type: system
scope: src/Core/JobSystem.h, src/Core/JobSystem.cpp
read-when: 需要并行 CPU 任务，或要改线程池调度
size: ~140 行
updated: 2026-11-01
---

# CPU 作业系统

## 1. 接口

```cpp
JobSystem jobs;                                       // 默认 hardware_concurrency - 1 个 worker

JobHandle h = jobs.submit([]{ /* work */ });          // 提交单个任务
h.wait();                                             // 等待，异常会重新抛到调用线程

jobs.parallelFor(0, 100000, grain, [](size_t i){ ... });  // 按 chunk 并行
```

## 2. 当前实现

| 特性 | 状态 |
|---|---|
| 全局任务队列 | 有 |
| worker pool | 有（`std::thread`） |
| `submit` / `JobHandle::wait` | 有 |
| `parallelFor` | 有（按 grain 切 chunk） |
| 异常传播 | 有（`wait()` 重新抛回调用线程） |
| worker 内嵌套 `parallelFor` | **顺序退化为同步执行**（防止线程互相等待死锁） |
| work stealing | 无 |
| 任务依赖图 | 无 |
| 取消 | 无 |
| 优先级 | 无 |

## 3. 为什么是"正确性优先的基础版"

地形/体素接入前，任务粒度大且独立，全局队列足够。上 work stealing 和依赖图
会引入新的一类并发 bug（steal 的 ABA、依赖环、取消时的资源泄漏），
在没有 profile 数据证明需要之前不值得。

**升级触发条件**：出现大量短任务或递归分块（例如体素 chunk 递归细分）时。

## 4. 用法约定

- 任务里**不要碰 Vulkan 对象**。Vulkan 调用必须发生在提交线程或显式加锁。
- 任务里不要依赖执行顺序——`parallelFor` 的 chunk 顺序与完成顺序都不保证。
- `JobHandle` 必须 `wait()` 或让它析构（析构会等待），否则可能在工作线程里访问已销毁的捕获对象。

### 4.1 地形异步约定（D-23）

`Scene::requestRegenerateTerrain()` 是当前第一个正式使用方：

- worker 只执行 `HeightField` + `TerrainMeshBuilder` 的纯 CPU 构建；
- `Scene::update()` 在主线程用 `JobHandle::isComplete()` 轮询，完成后才做
  `Model` 的 Vulkan staging 上传；
- 每次请求带全局递增 request serial，新请求 supersede 旧请求，并把旧 task 的
  `cancelRequested` atomic 置位；`HeightField`/`MeshBuilder` 每行轮询后尽早退出；
  即使旧任务跑完，结果与回调也会被丢弃；
- 回调永远在主线程执行，编辑器据此构造可撤销的 `RegenerateTerrainCommand`；
- Scene 清理会先等待所有未完成任务，避免 worker 写已销毁的 task 对象。

当前取消是 **Scene 级协作取消**；JobSystem 核心仍没有通用取消令牌或硬中断。
如果以后需要通用停止语义，必须把取消令牌纳入任务 API 并明确 task 生命周期，
不能只从容器里删掉任务。

## 5. 自检

```powershell
.\\build\\Debug\\TinyEngine.exe --jobtest
```

输出：`[JobSystem] selfTest PASS | workers=15 | 512 submit + parallelFor(100000) + nested + exception propagation + parallelFor exception`

覆盖：512 个独立 submit、`parallelFor(100000)` 逐元素校验、worker 内嵌套
`parallelFor`、异常传播、`parallelFor` 内异常。
