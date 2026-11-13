#include "Core/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    // 当前线程是不是 worker（是则记录下标，否则 kExternalThread）。
    thread_local uint32_t t_workerIndex = 0xFFFFFFFFu;
}

// 任务节点：句柄、依赖、取消都挂在它上面。
// 定义在 .cpp 里，头文件只前置声明 —— 句柄只是 shared_ptr，不需要完整类型。
struct JobTask
{
    std::function<void()> fn;
    JobSystem::CancelTokenPtr token;
    JobSystem* owner = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    bool cancelled = false;
    std::exception_ptr error;
    std::atomic<bool> completedFlag{ false };

    std::atomic<uint32_t> pendingDependencies{ 0 };
    std::mutex dependentsMutex;
    std::vector<std::shared_ptr<JobTask>> dependents;
};

struct JobSystem::WorkerQueue
{
    std::mutex mutex;
    std::deque<std::shared_ptr<JobTask>> tasks;
};

bool JobSystem::JobHandle::isComplete() const
{
    if (!task) return true;
    return task->completedFlag.load(std::memory_order_acquire);
}

bool JobSystem::JobHandle::cancelled() const
{
    if (!task) return false;
    std::lock_guard<std::mutex> lock(task->mutex);
    return task->cancelled;
}

void JobSystem::JobHandle::wait() const
{
    if (!task) return;

    // 等待期间帮忙执行就绪任务：否则 worker 里 submit + wait 会在
    // "所有 worker 都在等"时互相卡死（旧版靠 parallelFor 退化规避，
    // 但 submit 的嵌套是真实存在的用法）。
    while (!task->completedFlag.load(std::memory_order_acquire))
    {
        JobSystem* owner = task->owner;
        if (owner != nullptr && owner->tryExecuteOne(t_workerIndex))
            continue;

        std::unique_lock<std::mutex> lock(task->mutex);
        task->cv.wait_for(lock, std::chrono::milliseconds(1),
                          [&] { return task->completed; });
    }

    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        error = task->error;
    }
    if (error)
        std::rethrow_exception(error);
}

JobSystem::JobSystem(uint32_t threadCount)
{
    if (threadCount == 0)
    {
        const uint32_t hardware = std::thread::hardware_concurrency();
        // 调用线程本身也要工作（parallelFor 的提交/等待、未来主线程 chunk），
        // 所以默认只创建 hardware-1 个 worker。
        threadCount = hardware > 1 ? hardware - 1 : 1;
    }

    workers.reserve(threadCount);
    queues.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i)
        queues.push_back(std::make_unique<WorkerQueue>());
    for (uint32_t i = 0; i < threadCount; ++i)
        workers.emplace_back([this, i] { workerLoop(i); });

    std::cout << "[JobSystem] " << workers.size() << " worker threads ready (hardware_concurrency="
              << std::thread::hardware_concurrency() << ", scheduling=work-stealing)\n";
}

JobSystem::~JobSystem()
{
    shutdown();
}

JobSystem::JobHandle JobSystem::submit(Function fn, CancelTokenPtr token)
{
    if (!fn)
        throw std::runtime_error("JobSystem: submit 收到了空任务");

    auto task = std::make_shared<JobTask>();
    task->owner = this;
    task->fn = std::move(fn);
    task->token = std::move(token);
    submittedCount.fetch_add(1, std::memory_order_relaxed);
    pushReady(task);
    return JobHandle(std::move(task));
}

