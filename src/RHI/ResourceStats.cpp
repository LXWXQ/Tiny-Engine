#include "ResourceStats.h"

#include <cstdio>

namespace tiny::resourcestats
{
    namespace
    {
        constexpr uint32_t kMaxHeaps = 8;

        struct HeapSample
        {
            bool valid = false;
            VkDeviceSize usageFirst = 0;
            VkDeviceSize usageLast = 0;
            VkDeviceSize usagePeak = 0;
            uint64_t allocFirst = 0;
            uint64_t allocLast = 0;
            uint64_t allocPeak = 0;
        };

        uint64_t g_imagesTotal = 0;
        uint64_t g_buffersTotal = 0;
        uint64_t g_imagesFrame = 0;
        uint64_t g_buffersFrame = 0;
        uint64_t g_peakImagesInFrame = 0;
        uint64_t g_peakBuffersInFrame = 0;
        uint64_t g_frames = 0;

        VmaAllocator g_allocator = nullptr;
        HeapSample g_heaps[kMaxHeaps];
        uint32_t g_heapCount = 0;

        unsigned long long u64(uint64_t value)
        {
            return static_cast<unsigned long long>(value);
        }

        double mib(VkDeviceSize bytes)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        }

        void sampleMemory()
        {
            if (g_allocator == nullptr) return;

            const VkPhysicalDeviceMemoryProperties* props = nullptr;
            vmaGetMemoryProperties(g_allocator, &props);
            if (props == nullptr) return;

            VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
            vmaGetHeapBudgets(g_allocator, budgets);

            uint32_t count = props->memoryHeapCount;
            if (count > kMaxHeaps) count = kMaxHeaps;
            g_heapCount = count;

            for (uint32_t i = 0; i < count; ++i)
            {
                HeapSample& sample = g_heaps[i];
                const VkDeviceSize usage = budgets[i].usage;
                const uint64_t allocs = budgets[i].statistics.allocationCount;

                if (!sample.valid)
                {
                    sample.valid = true;
                    sample.usageFirst = usage;
                    sample.allocFirst = allocs;
                }
                sample.usageLast = usage;
                sample.allocLast = allocs;
                if (usage > sample.usagePeak) sample.usagePeak = usage;
                if (allocs > sample.allocPeak) sample.allocPeak = allocs;
            }
        }
    }

    void beginFrame()
    {
        // 这里故意不清本帧计数：资源可能在 beginFrame 之前就创建好（启动期的 G-Buffer、
        // 渲染图自持目标），清掉会让它们既不算进总计、也不出现在任何一帧里。
        // 清零放在 endFrame 累加之后 —— 于是「第 1 帧」自然带上启动期的创建量。
    }

    void endFrame()
    {
        g_imagesTotal += g_imagesFrame;
        g_buffersTotal += g_buffersFrame;
        if (g_imagesFrame > g_peakImagesInFrame) g_peakImagesInFrame = g_imagesFrame;
        if (g_buffersFrame > g_peakBuffersInFrame) g_peakBuffersInFrame = g_buffersFrame;
        g_imagesFrame = 0;
        g_buffersFrame = 0;
        ++g_frames;

        // 10 Hz：够看出显存斜率，又不会把 vmaGetHeapBudgets 的遍历成本摊进每帧。
        if ((g_frames % 60u) == 1u) sampleMemory();
    }

    void countImage() { ++g_imagesFrame; }
    void countBuffer() { ++g_buffersFrame; }

    void setAllocator(VmaAllocator allocator)
    {
        g_allocator = allocator;
        for (uint32_t i = 0; i < kMaxHeaps; ++i) g_heaps[i] = HeapSample{};
        g_heapCount = 0;
    }

    void report()
    {
        // 退出前补一次采样：短跑（selftest 60 帧）只会在第 1 帧采到一次，
        // 那次往往还在加载期，first 与 last 差得太远会误读成在增长。
        sampleMemory();

        if (g_frames == 0)
        {
            std::printf("[Resources] 没有采集到帧（渲染图没跑过）\n");
            return;
        }

        const double frames = static_cast<double>(g_frames);
        std::printf("[Resources] frames=%llu | images/frame=%.2f total=%llu peakFrame=%llu | buffers/frame=%.2f total=%llu peakFrame=%llu\n",
            u64(g_frames),
            static_cast<double>(g_imagesTotal) / frames, u64(g_imagesTotal), u64(g_peakImagesInFrame),
            static_cast<double>(g_buffersTotal) / frames, u64(g_buffersTotal), u64(g_peakBuffersInFrame));

        // first / last / peak 三列是 T-04 的判据：稳定态 last 应当贴着 first 与 peak，
        // 一路往上爬说明每帧还在拿新显存。
        for (uint32_t i = 0; i < g_heapCount; ++i)
        {
            const HeapSample& sample = g_heaps[i];
            if (!sample.valid || sample.usagePeak == 0) continue;
            std::printf("[Memory] heap%u used first=%.1f last=%.1f peak=%.1f MiB | allocs first=%llu last=%llu peak=%llu\n",
                i, mib(sample.usageFirst), mib(sample.usageLast), mib(sample.usagePeak),
                u64(sample.allocFirst), u64(sample.allocLast), u64(sample.allocPeak));
        }
    }

    bool selfTest()
    {
        // 自检会动真实的累加器，先存下来再还原。
        const uint64_t savedImagesTotal = g_imagesTotal;
        const uint64_t savedBuffersTotal = g_buffersTotal;
        const uint64_t savedImagesFrame = g_imagesFrame;
        const uint64_t savedBuffersFrame = g_buffersFrame;
        const uint64_t savedPeakImages = g_peakImagesInFrame;
        const uint64_t savedPeakBuffers = g_peakBuffersInFrame;
        const uint64_t savedFrames = g_frames;
        VmaAllocator savedAllocator = g_allocator;
        HeapSample savedHeaps[kMaxHeaps];
        for (uint32_t i = 0; i < kMaxHeaps; ++i) savedHeaps[i] = g_heaps[i];
        const uint32_t savedHeapCount = g_heapCount;

        // 用干净的计数验语义：beginFrame 不清（帧外创建也要算），endFrame 累加后清零。
        g_imagesFrame = 0;
        g_buffersFrame = 0;
        beginFrame();
        countImage();
        countImage();
        countBuffer();
        bool ok = g_imagesFrame == 2 && g_buffersFrame == 1;

        endFrame();
        ok = ok && g_frames == savedFrames + 1;
        ok = ok && g_imagesTotal == savedImagesTotal + 2 && g_buffersTotal == savedBuffersTotal + 1;
        ok = ok && g_imagesFrame == 0 && g_buffersFrame == 0;
        ok = ok && g_peakImagesInFrame >= 2;

        // 没有分配器时那一次采样必须是空操作，不能崩。
        setAllocator(nullptr);
        endFrame();
        ok = ok && g_heapCount == 0;

        g_imagesTotal = savedImagesTotal;
        g_buffersTotal = savedBuffersTotal;
        g_imagesFrame = savedImagesFrame;
        g_buffersFrame = savedBuffersFrame;
        g_peakImagesInFrame = savedPeakImages;
        g_peakBuffersInFrame = savedPeakBuffers;
        g_frames = savedFrames;
        g_allocator = savedAllocator;
        for (uint32_t i = 0; i < kMaxHeaps; ++i) g_heaps[i] = savedHeaps[i];
        g_heapCount = savedHeapCount;
        return ok;
    }
}