#include "RHI/TransientProbe.h"
#include "RHI/FormatTable.h"

#include <cstring>

namespace tiny::transient
{
    uint32_t countLazilyAllocatedMemoryTypes(VkPhysicalDevice physicalDevice)
    {
        if (physicalDevice == VK_NULL_HANDLE)
            return 0;

        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

        uint32_t count = 0;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            if ((memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0)
                ++count;
        }
        return count;
    }

    const char* classify(bool transientUsageSupported, uint32_t lazilyAllocatedMemoryTypes)
    {
        if (!transientUsageSupported)
            return "驱动不接受 TRANSIENT_ATTACHMENT 用法";
        if (lazilyAllocatedMemoryTypes == 0)
            return "用法被接受，但本设备没有惰性分配内存类型（桌面独显常见）";
        return "可启用：TRANSIENT_ATTACHMENT + LAZILY_ALLOCATED 内存";
    }

    std::vector<Result> probe(VkPhysicalDevice physicalDevice, const std::vector<Target>& targets)
    {
        std::vector<Result> results;
        results.reserve(targets.size());

        const uint32_t lazilyAllocatedTypes = countLazilyAllocatedMemoryTypes(physicalDevice);

        for (const Target& target : targets)
        {
            Result result{};
            result.name = target.name;
            result.format = target.format;

            // 颜色目标与深度目标的用法组合不能混：transient 只能和"自己那类"附件共存。
            const VkImageUsageFlags usage =
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
                (target.depthStencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

            VkImageFormatProperties imageProperties{};
            const VkResult queryResult = vkGetPhysicalDeviceImageFormatProperties(
                physicalDevice, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                usage, 0, &imageProperties);
            result.transientUsageSupported = queryResult == VK_SUCCESS;
            result.viable = result.transientUsageSupported && lazilyAllocatedTypes > 0;
            result.verdict = classify(result.transientUsageSupported, lazilyAllocatedTypes);
            results.push_back(result);
        }
        return results;
    }

    void print(const std::vector<Result>& results, uint32_t lazilyAllocatedMemoryTypes,
               std::FILE* out)
    {
        if (out == nullptr)
            out = stdout;

        std::fprintf(out, "[Transient] 惰性分配探测（TM-18，只读，不改分配策略）\n");
        std::fprintf(out, "  本设备 LAZILY_ALLOCATED 内存类型：%u 个\n", lazilyAllocatedMemoryTypes);
        std::fprintf(out, "  %-16s %-32s %-8s %s\n", "目标", "格式", "用法", "结论");

        for (const Result& result : results)
        {
            std::fprintf(out, "  %-16s %-32s %-8s %s\n",
                         result.name,
                         tiny::formats::formatName(result.format),
                         result.transientUsageSupported ? "接受" : "拒绝",
                         result.verdict);
        }

        if (lazilyAllocatedMemoryTypes == 0)
        {
            std::fprintf(out, "  结论：本设备没有惰性分配内存类型 —— transient attachment 既不会省带宽\n");
            std::fprintf(out, "        （没有 tile memory 可省），也不会省显存（驱动退化成普通分配）。\n");
            std::fprintf(out, "        保持现有普通 device-local 中间目标；真到移动端目标再改分配器。\n");
        }
        else
        {
            std::fprintf(out, "  结论：本设备存在惰性分配内存类型，可考虑把中间目标换成 TRANSIENT_ATTACHMENT；\n");
            std::fprintf(out, "        仍需先量带宽收益，并按 TM-08 的生命周期区间确定哪些目标能安全转。\n");
        }
    }

    bool selfTest()
    {
        bool ok = true;

        // 两条判据的三种组合必须各自给出不同结论，只有都成立才 viable。
        const char* rejected = classify(false, 0);
        const char* noMemory = classify(true, 0);
        const char* ready = classify(true, 2);

        ok = ok && rejected != nullptr && noMemory != nullptr && ready != nullptr;
        ok = ok && std::strcmp(rejected, noMemory) != 0;
        ok = ok && std::strcmp(noMemory, ready) != 0;
        ok = ok && std::strcmp(rejected, ready) != 0;

        ok = ok && std::strstr(rejected, "不接受") != nullptr;
        ok = ok && std::strstr(noMemory, "没有") != nullptr;
        ok = ok && std::strstr(ready, "可启用") != nullptr;

        // 空设备：内存类型计数必须安全返回 0（探测入口会用到，别在 null 上崩）。
        ok = ok && countLazilyAllocatedMemoryTypes(VK_NULL_HANDLE) == 0;

        std::printf(ok ? "[Transient] selfTest PASS | 两条判据组合 + 空设备安全\n"
                       : "[Transient] selfTest FAIL\n");
        return ok;
    }
}