JobSystem::JobHandle JobSystem::submitAfter(std::vector<JobHandle> dependencies, Function fn,
                                            CancelTokenPtr token)
{
    if (!fn)
        throw std::runtime_error("JobSystem: submitAfter 收到了空任务");

    auto task = std::make_shared<JobTask>();
    task->owner = this;
    task->fn = std::move(fn);
    task->token = std::move(token);
    submittedCount.fetch_add(1, std::memory_order_relaxed);

    uint32_t pending = 0;
    for (const JobHandle& dependency : dependencies)
    {
        if (!dependency.task)
            continue;
        if (dependency.task->completedFlag.load(std::memory_order_acquire))
            continue;

        std::lock_guard<std::mutex> lock(dependency.task->dependentsMutex);
        // 拿到 dependentsMutex 之后再判一次：依赖可能刚好在这两条语句之间
        // 完成，那时它的 dependents 已经交换走了，我们挂上去就永远等不到。
        if (dependency.task->completedFlag.load(std::memory_order_acquire))
            continue;
        ++pending;
        dependency.task->dependents.push_back(task);
    }

    task->pendingDependencies.store(pending, std::memory_order_relaxed);
    if (pending == 0)
        pushReady(task);

    // pushReady 按值接收 shared_ptr，里面的 std::move 只移动形参，
    // 调用方仍持有同一个节点，因此这里可以安全地交出一个有效句柄。
    return JobHandle(std::move(task));
}

void JobSystem::parallelFor(uint32_t begin, uint32_t end, uint32_t grain,
                            const std::function<void(uint32_t, uint32_t)>& fn,
                            CancelTokenPtr token)
{
    if (begin >= end)
        return;
    if (!fn)
        throw std::runtime_error("JobSystem: parallelFor 收到了空函数");

    grain = std::max(1u, grain);

    // worker 内递归 parallelFor 如果继续提交并等待，在所有 worker 都阻塞时
    // 会互相死锁（等待会帮忙执行，但可能撞上自己正在等的依赖链）。
    // 保守策略：worker 内顺序执行，并逐块检查取消。
    if (t_workerIndex != kExternalThread)
    {
        for (uint32_t it = begin; it < end; it += grain)
        {
            if (token && token->cancelled())
                return;
            fn(it, std::min(end, it + grain));
        }
        return;
    }

    std::vector<JobHandle> handles;
    const uint32_t chunkCount = (end - begin + grain - 1) / grain;
    handles.reserve(chunkCount);

    for (uint32_t it = begin; it < end; it += grain)
    {
        if (token && token->cancelled())
            break;
        const uint32_t chunkBegin = it;
        const uint32_t chunkEnd = std::min(end, it + grain);
        handles.push_back(submit([&fn, chunkBegin, chunkEnd] { fn(chunkBegin, chunkEnd); }, token));
    }

    // 必须等所有 chunk 都结束后再抛异常。否则第一个 wait 抛错时 local `fn`
    // 已离开作用域，而其他 worker 仍持有对它的引用，会访问悬空对象。
    std::exception_ptr firstError;
    for (const JobHandle& handle : handles)
    {
        try
        {
            handle.wait();
        }
        catch (...)
        {
            if (!firstError)
                firstError = std::current_exception();
        }
    }
    if (firstError)
        std::rethrow_exception(firstError);
}

void JobSystem::waitIdle()
{
    for (;;)
    {
        if (readyCount.load(std::memory_order_acquire) == 0 &&
            activeCount.load(std::memory_order_acquire) == 0)
            return;

        if (tryExecuteOne(t_workerIndex))
            continue;

        std::unique_lock<std::mutex> lock(injectionMutex);
        idleCv.wait_for(lock, std::chrono::milliseconds(1));
    }
}

JobSystem::Stats JobSystem::stats() const
{
    Stats result;
    result.submitted = submittedCount.load(std::memory_order_relaxed);
    result.executed = executedCount.load(std::memory_order_relaxed);
    result.skippedCancelled = skippedCancelledCount.load(std::memory_order_relaxed);
    result.stealAttempts = stealAttemptCount.load(std::memory_order_relaxed);
    result.stolen = stolenCount.load(std::memory_order_relaxed);
    result.maxQueueDepth = maxQueueDepth.load(std::memory_order_relaxed);
    return result;
}

