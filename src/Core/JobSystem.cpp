#include "Core/JobSystem.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
    thread_local bool t_isJobWorker = false;
}

bool JobSystem::JobHandle::isComplete() const
{
    if (!state) return true;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->completed;
}

void JobSystem::JobHandle::wait() const
{
    if (!state) return;

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&] { return state->completed; });
    if (state->error)
        std::rethrow_exception(state->error);
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
    for (uint32_t i = 0; i < threadCount; ++i)
        workers.emplace_back([this] { workerLoop(); });

    std::cout << "[JobSystem] " << workers.size() << " worker threads ready (hardware_concurrency="
              << std::thread::hardware_concurrency() << ")\n";
}

JobSystem::~JobSystem()
{
    shutdown();
}

JobSystem::JobHandle JobSystem::submit(Function fn)
{
    if (!fn)
        throw std::runtime_error("JobSystem: submit 收到了空任务");

    auto state = std::make_shared<JobHandle::State>();
    enqueue(Task{ state, std::move(fn) });
    return JobHandle(std::move(state));
}

void JobSystem::parallelFor(uint32_t begin, uint32_t end, uint32_t grain,
                            const std::function<void(uint32_t, uint32_t)>& fn)
{
    if (begin >= end)
        return;
    if (!fn)
        throw std::runtime_error("JobSystem: parallelFor 收到了空函数");

    grain = std::max(1u, grain);

    // worker 内递归 parallelFor 如果继续提交并等待，在所有 worker 都阻塞时
    // 会互相死锁。D-6 先采用安全退化：worker 内顺序执行。
    if (t_isJobWorker)
    {
        for (uint32_t it = begin; it < end; it += grain)
            fn(it, std::min(end, it + grain));
        return;
    }

    std::vector<JobHandle> handles;
    const uint32_t chunkCount = (end - begin + grain - 1) / grain;
    handles.reserve(chunkCount);

    for (uint32_t it = begin; it < end; it += grain)
    {
        const uint32_t chunkBegin = it;
        const uint32_t chunkEnd = std::min(end, it + grain);
        handles.push_back(submit([&fn, chunkBegin, chunkEnd] { fn(chunkBegin, chunkEnd); }));
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
    std::unique_lock<std::mutex> lock(queueMutex);
    idleCv.wait(lock, [&] { return tasks.empty() && activeTasks == 0; });
}

void JobSystem::enqueue(Task task)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping)
            throw std::runtime_error("JobSystem: 已 shutdown，不能继续提交任务");
        tasks.push(std::move(task));
    }
    queueCv.notify_one();
}

void JobSystem::workerLoop()
{
    t_isJobWorker = true;

    for (;;)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [&] { return stopping || !tasks.empty(); });
            if (stopping && tasks.empty())
                return;

            task = std::move(tasks.front());
            tasks.pop();
            ++activeTasks;
        }

        try
        {
            task.fn();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(task.state->mutex);
            task.state->error = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> lock(task.state->mutex);
            task.state->completed = true;
        }
        task.state->cv.notify_all();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            --activeTasks;
        }
        idleCv.notify_all();
    }
}

void JobSystem::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping)
            return;
        stopping = true;
    }
    queueCv.notify_all();

    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();

    workers.clear();

    // 被销毁时仍有未执行完的任务，说明调用方没有等待句柄。
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!tasks.empty())
        std::cerr << "[JobSystem] 警告: shutdown 时还有 " << tasks.size()
                  << " 个未执行任务被丢弃\n";
}

bool JobSystem::selfTest()
{
    bool ok = true;

    // 1) 基本 submit / wait：所有任务必须各执行一次。
    std::atomic<uint32_t> completed{ 0 };
    std::vector<JobHandle> handles;
    handles.reserve(512);
    for (uint32_t i = 0; i < 512; ++i)
        handles.push_back(submit([&completed] { completed.fetch_add(1, std::memory_order_relaxed); }));
    for (const JobHandle& handle : handles)
        handle.wait();

    if (completed.load() != 512)
    {
        std::cerr << "[JobSystem] selfTest FAIL: submit count=" << completed.load() << " expected 512\n";
        ok = false;
    }

    // 2) parallelFor：每块写入互不重叠的区间，然后校验结果。
    constexpr uint32_t kValueCount = 100000;
    std::vector<uint32_t> values(kValueCount, 0);
    parallelFor(0, kValueCount, 1024, [&values](uint32_t begin, uint32_t end)
    {
        for (uint32_t i = begin; i < end; ++i)
            values[i] = i * 2u + 1u;
    });

    for (uint32_t i = 0; i < kValueCount; ++i)
    {
        if (values[i] != i * 2u + 1u)
        {
            std::cerr << "[JobSystem] selfTest FAIL: parallelFor mismatch at " << i << "\n";
            ok = false;
            break;
        }
    }

    // 3) worker 内嵌套 parallelFor 必须安全退化，不能所有 worker 互相等待死锁。
    std::atomic<uint32_t> nestedSum{ 0 };
    JobHandle nested = submit([this, &nestedSum]
    {
        parallelFor(0, 128, 8, [&nestedSum](uint32_t begin, uint32_t end)
        {
            nestedSum.fetch_add(end - begin, std::memory_order_relaxed);
        });
    });
    nested.wait();
    if (nestedSum.load() != 128)
    {
        std::cerr << "[JobSystem] selfTest FAIL: nested parallelFor sum=" << nestedSum.load() << "\n";
        ok = false;
    }

    // 4) 异常必须从 JobHandle::wait 重新抛回调用线程。
    bool caught = false;
    try
    {
        JobHandle failing = submit([] { throw std::runtime_error("intentional job failure"); });
        failing.wait();
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }
    if (!caught)
    {
        std::cerr << "[JobSystem] selfTest FAIL: exception was not propagated\n";
        ok = false;
    }

    // 5) parallelFor 中某个 chunk 抛错时，必须等所有 chunk 结束再统一抛回。
    bool parallelCaught = false;
    try
    {
        parallelFor(0, 64, 8, [](uint32_t, uint32_t)
        {
            throw std::runtime_error("intentional parallelFor failure");
        });
    }
    catch (const std::runtime_error&)
    {
        parallelCaught = true;
    }
    if (!parallelCaught)
    {
        std::cerr << "[JobSystem] selfTest FAIL: parallelFor exception was not propagated\n";
        ok = false;
    }

    waitIdle();
    std::cout << "[JobSystem] selfTest " << (ok ? "PASS" : "FAIL")
              << " | workers=" << workerCount()
              << " | 512 submit + parallelFor(" << kValueCount << ") + nested + exception propagation + parallelFor exception\n";
    return ok;
}
