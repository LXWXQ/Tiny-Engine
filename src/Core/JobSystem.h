#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// D-6：通用 CPU 作业系统 / 线程池。
//
// 设计目标不是做完整的 fiber 调度器，而是先给地形 / 体素生成提供可靠底座：
//   * submit()       -> 提交任意 CPU 作业，返回可等待句柄；
//   * parallelFor()  -> 把 [begin,end) 切成 chunk 并行执行，内部等待全部完成；
//   * selfTest()     -> 无人值守验证并发结果、异常传播和 parallelFor。
//
// 当前版本使用一个全局任务队列 + condition_variable。相比 per-thread deque +
// work stealing，它更容易证明正确性；等 terrain/voxel 真正把 chunk 数量做大后，
// 再根据 profile 决定是否升级。
class JobSystem
{
public:
    class JobHandle
    {
    public:
        JobHandle() = default;

        bool valid() const { return state != nullptr; }
        bool isComplete() const;
        void wait() const;   // 若作业抛出异常，这里会重新抛出

    private:
        struct State
        {
            std::mutex mutex;
            std::condition_variable cv;
            bool completed = false;
            std::exception_ptr error;
        };

        explicit JobHandle(std::shared_ptr<State> state) : state(std::move(state)) {}

        std::shared_ptr<State> state;
        friend class JobSystem;
    };

    using Function = std::function<void()>;

    explicit JobSystem(uint32_t threadCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    JobHandle submit(Function fn);

    // 把 [begin,end) 按 grain 切成块并等待完成。grain 是每块元素数。
    // 从 worker 线程调用时退化为当前线程顺序执行，避免 worker 互相等待造成死锁；
    // terrain 系统的递归分块需要 work stealing 时再扩展这一策略。
    void parallelFor(uint32_t begin, uint32_t end, uint32_t grain,
                     const std::function<void(uint32_t, uint32_t)>& fn);

    void waitIdle();
    uint32_t workerCount() const { return static_cast<uint32_t>(workers.size()); }

    // 创建一组确定性验证作业。返回 false 表示作业系统自身有问题。
    bool selfTest();

private:
    struct Task
    {
        std::shared_ptr<JobHandle::State> state;
        Function fn;
    };

    void enqueue(Task task);
    void workerLoop();
    void shutdown();

    std::vector<std::thread> workers;
    std::queue<Task> tasks;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::condition_variable idleCv;
    uint32_t activeTasks = 0;
    bool stopping = false;
};
