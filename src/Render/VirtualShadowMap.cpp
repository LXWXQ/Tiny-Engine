#include "Render/VirtualShadowMap.h"

#include "Render/SparsePageAllocator.h"
#include "RHI/ResourceStats.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
    const char* formatName(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_D32_SFLOAT: return "D32";
        case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
        default: return "?";
        }
    }

    double mib(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
}

VirtualShadowMap::VirtualShadowMap(Device& device)
    : device(device)
{
    stats.pageCount = kPageCount;
    stats.pageSize = kPageSize;
    stats.atlasResolution = kAtlasResolution;
    stats.clipmapLevels = kClipmapLevels;
    stats.pageTableEntries = kPageTableEntries;
    stats.pageTableUsed = kClipmapLevels * kPageTableEntries;
    stats.pageTableSpare = kPageCount - stats.pageTableUsed;
    stats.hzbLevels = kHzbLevels;
    uint32_t rowUsage = 0;
    for (uint32_t level = 1; level <= kHzbLevels; ++level)
        rowUsage += hzbTileSize(level);
    stats.hzbRowUsage = rowUsage;

    stats.depthBytes = static_cast<uint64_t>(kAtlasResolution) * kAtlasResolution * 4ull;
    // HZB 图集与深度图集同尺寸；RG16F 是 min/max 各一半，4 字节/texel。
    stats.hzbBytes = static_cast<uint64_t>(kAtlasResolution) * kAtlasResolution * 4ull;
    pageTableStride = static_cast<uint64_t>(kClipmapLevels) * kPageTableEntries * sizeof(uint32_t);
    stats.pageTableBytes = pageTableStride * kFramesInFlight;
    stats.totalBytes = stats.depthBytes + stats.hzbBytes + stats.pageTableBytes;
}

VirtualShadowMap::~VirtualShadowMap()
{
    destroy();
}

void VirtualShadowMap::init(bool allocateResources)
{
    if (initialized)
        return;

    allocator = std::make_unique<tiny::SparsePageAllocator>(kPageCount);
    initialized = true;

    if (!allocateResources)
        return;

    createAtlas();
    createPageTables();
    resetPageTable();
}

void VirtualShadowMap::destroy()
{
    if (!initialized)
        return;
    destroyResources();
    allocator.reset();
    initialized = false;
}

void VirtualShadowMap::createAtlas()
{
    if (depthAtlas != VK_NULL_HANDLE)
        return;

    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = { kAtlasResolution, kAtlasResolution, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // P4-3 起它既要当深度附件、又要被 HZB/页表路径采样，所以两种 usage 一起要。
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(device.getVmaAllocator(), &imageInfo, &allocInfo,
                       &depthAtlas, &depthAllocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("VirtualShadowMap: 深度图集创建失败");
    tiny::resourcestats::countImage();

    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = depthAtlas;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &depthView) != VK_SUCCESS)
        throw std::runtime_error("VirtualShadowMap: 深度图集视图创建失败");

    VkImageCreateInfo hzbInfo = imageInfo;
    hzbInfo.format = VK_FORMAT_R16G16_SFLOAT;
    // 不留 mip：七级金字塔横排在每页瓦片里，见类头注释。
    hzbInfo.mipLevels = 1;
    hzbInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateImage(device.getVmaAllocator(), &hzbInfo, &allocInfo,
                       &hzbAtlas, &hzbAllocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("VirtualShadowMap: HZB 图集创建失败");
    tiny::resourcestats::countImage();

    VkImageViewCreateInfo hzbViewInfo = viewInfo;
    hzbViewInfo.image = hzbAtlas;
    hzbViewInfo.format = VK_FORMAT_R16G16_SFLOAT;
    hzbViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device.getDevice(), &hzbViewInfo, nullptr, &hzbView) != VK_SUCCESS)
        throw std::runtime_error("VirtualShadowMap: HZB 图集视图创建失败");

    std::cout << "[VSM] atlas ready | depth=" << formatName(VK_FORMAT_D32_SFLOAT)
              << " hzb=" << formatName(VK_FORMAT_R16G16_SFLOAT)
              << " resolution=" << kAtlasResolution << "x" << kAtlasResolution
              << " pages=" << kPageCount << " pageSize=" << kPageSize << std::endl;
}