void JobSystem::pushReady(std::shared_ptr<JobTask> task)
{
    if (stopping.load(std::memory_order_relaxed))
        throw std::runtime_error("JobSystem: 已 shutdown，不能继续提交任务");

    const uint32_t index = t_workerIndex;
    if (index < queues.size())
    {
        // 自己产生的任务压自己的队头：LIFO 让"刚切出来的子块"优先跑完，
        // 递归分块时数据还在缓存里。
        WorkerQueue& own = *queues[index];
        std::lock_guard<std::mutex> lock(own.mutex);
        own.tasks.push_front(std::move(task));
    }
    else
    {
        std::lock_guard<std::mutex> lock(injectionMutex);
        injection.push_back(std::move(task));
    }

    const uint32_t depth = readyCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    uint64_t previousDepth = maxQueueDepth.load(std::memory_order_relaxed);
    while (depth > previousDepth &&
           !maxQueueDepth.compare_exchange_weak(previousDepth, depth, std::memory_order_relaxed))
    {
    }
    workCv.notify_one();
}

std::shared_ptr<JobTask> JobSystem::takeTask(uint32_t workerIndex)
{
    // 1) 自己的队列（LIFO）
    if (workerIndex < queues.size())
    {
        WorkerQueue& own = *queues[workerIndex];
        std::lock_guard<std::mutex> lock(own.mutex);
        if (!own.tasks.empty())
        {
            std::shared_ptr<JobTask> task = std::move(own.tasks.front());
            own.tasks.pop_front();
            readyCount.fetch_sub(1, std::memory_order_acq_rel);
            return task;
        }
    }

    // 2) injection 队列（外部提交，FIFO）
    {
        std::lock_guard<std::mutex> lock(injectionMutex);
        if (!injection.empty())
        {
            std::shared_ptr<JobTask> task = std::move(injection.front());
            injection.pop_front();
            readyCount.fetch_sub(1, std::memory_order_acq_rel);
            return task;
        }
    }

    // 3) 从别人的队尾偷
    const uint32_t count = static_cast<uint32_t>(queues.size());
    if (count == 0)
        return nullptr;

    const uint32_t start = (workerIndex < count) ? workerIndex + 1 : 0;
    for (uint32_t offset = 0; offset < count; ++offset)
    {
        const uint32_t victim = (start + offset) % count;
        if (victim == workerIndex)
            continue;

        WorkerQueue& queue = *queues[victim];
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (queue.tasks.empty())
                continue;

            stealAttemptCount.fetch_add(1, std::memory_order_relaxed);
            std::shared_ptr<JobTask> task = std::move(queue.tasks.back());
            queue.tasks.pop_back();
            readyCount.fetch_sub(1, std::memory_order_acq_rel);
            stolenCount.fetch_add(1, std::memory_order_relaxed);
            return task;
        }
    }
    return nullptr;
}

bool JobSystem::tryExecuteOne(uint32_t workerIndex)
{
    std::shared_ptr<JobTask> task = takeTask(workerIndex);
    if (!task)
        return false;
    runTask(std::move(task));
    return true;
}

void JobSystem::runTask(std::shared_ptr<JobTask> task)
{
    activeCount.fetch_add(1, std::memory_order_acq_rel);

    if (task->token && task->token->cancelled())
    {
        skippedCancelledCount.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->cancelled = true;
        }
    }
    else
    {
        try
        {
            task->fn();
            executedCount.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->error = std::current_exception();
        }
    }

    completeTask(task);

    activeCount.fetch_sub(1, std::memory_order_acq_rel);
    idleCv.notify_all();
}

void JobSystem::completeTask(const std::shared_ptr<JobTask>& task)
{
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->completed = true;
    }
    task->completedFlag.store(true, std::memory_order_release);
    task->cv.notify_all();

    std::vector<std::shared_ptr<JobTask>> dependents;
    {
        std::lock_guard<std::mutex> lock(task->dependentsMutex);
        dependents.swap(task->dependents);
    }

    for (const std::shared_ptr<JobTask>& dependent : dependents)
    {
        if (dependent->pendingDependencies.fetch_sub(1, std::memory_order_acq_rel) == 1)
            pushReady(dependent);
    }
}

