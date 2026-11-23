#include "FrameStats.h"

#include <cstdio>

namespace tiny::framestats
{
    namespace
    {
        struct Counters
        {
            uint64_t passes = 0;
            uint64_t skippedPasses = 0;
            uint64_t barrierCalls = 0;
            uint64_t barrierEntries = 0;
            uint64_t skippedSameState = 0;
            uint64_t skippedCrossQueue = 0;
            uint64_t pipelineBinds = 0;
            uint64_t redundantBinds = 0;
        };

        Counters g_frame;
        Counters g_total;
        uint64_t g_frames = 0;
        bool g_enabled = false;

        // 「上一次绑的是谁」。每帧复位 —— 命令缓冲每帧重录，跨帧记住的绑定是假的。
        VkPipeline g_lastGraphics = VK_NULL_HANDLE;
        VkPipeline g_lastCompute = VK_NULL_HANDLE;

        // 帧时间直方图：0.25 ms 一格，0..250 ms 共 1000 格，最后第 1001 格是溢出。
        constexpr double kFrameTimeBucketMs = 0.25;
        constexpr size_t kFrameTimeBuckets = 1000;

        struct FrameTimes
        {
            uint64_t buckets[kFrameTimeBuckets + 1] = {};
            uint64_t samples = 0;
            double maxMs = 0.0;
            double sumMs = 0.0;
        };

        FrameTimes g_frameTimes;

        double percentile(const FrameTimes& times, double q)
        {
            if (times.samples == 0) return 0.0;
            const double target = q * static_cast<double>(times.samples);
            uint64_t cumulative = 0;
            for (size_t i = 0; i <= kFrameTimeBuckets; ++i)
            {
                cumulative += times.buckets[i];
                if (static_cast<double>(cumulative) >= target)
                {
                    if (i == kFrameTimeBuckets) return times.maxMs;
                    return (static_cast<double>(i) + 0.5) * kFrameTimeBucketMs;
                }
            }
            return times.maxMs;
        }

        void accumulate(Counters& dst, const Counters& src)
        {
            dst.passes += src.passes;
            dst.skippedPasses += src.skippedPasses;
            dst.barrierCalls += src.barrierCalls;
            dst.barrierEntries += src.barrierEntries;
            dst.skippedSameState += src.skippedSameState;
            dst.skippedCrossQueue += src.skippedCrossQueue;
            dst.pipelineBinds += src.pipelineBinds;
            dst.redundantBinds += src.redundantBinds;
        }

        double perFrame(uint64_t value)
        {
            if (g_frames == 0) return 0.0;
            return static_cast<double>(value) / static_cast<double>(g_frames);
        }

        unsigned long long u64(uint64_t value)
        {
            return static_cast<unsigned long long>(value);
        }
    }

    void beginFrame()
    {
        g_frame = Counters{};
        g_lastGraphics = VK_NULL_HANDLE;
        g_lastCompute = VK_NULL_HANDLE;
    }

    void endFrame()
    {
        accumulate(g_total, g_frame);
        ++g_frames;
    }

    void countPass(bool recorded)
    {
        if (recorded) ++g_frame.passes;
        else ++g_frame.skippedPasses;
    }

    void countBarrierCall(uint32_t entries)
    {
        ++g_frame.barrierCalls;
        g_frame.barrierEntries += entries;
    }

    void countBarrierSkippedSameState()
    {
        ++g_frame.skippedSameState;
    }

    void countBarrierSkippedCrossQueue()
    {
        ++g_frame.skippedCrossQueue;
    }

    void countPipelineBind(VkPipeline pipeline, bool compute)
    {
        ++g_frame.pipelineBinds;

        VkPipeline& last = compute ? g_lastCompute : g_lastGraphics;
        if (pipeline != VK_NULL_HANDLE && pipeline == last)
            ++g_frame.redundantBinds;
        else
            last = pipeline;
    }

    void recordFrameTime(double milliseconds)
    {
        if (!(milliseconds > 0.0)) return;
        FrameTimes& times = g_frameTimes;
        if (milliseconds > times.maxMs) times.maxMs = milliseconds;
        times.sumMs += milliseconds;
        ++times.samples;
        size_t index = static_cast<size_t>(milliseconds / kFrameTimeBucketMs);
        if (index > kFrameTimeBuckets) index = kFrameTimeBuckets;
        ++times.buckets[index];
    }

    void setEnabled(bool value) { g_enabled = value; }
    bool enabled() { return g_enabled; }

