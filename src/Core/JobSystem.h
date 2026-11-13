#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct JobTask;

// D-6 / M1-C2：通用 CPU 作业系统 / 线程池。
//
// 设计目标不是做完整的 fiber 调度器，而是给地形 / 体素生成提供可靠底座：
//   * submit()       -> 提交任意 CPU 作业，返回可等待句柄；
//   * submitAfter()  -> 声明依赖：所有依赖完成后作业才会入队；
//   * parallelFor()  -> 把 [begin,end) 切成 chunk 并行执行，内部等待全部完成；
//   * CancelToken    -> 取消：未开始的任务直接跳过，长任务自己轮询；
//   * selfTest()     -> 无人值守验证并发结果、依赖顺序、取消延迟与吞吐。
//
// M1-C2 把调度器从"一个全局队列 + condition_variable"换成
// **per-worker 双端队列 + work stealing**：
//   * worker 自己产生的任务压进自己的队列（push_front / pop_front，LIFO），
//     这样递归分块（地形 chunk 再切 LOD）在同一线程上保持数据局部性；
//   * 外部线程 submit 的任务进 injection 队列（FIFO），保证提交顺序；
//   * 空闲 worker 从别人的队列**尾部**偷（pop_back），偷到的是最早产生、
//     粒度最大的任务，负载均衡效果最好；
//   * 等待中的线程（包括主线程）会"帮忙执行"就绪任务，而不是纯阻塞 ——
//     这是嵌套 submit + wait 不死锁的关键。
//
// 依赖用引用计数实现：提交时把新任务挂到每个依赖的 dependents 上，
// 依赖完成时把计数减一，减到 0 才入队。取消用 CancelToken（共享的
// atomic<bool>），worker 在取到任务后、执行前检查一次；已经跑起来的长任务
// 需要自己轮询 token。
class JobSystem
{
public:
    class CancelToken
    {
    public:
        void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
        bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

    private:
        std::atomic<bool> cancelled_{ false };
    };
    using CancelTokenPtr = std::shared_ptr<CancelToken>;

    class JobHandle
    {
    public:
        JobHandle() = default;

        bool valid() const { return task != nullptr; }
        bool isComplete() const;
        // 任务因 token 被取消而跳过（未执行 fn）时为 true。
        bool cancelled() const;
        void wait() const;   // 若作业抛出异常，这里会重新抛出

    private:
        explicit JobHandle(std::shared_ptr<JobTask> task) : task(std::move(task)) {}

        std::shared_ptr<JobTask> task;
        friend class JobSystem;
    };

    using Function = std::function<void()>;

    struct Stats
    {
        uint64_t submitted = 0;
        uint64_t executed = 0;
        uint64_t skippedCancelled = 0;
        uint64_t stealAttempts = 0;
        uint64_t stolen = 0;
        uint64_t maxQueueDepth = 0;
    };

    explicit JobSystem(uint32_t threadCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    JobHandle submit(Function fn, CancelTokenPtr token = nullptr);

    // 所有依赖完成后才入队。已完成的依赖会被跳过；同一个句柄重复出现
    // 视为两次依赖（计数会减两次，语义上仍然是"全部完成"）。
    JobHandle submitAfter(std::vector<JobHandle> dependencies, Function fn,
                          CancelTokenPtr token = nullptr);

    // 把 [begin,end) 按 grain 切成块并等待完成。grain 是每块元素数。
    // 从 worker 线程调用时退化为当前线程顺序执行，避免 worker 互相等待造成死锁；
    // 传入 token 时，取消后不再提交新块，已提交的块会各自尽快返回。
    void parallelFor(uint32_t begin, uint32_t end, uint32_t grain,
                     const std::function<void(uint32_t, uint32_t)>& fn,
                     CancelTokenPtr token = nullptr);

    void waitIdle();
    uint32_t workerCount() const { return static_cast<uint32_t>(workers.size()); }
    Stats stats() const;

    // 创建一组确定性验证作业。返回 false 表示作业系统自身有问题。
    bool selfTest();

private:
    struct WorkerQueue;

    static constexpr uint32_t kExternalThread = 0xFFFFFFFFu;

    void pushReady(std::shared_ptr<JobTask> task);
    std::shared_ptr<JobTask> takeTask(uint32_t workerIndex);
    bool tryExecuteOne(uint32_t workerIndex);
    void runTask(std::shared_ptr<JobTask> task);
    void completeTask(const std::shared_ptr<JobTask>& task);
    void workerLoop(uint32_t index);
    void shutdown();

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<WorkerQueue>> queues;

    std::mutex injectionMutex;
    std::deque<std::shared_ptr<JobTask>> injection;
    std::condition_variable workCv;
    std::condition_variable idleCv;

    std::atomic<uint32_t> readyCount{ 0 };
    std::atomic<uint32_t> activeCount{ 0 };
    std::atomic<bool> stopping{ false };

    std::atomic<uint64_t> submittedCount{ 0 };
    std::atomic<uint64_t> executedCount{ 0 };
    std::atomic<uint64_t> skippedCancelledCount{ 0 };
    std::atomic<uint64_t> stealAttemptCount{ 0 };
    std::atomic<uint64_t> stolenCount{ 0 };
    std::atomic<uint64_t> maxQueueDepth{ 0 };
};