void VirtualShadowMap::createPageTables()
{
    if (pageTableBuffers[0] != VK_NULL_HANDLE)
        return;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = pageTableStride;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        // 页表每帧由 CPU 写（P4-3 的 clipmap 滚动会逐项改），所以就要 host visible。
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &pageTableBuffers[frame], &pageTableAllocations[frame], &info) != VK_SUCCESS)
            throw std::runtime_error("VirtualShadowMap: 页表缓冲创建失败");
        tiny::resourcestats::countBuffer();
        pageTableMapped[frame] = info.pMappedData;
        if (pageTableMapped[frame] == nullptr)
            throw std::runtime_error("VirtualShadowMap: 页表缓冲没有映射到 CPU");
    }

    std::cout << "[VSM] page table ready | levels=" << kClipmapLevels
              << " axis=" << kPageTableAxis << " entries=" << kPageTableEntries
              << " stride=" << pageTableStride << "B frameSlots=" << kFramesInFlight << std::endl;
}

void VirtualShadowMap::destroyResources()
{
    if (hzbView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device.getDevice(), hzbView, nullptr);
        hzbView = VK_NULL_HANDLE;
    }
    if (hzbAtlas != VK_NULL_HANDLE)
    {
        vmaDestroyImage(device.getVmaAllocator(), hzbAtlas, hzbAllocation);
        hzbAtlas = VK_NULL_HANDLE;
        hzbAllocation = VK_NULL_HANDLE;
    }
    if (depthView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device.getDevice(), depthView, nullptr);
        depthView = VK_NULL_HANDLE;
    }
    if (depthAtlas != VK_NULL_HANDLE)
    {
        vmaDestroyImage(device.getVmaAllocator(), depthAtlas, depthAllocation);
        depthAtlas = VK_NULL_HANDLE;
        depthAllocation = VK_NULL_HANDLE;
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        pageTableMapped[frame] = nullptr;
        if (pageTableBuffers[frame] != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device.getVmaAllocator(), pageTableBuffers[frame], pageTableAllocations[frame]);
            pageTableBuffers[frame] = VK_NULL_HANDLE;
            pageTableAllocations[frame] = VK_NULL_HANDLE;
        }
    }
}

tiny::SparsePageAllocator& VirtualShadowMap::getAllocator()
{
    if (!allocator)
        throw std::runtime_error("VirtualShadowMap: 还没 init 就要拿页分配器");
    return *allocator;
}

void VirtualShadowMap::resetPageTable()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (pageTableMapped[frame] == nullptr)
            continue;
        auto* entries = static_cast<uint32_t*>(pageTableMapped[frame]);
        for (uint64_t i = 0; i < pageTableStride / sizeof(uint32_t); ++i)
            entries[i] = kInvalidPage;
        vmaFlushAllocation(device.getVmaAllocator(), pageTableAllocations[frame], 0, VK_WHOLE_SIZE);
    }
}

void VirtualShadowMap::reportResources() const
{
    std::printf("[VSM] resources | pages=%u pageSize=%u atlas=%ux%u depth=%.1fMiB(%s) "
                "hzb=%.1fMiB(%s, %u levels, rowUsage=%u/%u) "
                "pageTable=%.1fKiB (levels=%u x %u entries x %u slots) | total=%.1fMiB | "
                "clipmap=%u levels x %ux%u pages = %u used / %u spare\n",
                stats.pageCount, stats.pageSize, stats.atlasResolution, stats.atlasResolution,
                mib(stats.depthBytes), formatName(VK_FORMAT_D32_SFLOAT),
                mib(stats.hzbBytes), formatName(VK_FORMAT_R16G16_SFLOAT), stats.hzbLevels,
                stats.hzbRowUsage, kPageSize,
                static_cast<double>(stats.pageTableBytes) / 1024.0,
                stats.clipmapLevels, stats.pageTableEntries, kFramesInFlight,
                mib(stats.totalBytes),
                stats.clipmapLevels, kPageTableAxis, kPageTableAxis,
                stats.pageTableUsed, stats.pageTableSpare);
    std::printf("[VSM] hzb layout | levelSizes=");
    for (uint32_t level = 1; level <= kHzbLevels; ++level)
        std::printf("%u%s", hzbTileSize(level), level == kHzbLevels ? "" : ",");
    std::printf(" offsets=");
    for (uint32_t level = 1; level <= kHzbLevels; ++level)
        std::printf("%u%s", hzbOffsetX(level), level == kHzbLevels ? "" : ",");
    std::printf(" (每页瓦片第一条行带，%u/%u texel)\n", stats.hzbRowUsage, kPageSize);
    if (!isAllocated())
        std::printf("[VSM] resources | 没有建显存（只有 --dump-vsm-resources 会建）\n");
}