    void report()
    {
        if (g_frames == 0)
        {
            std::printf("[FrameStats] 没有采集到帧（渲染图没跑过）\n");
            return;
        }

        const double redundantRate = g_total.pipelineBinds == 0
            ? 0.0
            : 100.0 * static_cast<double>(g_total.redundantBinds) /
                static_cast<double>(g_total.pipelineBinds);

        std::printf("[FrameStats] frames=%llu | passes/frame=%.1f skipped=%.1f\n",
            u64(g_frames), perFrame(g_total.passes), perFrame(g_total.skippedPasses));

        // barrierCalls 是 vkCmdPipelineBarrier2 的调用次数（批处理的收益看这个），
        // barrierEntries 是其中包了多少条 barrier（真正表达依赖的条数）。
        std::printf("[FrameStats] barriers/frame=%.1f calls %.1f entries | skipped: sameState=%.1f crossQueue=%.1f\n",
            perFrame(g_total.barrierCalls), perFrame(g_total.barrierEntries),
            perFrame(g_total.skippedSameState), perFrame(g_total.skippedCrossQueue));

        // redundant 就是「同一帧里连续绑了同一条管线」的次数 —— T-09 状态缓存的上限。
        // 它接近 pipelineBinds 说明排序缓存的收益几乎为零，改造就不必做。
        std::printf("[FrameStats] pipelineBinds/frame=%.1f redundant=%.1f (%.1f%%)\n",
            perFrame(g_total.pipelineBinds), perFrame(g_total.redundantBinds), redundantRate);

        // 平均 FPS 是钝尺子：一次 3 倍 hitch 对 600 帧的均值几乎无损，但人眼看得见。
        // median / p95 / max 三个数才说明「有没有卡一下」。
        if (g_frameTimes.samples > 0)
        {
            std::printf("[FrameTime] frames=%llu | median=%.2fms p95=%.2fms max=%.2fms\n",
                u64(g_frameTimes.samples), percentile(g_frameTimes, 0.5),
                percentile(g_frameTimes, 0.95), g_frameTimes.maxMs);
        }
    }

    bool selfTest()
    {
        // 自检会动真实的累加器，先存下来再还原 —— 否则 --selftest 跑完的
        // [FrameStats] 里会混进几条假数据。
        const Counters savedFrame = g_frame;
        const Counters savedTotal = g_total;
        const uint64_t savedFrames = g_frames;
        const VkPipeline savedGraphics = g_lastGraphics;
        const VkPipeline savedCompute = g_lastCompute;
        const FrameTimes savedTimes = g_frameTimes;

        beginFrame();

        const VkPipeline a = reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(0x1000));
        const VkPipeline b = reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(0x2000));

        countPipelineBind(a, false);
        countPipelineBind(a, false);   // 冗余
        countPipelineBind(b, false);
        countPipelineBind(b, true);    // 计算流：图形流刚绑过 b 不算冗余
        countPipelineBind(b, true);    // 冗余

        bool ok = g_frame.pipelineBinds == 5 && g_frame.redundantBinds == 2;

        countBarrierCall(3);
        ok = ok && g_frame.barrierCalls == 1 && g_frame.barrierEntries == 3;

        countPass(true);
        countPass(false);
        ok = ok && g_frame.passes == 1 && g_frame.skippedPasses == 1;

        endFrame();
        ok = ok && g_frames == 1 && g_total.barrierEntries == 3;

        // 帧首必须把上一帧的绑定与计数都清掉。
        beginFrame();
        ok = ok && g_frame.pipelineBinds == 0 && g_lastGraphics == VK_NULL_HANDLE;

        // 帧时间直方图：100 个样本 1..100 ms，中位应落在 50 ms 附近，p95 落在 95 ms 附近。
        g_frameTimes = FrameTimes{};
        for (int i = 1; i <= 100; ++i) recordFrameTime(static_cast<double>(i));
        const double median = percentile(g_frameTimes, 0.5);
        const double p95 = percentile(g_frameTimes, 0.95);
        ok = ok && g_frameTimes.samples == 100 && median >= 50.0 && median <= 51.0;
        ok = ok && p95 >= 95.0 && p95 <= 96.0;
        ok = ok && g_frameTimes.maxMs >= 100.0;

        g_frame = savedFrame;
        g_total = savedTotal;
        g_frames = savedFrames;
        g_lastGraphics = savedGraphics;
        g_lastCompute = savedCompute;
        g_frameTimes = savedTimes;
        return ok;
    }
}