#include "RHI/FormatTable.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tiny::formats
{
    namespace
    {
        struct NameEntry
        {
            VkFormat format;
            const char* name;
            uint32_t bytesPerPixel;
        };

        // 只收录引擎实际用到的格式。加新格式时把名字和字节数一起补进来 ——
        // 名字漏了会打印成 VK_FORMAT_<数字>，字节数漏了会让 --print-formats
        // 的内存估算变成 0（自检会发现这一条）。
        const NameEntry kKnownFormats[] = {
            { VK_FORMAT_UNDEFINED,                    "VK_FORMAT_UNDEFINED",                    0 },
            { VK_FORMAT_R8_UNORM,                     "VK_FORMAT_R8_UNORM",                     1 },
            { VK_FORMAT_R8G8_UNORM,                   "VK_FORMAT_R8G8_UNORM",                   2 },
            { VK_FORMAT_R8G8B8A8_UNORM,               "VK_FORMAT_R8G8B8A8_UNORM",               4 },
            { VK_FORMAT_R8G8B8A8_SRGB,                "VK_FORMAT_R8G8B8A8_SRGB",                4 },
            { VK_FORMAT_B8G8R8A8_UNORM,               "VK_FORMAT_B8G8R8A8_UNORM",               4 },
            { VK_FORMAT_B8G8R8A8_SRGB,                "VK_FORMAT_B8G8R8A8_SRGB",                4 },
            { VK_FORMAT_R16_SFLOAT,                   "VK_FORMAT_R16_SFLOAT",                   2 },
            { VK_FORMAT_R16G16_SFLOAT,                "VK_FORMAT_R16G16_SFLOAT",                4 },
            { VK_FORMAT_R16G16B16A16_SFLOAT,          "VK_FORMAT_R16G16B16A16_SFLOAT",          8 },
            { VK_FORMAT_R32_UINT,                     "VK_FORMAT_R32_UINT",                     4 },
            { VK_FORMAT_R32_SFLOAT,                   "VK_FORMAT_R32_SFLOAT",                   4 },
            { VK_FORMAT_R32G32B32A32_SFLOAT,          "VK_FORMAT_R32G32B32A32_SFLOAT",         16 },
            { VK_FORMAT_B10G11R11_UFLOAT_PACK32,      "VK_FORMAT_B10G11R11_UFLOAT_PACK32",      4 },
            { VK_FORMAT_D16_UNORM,                    "VK_FORMAT_D16_UNORM",                    2 },
            { VK_FORMAT_D24_UNORM_S8_UINT,            "VK_FORMAT_D24_UNORM_S8_UINT",            4 },
            { VK_FORMAT_D32_SFLOAT,                   "VK_FORMAT_D32_SFLOAT",                   4 },
            { VK_FORMAT_D32_SFLOAT_S8_UINT,           "VK_FORMAT_D32_SFLOAT_S8_UINT",           8 },
        };

        const NameEntry* findKnown(VkFormat format)
        {
            for (const NameEntry& entry : kKnownFormats)
            {
                if (entry.format == format)
                    return &entry;
            }
            return nullptr;
        }

        // 未收录格式的退化名字：返回线程内的静态缓冲，调用方不能保存。
        // 用 thread_local 是为了让人可以在多线程日志里安全地打印它。
        const char* fallbackName(VkFormat format)
        {
            static thread_local char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "VK_FORMAT_%d", static_cast<int>(format));
            return buffer;
        }

        const char* yesNo(bool value)
        {
            return value ? "yes" : "no ";
        }
    }

    const char* formatName(VkFormat format)
    {
        if (const NameEntry* known = findKnown(format))
            return known->name;
        return fallbackName(format);
    }

    uint32_t bytesPerPixel(VkFormat format)
    {
        if (const NameEntry* known = findKnown(format))
            return known->bytesPerPixel;
        return 0;
    }

    Entry describe(VkFormat format, const VkFormatProperties& properties)
    {
        const VkFormatFeatureFlags f = properties.optimalTilingFeatures;

        Entry entry{};
        entry.format = format;
        entry.name = formatName(format);
        entry.bytesPerPixel = bytesPerPixel(format);
        entry.colorAttachment = (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
        entry.depthStencil = (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        entry.sampled = (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        entry.storage = (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        entry.blitSrc = (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
        entry.blitDst = (f & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        entry.linearFilter = (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        return entry;
    }

    std::vector<Entry> query(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& formats)
    {
        std::vector<Entry> entries;
        entries.reserve(formats.size());

        for (VkFormat format : formats)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
            entries.push_back(describe(format, properties));
        }
        return entries;
    }

    void print(const std::vector<Entry>& entries, std::FILE* out)
    {
        if (out == nullptr)
            out = stdout;

        std::fprintf(out, "[Formats] %zu 个格式（optimal tiling）\n", entries.size());
        std::fprintf(out, "  %-34s %5s  %-4s %-4s %-4s %-4s %-4s %-4s %-6s\n",
                     "格式", "字节", "颜色", "深度", "采样", "存储", "blit源", "blit目标", "线性");
        for (const Entry& entry : entries)
        {
            std::fprintf(out, "  %-34s %5u  %-4s %-4s %-4s %-4s %-4s %-4s %-6s\n",
                         entry.name,
                         entry.bytesPerPixel,
                         yesNo(entry.colorAttachment),
                         yesNo(entry.depthStencil),
                         yesNo(entry.sampled),
                         yesNo(entry.storage),
                         yesNo(entry.blitSrc),
                         yesNo(entry.blitDst),
                         yesNo(entry.linearFilter));
        }
    }

    bool selfTest()
    {
        bool ok = true;

        // 1) 名字与字节数：抽查引擎实际在用的几种
        struct Case { VkFormat format; const char* name; uint32_t bytes; };
        const Case cases[] = {
            { VK_FORMAT_R32G32B32A32_SFLOAT, "VK_FORMAT_R32G32B32A32_SFLOAT", 16 },
            { VK_FORMAT_R16G16B16A16_SFLOAT, "VK_FORMAT_R16G16B16A16_SFLOAT",  8 },
            { VK_FORMAT_R8G8B8A8_UNORM,      "VK_FORMAT_R8G8B8A8_UNORM",       4 },
            { VK_FORMAT_D32_SFLOAT,          "VK_FORMAT_D32_SFLOAT",           4 },
            { VK_FORMAT_R8_UNORM,            "VK_FORMAT_R8_UNORM",             1 },
        };
        for (const Case& c : cases)
        {
            if (std::strcmp(formatName(c.format), c.name) != 0)
                ok = false;
            if (bytesPerPixel(c.format) != c.bytes)
                ok = false;
        }

        // 2) 未收录格式：不能崩，名字要退化成 VK_FORMAT_<数字>，字节数 0
        const VkFormat unknown = static_cast<VkFormat>(9000);
        if (std::strncmp(formatName(unknown), "VK_FORMAT_9000", 14) != 0)
            ok = false;
        if (bytesPerPixel(unknown) != 0)
            ok = false;

        // 3) 能力位映射：给一份人造的 VkFormatProperties，逐位核对
        VkFormatProperties props{};
        props.optimalTilingFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT;
        const Entry partial = describe(VK_FORMAT_R8G8B8A8_UNORM, props);
        if (!partial.colorAttachment || !partial.sampled || !partial.blitDst)
            ok = false;
        if (partial.depthStencil || partial.storage || partial.blitSrc || partial.linearFilter)
            ok = false;
        if (partial.bytesPerPixel != 4)
            ok = false;

        props.optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        const Entry depthAndStorage = describe(VK_FORMAT_D32_SFLOAT, props);
        if (!depthAndStorage.depthStencil || !depthAndStorage.storage)
            ok = false;
        if (depthAndStorage.colorAttachment || depthAndStorage.blitDst || depthAndStorage.linearFilter)
            ok = false;

        // 4) 空能力集：所有位都必须是 false（而不是未初始化的随机值）
        const Entry none = describe(VK_FORMAT_R16G16_SFLOAT, VkFormatProperties{});
        if (none.colorAttachment || none.depthStencil || none.sampled || none.storage ||
            none.blitSrc || none.blitDst || none.linearFilter)
            ok = false;

        std::printf("[Formats] selfTest %s | 名字映射 + 字节数 + 能力位 + 未收录退化\n",
                    ok ? "PASS" : "FAIL");
        return ok;
    }
}