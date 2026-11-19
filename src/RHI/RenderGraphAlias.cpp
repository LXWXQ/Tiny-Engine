#include "RHI/RenderGraphAlias.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace
{
    bool g_enabled = false;
    std::vector<tiny::aliasreport::Resource> g_lastFrame;

    // 单 texel 字节数。只覆盖渲染图实际会用到的那批格式；
    // 不认识的一律按 4 字节估并在报告里标 est，好过让数字变成 0。
    uint32_t texelBytes(VkFormat format, bool& exact)
    {
        exact = true;
        switch (format)
        {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:
            return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_D16_UNORM:
            return 2;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            exact = false;
            return 4;
        }
    }

    bool sameShape(const tiny::aliasreport::Resource& a, const tiny::aliasreport::Resource& b)
    {
        return a.format == b.format &&
               a.width == b.width && a.height == b.height &&
               a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers;
    }

    bool isCandidate(const tiny::aliasreport::Resource& r)
    {
        return r.owned && r.transient && r.bytes > 0 &&
               r.firstWrite >= 0 && r.lastUse >= r.firstWrite;
    }
}

namespace tiny::aliasreport
{
    std::string formatBytes(uint64_t bytes)
    {
        if (bytes == 0)
            return "0";
        char buffer[64];
        const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mib >= 1.0)
            std::snprintf(buffer, sizeof(buffer), "%.1f MiB", mib);
        else
            std::snprintf(buffer, sizeof(buffer), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
        return std::string(buffer);
    }