bool VirtualShadowMap::selfTest()
{
    // 1) 图集尺寸与页数自洽：8192 / 128 = 64，64x64 = 4096。
    if (kAtlasResolution != kAtlasPagesPerAxis * kPageSize)
        return false;
    if (kPageCount != kAtlasPagesPerAxis * kAtlasPagesPerAxis)
        return false;
    if (kAtlasResolution / kPageSize != kAtlasPagesPerAxis)
        return false;

    // 2) 每个页的瓦片都落在图集内，且相邻页不重叠（四条角页 + 每个角各加一页）。
    const uint32_t samples[] = { 0u, 1u, 63u, 64u, 65u, 4032u, 4095u };
    for (uint32_t page : samples)
    {
        if (page >= kPageCount)
            return false;
        const uint32_t x = pageOriginX(page);
        const uint32_t y = pageOriginY(page);
        if (x % kPageSize != 0 || y % kPageSize != 0)
            return false;
        if (x + kPageSize > kAtlasResolution || y + kPageSize > kAtlasResolution)
            return false;
    }
    if (pageOriginX(0) != 0 || pageOriginY(0) != 0)
        return false;
    if (pageOriginX(63) != kAtlasResolution - kPageSize || pageOriginY(63) != 0)
        return false;
    if (pageOriginX(64) != 0 || pageOriginY(64) != kPageSize)
        return false;
    if (pageOriginX(4095) != kAtlasResolution - kPageSize || pageOriginY(4095) != kAtlasResolution - kPageSize)
        return false;

    // 3) HZB 七级：边长逐级减半到 1，偏移严格递增，整条行带不出页瓦片。
    uint32_t expected = kPageSize >> 1;
    uint32_t previousOffset = 0;
    uint32_t previousSize = 0;
    for (uint32_t level = 1; level <= kHzbLevels; ++level)
    {
        const uint32_t size = hzbTileSize(level);
        const uint32_t offset = hzbOffsetX(level);
        if (size != expected)
            return false;
        if (level > 1 && (offset <= previousOffset || offset != previousOffset + previousSize))
            return false;
        if (offset + size > kPageSize)
            return false;
        previousOffset = offset;
        previousSize = size;
        expected >>= 1;
    }
    if (hzbTileSize(kHzbLevels) != 1)
        return false;
    // 行带占用必须是 64+32+16+8+4+2+1 = 127，正好剩 1 个 texel。
    if (hzbOffsetX(kHzbLevels) + hzbTileSize(kHzbLevels) != kPageSize - 1)
        return false;

    // 4) 页表覆盖率：3 级 x 1024 项 = 3072 <= 4096，余 1024。
    const uint32_t used = kClipmapLevels * kPageTableEntries;
    if (used > kPageCount || kPageCount - used != 1024)
        return false;

    // 5) 页表下标与字节步长。
    if (pageTableIndex(0, 0) != 0 || pageTableIndex(31, 0) != 31 || pageTableIndex(0, 1) != 32)
        return false;
    if (pageTableIndex(31, 31) != kPageTableEntries - 1)
        return false;
    const uint64_t stride = static_cast<uint64_t>(kClipmapLevels) * kPageTableEntries * sizeof(uint32_t);
    if (stride != 12ull * 1024ull)
        return false;

    return true;
}