void JobSystem::workerLoop(uint32_t index)
{
    t_workerIndex = index;

    for (;;)
    {
        if (tryExecuteOne(index))
            continue;

        std::unique_lock<std::mutex> lock(injectionMutex);
        workCv.wait_for(lock, std::chrono::milliseconds(2), [&]
        {
            return stopping.load(std::memory_order_relaxed) ||
                   readyCount.load(std::memory_order_acquire) > 0;
        });

        if (stopping.load(std::memory_order_relaxed) &&
            readyCount.load(std::memory_order_acquire) == 0)
            return;
    }
}

void JobSystem::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(injectionMutex);
        if (stopping.load(std::memory_order_relaxed))
            return;
        stopping.store(true, std::memory_order_relaxed);
    }
    workCv.notify_all();

    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();

    workers.clear();

    // 被销毁时仍有未执行完的任务，说明调用方没有等待句柄。
    uint32_t remaining = readyCount.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(injectionMutex);
        remaining = static_cast<uint32_t>(injection.size());
    }
    if (remaining > 0)
        std::cerr << "[JobSystem] 警告: shutdown 时还有 " << remaining
                  << " 个未执行任务被丢弃\n";
}

bool JobSystem::selfTest()
{
    bool ok = true;
    auto expect = [&ok](bool condition, const char* message)
    {
        if (!condition)
        {
            ok = false;
            std::cerr << "[JobSystem] selfTest FAIL: " << message << "\n";
        }
    };

    // 1) 512 个独立作业：全部执行、句柄有效
    {
        std::atomic<uint32_t> counter{ 0 };
        std::vector<JobHandle> handles;
        handles.reserve(512);
        for (uint32_t i = 0; i < 512; ++i)
        {
            handles.push_back(submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }));
        }
        for (const JobHandle& handle : handles)
        {
            expect(handle.valid(), "submit 返回了无效句柄");
            handle.wait();
        }
        expect(counter.load() == 512, "512 个 submit 作业没有全部执行");
    }

    // 2) 依赖链 A -> B -> C：执行顺序必须严格递增
    {
        std::mutex orderMutex;
        std::vector<int> order;
        auto record = [&order, &orderMutex](int value)
        {
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back(value);
        };

        JobHandle a = submit([&record] { record(0); });
        JobHandle b = submitAfter({ a }, [&record] { record(1); });
        JobHandle c = submitAfter({ b }, [&record] { record(2); });
        c.wait();
        expect(order.size() == 3, "依赖链没有执行完");
        if (order.size() == 3)
            expect(order[0] == 0 && order[1] == 1 && order[2] == 2, "依赖链执行顺序错误");
    }

    // 3) 菱形依赖 A -> {B,C} -> D：D 必须在 B、C 都完成后才跑
    {
        std::atomic<uint32_t> finished{ 0 };
        std::atomic<uint32_t> observedAtJoin{ 0xFFFFFFFFu };
        std::mutex orderMutex;
        std::vector<std::string> order;

        JobHandle a = submit([&] { std::lock_guard<std::mutex> lock(orderMutex); order.push_back("A"); });
        JobHandle b = submitAfter({ a }, [&]
        {
            finished.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back("B");
        });
        JobHandle c = submitAfter({ a }, [&]
        {
            finished.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back("C");
        });
        JobHandle d = submitAfter({ b, c }, [&]
        {
            observedAtJoin.store(finished.load(std::memory_order_relaxed));
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back("D");
        });
        d.wait();

        expect(observedAtJoin.load() == 2, "菱形依赖的汇合点在依赖未完成时就开始执行");
        expect(order.size() == 4, "菱形依赖没有执行完");
        if (order.size() == 4)
            expect(order.front() == "A" && order.back() == "D", "菱形依赖执行顺序错误");
    }

    // 4) 提交前取消：不执行、句柄标记 cancelled
    {
        auto token = std::make_shared<CancelToken>();
        token->cancel();
        std::atomic<uint32_t> ran{ 0 };
        JobHandle handle = submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, token);
        handle.wait();
        expect(handle.valid(), "取消用例的句柄无效");
        expect(ran.load() == 0, "已取消的作业仍被执行");
        expect(handle.cancelled(), "跳过的作业没有标记 cancelled");
    }

    // 5) 运行中取消：从 cancel 到 wait 返回必须 < 500ms
    {
        auto token = std::make_shared<CancelToken>();
        std::atomic<bool> started{ false };
        JobHandle handle = submit([&token, &started]
        {
            started.store(true, std::memory_order_relaxed);
            while (!token->cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }, token);

        while (!started.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        const auto begin = std::chrono::steady_clock::now();
        token->cancel();
        handle.wait();
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        expect(elapsedMs < 500.0, "取消延迟超过 500ms");
    }

    // 6) 异常传播：submit 与 parallelFor 都必须把异常带回调用方
    {
        JobHandle handle = submit([] { throw std::runtime_error("job boom"); });
        bool caught = false;
        try
        {
            handle.wait();
        }
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        expect(caught, "submit 作业抛出的异常没有从 wait 重新抛出");

        bool forCaught = false;
        try
        {
            parallelFor(0, 4096, 256, [](uint32_t begin, uint32_t) {
                if (begin == 1024)
                    throw std::runtime_error("chunk boom");
            });
        }
        catch (const std::runtime_error&)
        {
            forCaught = true;
        }
        expect(forCaught, "parallelFor 的异常没有抛回调用方");
    }

    // 7) parallelFor 覆盖：100k 个元素每个恰好被写一次
    {
        constexpr uint32_t kCount = 100000;
        std::vector<std::atomic<uint32_t>> hits(kCount);
        for (std::atomic<uint32_t>& hit : hits)
            hit.store(0, std::memory_order_relaxed);

        parallelFor(0, kCount, 1024, [&hits](uint32_t begin, uint32_t end)
        {
            for (uint32_t i = begin; i < end; ++i)
                hits[i].fetch_add(1, std::memory_order_relaxed);
        });

        uint32_t bad = 0;
        for (const std::atomic<uint32_t>& hit : hits)
        {
            if (hit.load(std::memory_order_relaxed) != 1)
                ++bad;
        }
        expect(bad == 0, "parallelFor 的区间划分有重叠或遗漏");
    }

    // 8) 吞吐：同样的 CPU 工作量，串行 vs 并行（workerCount >= 2 才断言加速比）
    {
        constexpr uint32_t kTasks = 32;
        constexpr uint32_t kIterations = 1u << 17;
        auto workload = [](uint32_t iterations)
        {
            volatile double sum = 0.0;
            for (uint32_t i = 1; i <= iterations; ++i)
                sum = sum + std::sqrt(static_cast<double>(i)) / static_cast<double>(i);
            return sum;
        };

        const auto serialBegin = std::chrono::steady_clock::now();
        volatile double serialSum = 0.0;
        for (uint32_t i = 0; i < kTasks; ++i)
            serialSum = serialSum + workload(kIterations);
        const double serialMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - serialBegin).count();

        const auto parallelBegin = std::chrono::steady_clock::now();
        std::vector<JobHandle> handles;
        handles.reserve(kTasks);
        for (uint32_t i = 0; i < kTasks; ++i)
            handles.push_back(submit([workload, kIterations] { workload(kIterations); }));
        for (const JobHandle& handle : handles)
            handle.wait();
        const double parallelMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - parallelBegin).count();

        const double speedup = parallelMs > 0.0 ? serialMs / parallelMs : 0.0;
        if (workerCount() >= 2)
            expect(speedup >= 1.5, "并行加速比低于 1.5x");

        std::cout << "[JobSystem] 吞吐 串行 " << serialMs << "ms | 并行 " << parallelMs
                  << "ms | 加速 " << speedup << "x | workers=" << workerCount() << "\n";
    }

    const Stats snapshot = stats();
    if (ok)
    {
        std::cout << "[JobSystem] selfTest PASS | workers=" << workerCount()
                  << " cases=8 executed=" << snapshot.executed
                  << " stolen=" << snapshot.stolen << " skipped=" << snapshot.skippedCancelled
                  << "\n";
    }
    return ok;
}