    uint64_t estimateBytes(VkFormat format, uint32_t width, uint32_t height,
                           uint32_t mipLevels, uint32_t arrayLayers)
    {
        bool exact = true;
        const uint32_t perTexel = texelBytes(format, exact);
        uint64_t total = 0;
        uint32_t w = width == 0 ? 1 : width;
        uint32_t h = height == 0 ? 1 : height;
        // mip 链：每级各算一次，按 1 级下限兜住 clamp 到 0 的情况。
        const uint32_t levels = mipLevels == 0 ? 1 : mipLevels;
        for (uint32_t level = 0; level < levels; ++level)
        {
            total += static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * perTexel;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return total * (arrayLayers == 0 ? 1 : arrayLayers);
    }

    void setEnabled(bool on) { g_enabled = on; }
    bool enabled() { return g_enabled; }

    void recordFrame(std::vector<Resource> resources)
    {
        if (!g_enabled)
            return;
        g_lastFrame = std::move(resources);
    }

    std::vector<Slot> pack(const std::vector<Resource>& resources)
    {
        // 按 firstWrite 升序处理：这样「块什么时候空出来」只需要看已放入成员的 end。
        std::vector<const Resource*> order;
        order.reserve(resources.size());
        for (const Resource& r : resources)
            if (isCandidate(r))
                order.push_back(&r);
        std::stable_sort(order.begin(), order.end(), [](const Resource* a, const Resource* b) {
            if (a->firstWrite != b->firstWrite)
                return a->firstWrite < b->firstWrite;
            return a->bytes > b->bytes;
        });

        std::vector<Slot> slots;
        for (const Resource* r : order)
        {
            // best fit：在所有「已经空了」且够大、形状兼容的块里挑最小的那个，
            // 把大块留给后面的大资源。
            Slot* best = nullptr;
            for (Slot& slot : slots)
            {
                if (slot.end >= r->firstWrite)
                    continue;                       // 生命周期重叠
                if (slot.capacity < r->bytes)
                    continue;                       // 装不下
                if (!sameShape(*slot.members.front(), *r))
                    continue;                       // 形状不同，不能落在一起
                if (best == nullptr || slot.capacity < best->capacity)
                    best = &slot;
            }

            if (best == nullptr)
            {
                Slot slot;
                slot.capacity = r->bytes;
                slot.end = r->lastUse;
                slot.members.push_back(r);
                slots.push_back(std::move(slot));
            }
            else
            {
                best->end = std::max(best->end, r->lastUse);
                if (r->bytes > best->capacity)
                    best->capacity = r->bytes;
                best->members.push_back(r);
            }
        }
        return slots;
    }

    void report()
    {
        if (g_lastFrame.empty())
            return;

        const std::vector<Slot> slots = pack(g_lastFrame);

        uint64_t logicalBytes = 0;
        size_t candidates = 0;
        for (const Resource& r : g_lastFrame)
        {
            if (!isCandidate(r))
                continue;
            logicalBytes += r.bytes;
            ++candidates;
        }
        if (candidates == 0)
            return;

        uint64_t physicalBytes = 0;
        uint64_t biggest = 0;
        for (const Slot& slot : slots)
        {
            physicalBytes += slot.capacity;
            biggest = std::max(biggest, slot.capacity);
        }

        // 导入目标分两类：每帧重建的（有别名价值，但显存不归渲染图）和常驻的
        // （IBL / shadow / LUT，本来就不该复用）。分开报，免得 400 MiB 这个数字骗人。
        uint64_t importedTransientBytes = 0;
        size_t importedTransient = 0;
        uint64_t importedPersistentBytes = 0;
        size_t importedPersistent = 0;
        for (const Resource& r : g_lastFrame)
        {
            if (r.owned || r.bytes == 0)
                continue;
            if (r.transient)
            {
                importedTransientBytes += r.bytes;
                ++importedTransient;
            }
            else
            {
                importedPersistentBytes += r.bytes;
                ++importedPersistent;
            }
        }

        std::cout << "[Alias] 一帧 " << candidates << " 个瞬时目标，合计 " << formatBytes(logicalBytes) << "\n";
        std::cout << "[Alias] 线性扫描后 " << slots.size() << " 块（最大 " << formatBytes(biggest) << "），"
                  << "可省 " << formatBytes(logicalBytes > physicalBytes ? logicalBytes - physicalBytes : 0)
                  << "（" << (logicalBytes > 0 ? (100.0 * static_cast<double>(logicalBytes - physicalBytes) / static_cast<double>(logicalBytes)) : 0.0)
                  << "%）\n";

        // 只列前 3 组合并，否则一屏都是这一类行。
        int shown = 0;
        for (const Slot& slot : slots)
        {
            if (slot.members.size() < 2)
                continue;
            if (shown >= 3)
                break;
            const Resource& host = *slot.members.front();
            for (size_t i = 1; i < slot.members.size(); ++i)
            {
                const Resource& guest = *slot.members[i];
                std::cout << "[Alias]   " << guest.name << "（Pass " << guest.firstWrite << ".." << guest.lastUse
                          << "，" << formatBytes(guest.bytes) << "）可落在 " << host.name
                          << "（Pass " << host.firstWrite << ".." << host.lastUse << "）的显存上\n";
            }
            ++shown;
        }

        size_t merged = 0;
        for (const Slot& slot : slots)
            if (slot.members.size() >= 2)
                merged += slot.members.size() - 1;
        if (merged > 0)
            std::cout << "[Alias] 共 " << merged << " 个目标可以复用别人的显存\n";

        // 明细：8~12 个目标时全列出来，比只报汇总有用 —— 能直接看出谁压着谁。
        if (candidates <= 12)
        {
            for (const Resource& r : g_lastFrame)
            {
                if (!isCandidate(r))
                    continue;
                std::cout << "[Alias]   " << r.name << " [Pass " << r.firstWrite << ".." << r.lastUse
                          << "] " << formatBytes(r.bytes) << "\n";
            }
        }

        if (importedTransient > 0)
        {
            std::cout << "[Alias] 另有 " << importedTransient << " 个每帧重建的导入目标（合计 "
                      << formatBytes(importedTransientBytes)
                      << "）：它们才是有价值的目标，但显存不归渲染图管\n";
        }
        if (importedPersistent > 0)
        {
            std::cout << "[Alias] 常驻导入目标 " << importedPersistent << " 个（合计 "
                      << formatBytes(importedPersistentBytes)
                      << "，跨帧复用同一块，不该参与别名）\n";
        }

        std::cout << "[Alias] 说明：本报告只读，不改分配行为；真要复用还得接 "
                     "VK_IMAGE_CREATE_ALIAS_BIT 并逐对做像素校验\n";
    }

    bool selfTest()
    {
        int failures = 0;
        const auto check = [&failures](bool ok, const char* what) {
            if (!ok)
            {
                std::cout << "[Alias] selfTest FAIL: " << what << "\n";
                ++failures;
            }
        };

        // 1) 字节估算：64x64 RGBA8 一级 = 16384，两级 = 16384 + 4096
        check(estimateBytes(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1) == 16384, "estimate mip1");
        check(estimateBytes(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 2, 1) == 16384 + 4096, "estimate mip2");
        check(estimateBytes(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 6) == 16384 * 6, "estimate layers");
        check(formatBytes(1024 * 1024) == "1.0 MiB", "formatBytes MiB");
        check(formatBytes(0) == "0", "formatBytes zero");

        const auto make = [](const char* name, int32_t first, int32_t last, VkFormat format, uint32_t size) {
            Resource r;
            r.name = name;
            r.firstWrite = first;
            r.lastUse = last;
            r.bytes = static_cast<uint64_t>(size) * size * 4;
            r.format = format;
            r.width = size;
            r.height = size;
            return r;
        };

        // 2) 生命周期不重叠 -> 一块
        {
            std::vector<Resource> rs{
                make("a", 1, 3, VK_FORMAT_R8G8B8A8_UNORM, 64),
                make("b", 4, 6, VK_FORMAT_R8G8B8A8_UNORM, 64) };
            const std::vector<Slot> slots = pack(rs);
            check(slots.size() == 1, "disjoint should share one slot");
            check(slots.size() == 1 && slots[0].members.size() == 2, "disjoint members");
        }

        // 3) 生命周期重叠 -> 两块
        {
            std::vector<Resource> rs{
                make("a", 1, 5, VK_FORMAT_R8G8B8A8_UNORM, 64),
                make("b", 4, 7, VK_FORMAT_R8G8B8A8_UNORM, 64) };
            check(pack(rs).size() == 2, "overlap must not share");
        }

        // 4) 首尾相接（a 用到 3，b 从 4 开始）算不重叠
        {
            std::vector<Resource> rs{
                make("a", 1, 3, VK_FORMAT_R8G8B8A8_UNORM, 64),
                make("b", 4, 4, VK_FORMAT_R8G8B8A8_UNORM, 64) };
            check(pack(rs).size() == 1, "adjacent intervals may share");
        }

        // 5) 格式不同 -> 不合并
        {
            std::vector<Resource> rs{
                make("a", 1, 3, VK_FORMAT_R8G8B8A8_UNORM, 64),
                make("b", 4, 6, VK_FORMAT_R16G16B16A16_SFLOAT, 64) };
            check(pack(rs).size() == 2, "different format must not share");
        }

        // 6) 尺寸不同 -> 不合并（保守规则）
        {
            std::vector<Resource> rs{
                make("a", 1, 3, VK_FORMAT_R8G8B8A8_UNORM, 64),
                make("b", 4, 6, VK_FORMAT_R8G8B8A8_UNORM, 32) };
            check(pack(rs).size() == 2, "different extent must not share");
        }

        // 7) 导入的资源（owned=false）不参与：显存不是渲染图分配的
        {
            Resource r = make("imported", 1, 3, VK_FORMAT_R8G8B8A8_UNORM, 64);
            r.owned = false;
            std::vector<Resource> rs{ r };
            check(pack(rs).empty(), "imported resource excluded");
        }

        // 7) 没写过的资源不参与（firstWrite < 0）
        {
            Resource r = make("never-written", -1, 5, VK_FORMAT_R8G8B8A8_UNORM, 64);
            std::vector<Resource> rs{ r };
            check(pack(rs).empty(), "unwritten resource excluded");
        }

        if (failures == 0)
            std::cout << "[Alias] selfTest PASS | 生命周期装箱 + 形状兼容 + 字节估算\n";
        return failures == 0;
    }
}
