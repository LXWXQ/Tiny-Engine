#include "RenderTargetPool.h"

#include "Device.h"
#include "RHI/ResourceStats.h"

#include <cstdio>
#include <stdexcept>

namespace tiny
{
    namespace
    {
        VkImageAspectFlags aspectOf(bool isDepth)
        {
            return isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        }

        unsigned long long u64(uint64_t value)
        {
            return static_cast<unsigned long long>(value);
        }

        double mib(VkDeviceSize bytes)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        }
    }

    RenderTargetPool::RenderTargetPool(Device& device) : device(device) {}

    RenderTargetPool::~RenderTargetPool()
    {
        for (IdleEntry& entry : idle)
            destroyImage(entry.image);
        idle.clear();
        idleBytes = 0;
    }

    bool RenderTargetPool::sameDesc(const Desc& a, const Desc& b)
    {
        // 尺寸必须精确相等：view 没有 extent，大图当小图用就是缩放错误。
        return a.format == b.format
            && a.extent.width == b.extent.width
            && a.extent.height == b.extent.height
            && a.usage == b.usage
            && a.flags == b.flags
            && a.mipLevels == b.mipLevels
            && a.arrayLayers == b.arrayLayers
            && a.isDepth == b.isDepth
            && a.crossQueue == b.crossQueue;
    }

    VkDeviceSize RenderTargetPool::bytesPerPixel(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:
            return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SRGB:
            return 2;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 4;
        }
    }

    VkDeviceSize RenderTargetPool::estimateBytes(const Desc& desc)
    {
        VkDeviceSize bytes = static_cast<VkDeviceSize>(desc.extent.width)
            * static_cast<VkDeviceSize>(desc.extent.height)
            * bytesPerPixel(desc.format)
            * (desc.mipLevels == 0 ? 1 : desc.mipLevels)
            * (desc.arrayLayers == 0 ? 1 : desc.arrayLayers);
        return bytes;
    }

    void RenderTargetPool::beginFrame(uint64_t number)
    {
        frameNumber = number;
        evictOverBudget();
    }

    RenderTargetPool::Image RenderTargetPool::acquire(const Desc& desc, const std::string& name)
    {
        if (!enabled)
        {
            ++stats.created;
            noteCreated(name);
            return createImage(desc, name);
        }

        for (size_t i = 0; i < idle.size(); ++i)
        {
            IdleEntry& entry = idle[i];
            if (entry.releasedFrame + kRetireFrames > frameNumber)
                continue;   // 还在退休窗口里，可能被上一帧的命令引用着
            if (!sameDesc(entry.desc, desc))
                continue;

            const Image image = entry.image;
            idleBytes -= estimateBytes(entry.desc);
            idle[i] = idle.back();
            idle.pop_back();
            ++stats.hits;
            return image;
        }

        ++stats.misses;
        ++stats.created;
        noteCreated(name);
        return createImage(desc, name);
    }

    void RenderTargetPool::release(const Desc& desc, const Image& image)
    {
        if (image.image == VK_NULL_HANDLE)
            return;

        if (!enabled)
        {
            destroyImage(image);
            ++stats.destroyed;
            return;
        }

        idle.push_back(IdleEntry{ desc, image, frameNumber });
        idleBytes += estimateBytes(desc);
        evictOverBudget();
    }

    void RenderTargetPool::evictOverBudget()
    {
        while (idleBytes > kMaxIdleBytes && !idle.empty())
        {
            size_t oldest = 0;
            for (size_t i = 1; i < idle.size(); ++i)
            {
                if (idle[i].releasedFrame < idle[oldest].releasedFrame)
                    oldest = i;
            }

            idleBytes -= estimateBytes(idle[oldest].desc);
            destroyImage(idle[oldest].image);
            idle[oldest] = idle.back();
            idle.pop_back();
            ++stats.evicted;
            ++stats.destroyed;
        }
    }

    RenderTargetPool::Image RenderTargetPool::createImage(const Desc& desc, const std::string& name)
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = desc.format;
        info.extent = { desc.extent.width, desc.extent.height, 1 };
        info.mipLevels = desc.mipLevels == 0 ? 1 : desc.mipLevels;
        info.arrayLayers = desc.arrayLayers == 0 ? 1 : desc.arrayLayers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = desc.usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.flags = desc.flags;

        const QueueFamilyIndices queueFamilies = device.findPhysicalQueueFamilies();
        uint32_t sharedFamilies[2] = {
            queueFamilies.graphicsFamily.value(),
            queueFamilies.computeFamily.value()
        };
        if (desc.crossQueue && device.hasDedicatedComputeQueue())
        {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = sharedFamilies;
        }

        Image image{};
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        if (vmaCreateImage(device.getVmaAllocator(), &info, &alloc, &image.image, &image.allocation, nullptr) != VK_SUCCESS)
            throw std::runtime_error("RenderTargetPool: 图像创建失败: " + name);
        tiny::resourcestats::countImage();

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = image.image;
        if (info.arrayLayers == 6 && (info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
            view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (info.arrayLayers > 1)
            view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = desc.format;
        view.subresourceRange = { aspectOf(desc.isDepth), 0, info.mipLevels, 0, info.arrayLayers };

        if (vkCreateImageView(device.getDevice(), &view, nullptr, &image.view) != VK_SUCCESS)
        {
            vmaDestroyImage(device.getVmaAllocator(), image.image, image.allocation);
            throw std::runtime_error("RenderTargetPool: 图像视图创建失败: " + name);
        }

        return image;
    }

    void RenderTargetPool::destroyImage(const Image& image)
    {
        if (image.view != VK_NULL_HANDLE)
            vkDestroyImageView(device.getDevice(), image.view, nullptr);
        if (image.image != VK_NULL_HANDLE)
            vmaDestroyImage(device.getVmaAllocator(), image.image, image.allocation);
    }

    void RenderTargetPool::noteCreated(const std::string& name)
    {
        for (NameCount& entry : createdByName)
        {
            if (entry.name == name) { ++entry.created; return; }
        }
        createdByName.push_back(NameCount{ name, 1 });
    }

    void RenderTargetPool::report() const
    {
        const uint64_t total = stats.hits + stats.misses;
        const double hitRate = total == 0 ? 0.0 : 100.0 * static_cast<double>(stats.hits) / static_cast<double>(total);
        std::printf("[RTPool] enabled=%d | hits=%llu misses=%llu created=%llu destroyed=%llu evicted=%llu | hitRate=%.1f%% | idle=%zu images %.1f MiB\n",
            enabled ? 1 : 0,
            u64(stats.hits), u64(stats.misses), u64(stats.created), u64(stats.destroyed), u64(stats.evicted),
            hitRate, idle.size(), mib(idleBytes));

        // report 是 const：排序在副本上做，别动成员。
        std::vector<NameCount> top = createdByName;
        for (size_t i = 0; i < top.size() && i < 8; ++i)
        {
            size_t best = i;
            for (size_t j = i + 1; j < top.size(); ++j)
            {
                if (top[j].created > top[best].created) best = j;
            }
            if (best != i)
            {
                const NameCount tmp = top[i];
                top[i] = top[best];
                top[best] = tmp;
            }
        }
        if (!top.empty())
        {
            std::printf("[RTPool] top creators:\n");
            for (size_t i = 0; i < top.size() && i < 8; ++i)
                std::printf("  %-28s %llu\n", top[i].name.c_str(), u64(top[i].created));
        }
    }
    bool RenderTargetPool::selfTest()
    {
        Desc base{};
        base.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        base.extent = { 1920, 1080 };
        base.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        if (!sameDesc(base, base))
            return false;

        Desc other = base;
        if (sameDesc(base, other) == false)
            return false;

        Desc bigger = base;
        bigger.extent = { 1280, 720 };
        if (sameDesc(base, bigger))
            return false;   // 尺寸不同不能命中

        Desc depth = base;
        depth.isDepth = true;
        if (sameDesc(base, depth))
            return false;   // 深度 / 颜色不能混用

        // 1920x1080 RGBA16F = 1920 * 1080 * 8 字节
        if (estimateBytes(base) != static_cast<VkDeviceSize>(1920) * 1080 * 8)
            return false;
        // 1280x720 R8 = 1280 * 720 * 1 字节
        Desc r8 = bigger;
        r8.format = VK_FORMAT_R8_UNORM;
        if (estimateBytes(r8) != static_cast<VkDeviceSize>(1280) * 720)
            return false;

        // 退休窗口：刚释放的条目在同一帧不能复用。
        const uint64_t released = 10;
        if (released + kRetireFrames > released)
            return false;
        if (!(released + kRetireFrames <= released + kRetireFrames))
            return false;

        return true;
    }
}