#include "Render/VirtualShadowMap.h"

#include "Render/SparsePageAllocator.h"
#include "RHI/ResourceStats.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
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

    // 光空间的两条直角轴（与 ShadowPass::buildCascade 同一套取法：光方向 + 世界上方
    // 叉乘出 right / lightUp，方向几乎与上方平行时换一个参考轴避免退化）。
    void lightBasis(const glm::vec3& lightDirection, glm::vec3& direction, glm::vec3& right,
                    glm::vec3& lightUp)
    {
        direction = lightDirection;
        if (glm::dot(direction, direction) < 1.0e-8f)
            direction = glm::vec3(0.0f, -1.0f, 0.0f);
        direction = glm::normalize(direction);

        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(direction, up)) > 0.99f)
            up = glm::vec3(0.0f, 0.0f, 1.0f);
        right = glm::normalize(glm::cross(direction, up));
        lightUp = glm::cross(right, direction);
    }
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

    // P4-6：采样器。最近邻 + 边缘钳到 1.0：页是 128x128 的独立瓦片，
    // 线性过滤会把相邻页（页表是间接的，图上不一定相邻）混进同一个 tap。
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device.getDevice(), &samplerInfo, nullptr, &depthSampler) != VK_SUCCESS)
        throw std::runtime_error("VirtualShadowMap: 深度图集采样器创建失败");

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
    if (depthSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(device.getDevice(), depthSampler, nullptr);
        depthSampler = VK_NULL_HANDLE;
    }
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

void VirtualShadowMap::updatePageTable(uint32_t frameIndex)
{
    if (frameIndex >= kFramesInFlight)
        return;
    if (pageTableMapped[frameIndex] == nullptr)
        return;

    auto* entries = static_cast<uint32_t*>(pageTableMapped[frameIndex]);
    for (uint32_t level = 0; level < kClipmapLevels; ++level)
    {
        const uint32_t base = level * kPageTableEntries;
        for (uint32_t i = 0; i < kPageTableEntries; ++i)
            entries[base + i] = base + i;
    }
    vmaFlushAllocation(device.getVmaAllocator(), pageTableAllocations[frameIndex], 0, VK_WHOLE_SIZE);
}

// ---- P4-4：页标记 ----

bool VirtualShadowMap::pageRangeForBounds(const ClipmapLevel& level, const glm::vec3& lightDirection,
                                          const glm::vec3& center, const glm::vec3& halfExtent,
                                          uint32_t& outMinX, uint32_t& outMinY,
                                          uint32_t& outMaxX, uint32_t& outMaxY)
{
    if (level.pageWorldSize <= 0.0f)
        return false;

    glm::vec3 direction(0.0f);
    glm::vec3 right(0.0f);
    glm::vec3 lightUp(0.0f);
    lightBasis(lightDirection, direction, right, lightUp);

    // 世界 AABB 在光空间两条轴上的区间：中心投过去，半长按逐分量绝对值加权。
    const glm::vec3 offset = center - level.center;
    const float alongRight = glm::dot(right, offset);
    const float alongUp = glm::dot(lightUp, offset);
    const float halfRight = std::abs(right.x) * halfExtent.x + std::abs(right.y) * halfExtent.y +
                            std::abs(right.z) * halfExtent.z;
    const float halfUp = std::abs(lightUp.x) * halfExtent.x + std::abs(lightUp.y) * halfExtent.y +
                         std::abs(lightUp.z) * halfExtent.z;

    // 页坐标 = （光空间偏移 + 半边）/ 页边长 向下取整；一级就是 32 页 x 32 页。
    const float halfExtentWorld = level.worldExtent * 0.5f;
    const float page = level.pageWorldSize;
    const float axisCount = static_cast<float>(kPageTableAxis);

    const float minX = std::floor((alongRight - halfRight + halfExtentWorld) / page);
    const float maxX = std::floor((alongRight + halfRight + halfExtentWorld) / page);
    const float minY = std::floor((alongUp - halfUp + halfExtentWorld) / page);
    const float maxY = std::floor((alongUp + halfUp + halfExtentWorld) / page);

    if (maxX < 0.0f || maxY < 0.0f || minX >= axisCount || minY >= axisCount)
        return false;

    outMinX = static_cast<uint32_t>(std::max(0.0f, minX));
    outMinY = static_cast<uint32_t>(std::max(0.0f, minY));
    outMaxX = static_cast<uint32_t>(std::min(axisCount - 1.0f, maxX));
    outMaxY = static_cast<uint32_t>(std::min(axisCount - 1.0f, maxY));
    return true;
}

void VirtualShadowMap::setPageDirty(uint32_t level, uint32_t index)
{
    if (level >= kClipmapLevels || index >= kPageTableEntries)
        return;
    pageDirty[level][index >> 6] |= (1ull << (index & 63u));
}

void VirtualShadowMap::markWholeLevelDirty(uint32_t level)
{
    if (level >= kClipmapLevels)
        return;
    for (uint32_t word = 0; word < kDirtyWordsPerLevel; ++word)
        pageDirty[level][word] = ~0ull;
}

void VirtualShadowMap::clearPageDirty()
{
    for (uint32_t level = 0; level < kClipmapLevels; ++level)
        for (uint32_t word = 0; word < kDirtyWordsPerLevel; ++word)
            pageDirty[level][word] = 0ull;
}

bool VirtualShadowMap::isPageDirty(uint32_t level, uint32_t pageX, uint32_t pageY) const
{
    if (level >= kClipmapLevels || pageX >= kPageTableAxis || pageY >= kPageTableAxis)
        return false;
    const uint32_t index = pageTableIndex(pageX, pageY);
    return (pageDirty[level][index >> 6] & (1ull << (index & 63u))) != 0ull;
}

uint32_t VirtualShadowMap::getDirtyPageCount(uint32_t level) const
{
    if (level >= kClipmapLevels)
        return 0u;
    uint32_t count = 0;
    for (uint32_t word = 0; word < kDirtyWordsPerLevel; ++word)
    {
        uint64_t bits = pageDirty[level][word];
        while (bits != 0ull)
        {
            bits &= bits - 1ull;
            ++count;
        }
    }
    return count;
}

uint32_t VirtualShadowMap::pageOriginX(uint32_t page)
{
    const uint32_t level = page / kPageTableEntries;
    const uint32_t local = page % kPageTableEntries;
    return (level % kAtlasBlocksPerAxis) * kAtlasBlockSize + (local % kPageTableAxis) * kPageSize;
}

uint32_t VirtualShadowMap::pageOriginY(uint32_t page)
{
    const uint32_t level = page / kPageTableEntries;
    const uint32_t local = page % kPageTableEntries;
    return (level / kAtlasBlocksPerAxis) * kAtlasBlockSize + (local / kPageTableAxis) * kPageSize;
}

void VirtualShadowMap::levelAtlasOrigin(uint32_t level, uint32_t& originX, uint32_t& originY)
{
    originX = (level % kAtlasBlocksPerAxis) * kAtlasBlockSize;
    originY = (level / kAtlasBlocksPerAxis) * kAtlasBlockSize;
}

glm::mat4 VirtualShadowMap::pageViewProjection(const ClipmapLevel& level, uint32_t pageX, uint32_t pageY)
{
    const float axisCount = static_cast<float>(kPageTableAxis);
    // 页中心在这一级 clip 空间里的坐标（[-1,1]）。
    const float centerX = ((static_cast<float>(pageX) + 0.5f) / axisCount) * 2.0f - 1.0f;
    const float centerY = ((static_cast<float>(pageY) + 0.5f) / axisCount) * 2.0f - 1.0f;

    glm::mat4 tile(1.0f);
    tile[0][0] = axisCount;
    tile[1][1] = axisCount;
    tile[3][0] = -centerX * axisCount;
    tile[3][1] = -centerY * axisCount;
    return tile * level.lightViewProjection;
}

void VirtualShadowMap::markPages(const glm::vec3& lightDirection, const std::vector<CasterBounds>& casters)
{
    if (clipmaps.size() != static_cast<size_t>(kClipmapLevels))
        return;

    clearPageDirty();
    ++pageMarkFrames;
    pageMarkStats = PageMarkStats{};
    pageMarkStats.frames = pageMarkFrames;
    pageMarkStats.livePages = kClipmapLevels * kPageTableEntries;

    const glm::vec3 light = glm::normalize(lightDirection);

    // 条件一：光源变换 —— 整张图都得重画（首帧也算一次，那一次确实是全画）。
    const bool lightChanged = !hasMarkLight || glm::dot(light, lastMarkLightDirection) < 0.9999996f;
    if (lightChanged)
    {
        for (uint32_t level = 0; level < kClipmapLevels; ++level)
            markWholeLevelDirty(level);
        ++pageMarkStats.lightEvents;
    }

    // 条件二：页网格滚动 —— 一级中心整页跳了，这一级每一页覆盖的世界方块都变了。
    for (uint32_t level = 0; level < kClipmapLevels; ++level)
    {
        const bool moved = !hasMarkCenters ||
                           glm::length(clipmaps[level].center - lastMarkCenters[level]) > 1e-5f;
        if (moved)
        {
            markWholeLevelDirty(level);
            if (!lightChanged)
                ++pageMarkStats.scrollEvents;
        }
    }

    // 条件三：投影者变换 / 变形 / alpha 模式变化 —— 只重画它盖住的那几页。
    for (const CasterBounds& caster : casters)
    {
        bool changed = true;
        for (const CasterBounds& previous : lastCasters)
        {
            if (previous.id == caster.id)
            {
                changed = previous.version != caster.version;
                break;
            }
        }
        if (!changed)
            continue;

        for (uint32_t level = 0; level < kClipmapLevels; ++level)
        {
            uint32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
            if (!pageRangeForBounds(clipmaps[level], light, caster.center, caster.halfExtent,
                                    minX, minY, maxX, maxY))
                continue;
            for (uint32_t y = minY; y <= maxY; ++y)
                for (uint32_t x = minX; x <= maxX; ++x)
                    setPageDirty(level, pageTableIndex(x, y));
        }
        ++pageMarkStats.casterEvents;
    }

    lastCasters = casters;
    lastMarkLightDirection = light;
    hasMarkLight = true;
    hasMarkCenters = true;
    for (uint32_t level = 0; level < kClipmapLevels; ++level)
        lastMarkCenters[level] = clipmaps[level].center;

    for (uint32_t level = 0; level < kClipmapLevels; ++level)
    {
        pageMarkStats.dirtyByLevel[level] = getDirtyPageCount(level);
        pageMarkStats.dirtyPages += pageMarkStats.dirtyByLevel[level];
    }
    pageMarkStats.cleanPages = pageMarkStats.livePages - pageMarkStats.dirtyPages;

    if (pageMarkFrames % 60ull == 1ull)
    {
        std::printf("[VSM] pages | frame=%llu dirty=%u/%u clean=%u | level0=%u level1=%u level2=%u | light=%u scroll=%u casters=%u\n",
                    static_cast<unsigned long long>(pageMarkFrames), pageMarkStats.dirtyPages,
                    pageMarkStats.livePages, pageMarkStats.cleanPages, pageMarkStats.dirtyByLevel[0],
                    pageMarkStats.dirtyByLevel[1], pageMarkStats.dirtyByLevel[2],
                    pageMarkStats.lightEvents, pageMarkStats.scrollEvents, pageMarkStats.casterEvents);
    }
}

glm::vec3 VirtualShadowMap::snapToPageGrid(const glm::vec3& position, const glm::vec3& lightDirection,
                                            float pageWorldSize)
{
    if (pageWorldSize <= 0.0f)
        return position;

    glm::vec3 direction(0.0f);
    glm::vec3 right(0.0f);
    glm::vec3 lightUp(0.0f);
    lightBasis(lightDirection, direction, right, lightUp);

    // 沿光空间的两条轴各自吸附到页网格：页面在光空间里是轴对齐的方块，
    // 所以只有在这两条轴上取整，页面边界才会和 texel 边界对齐。
    const float alongRight = std::round(glm::dot(right, position) / pageWorldSize) * pageWorldSize;
    const float alongUp = std::round(glm::dot(lightUp, position) / pageWorldSize) * pageWorldSize;
    const float alongDirection = glm::dot(direction, position);

    return right * alongRight + lightUp * alongUp + direction * alongDirection;
}

VirtualShadowMap::ClipmapLevel VirtualShadowMap::computeClipmapLevel(uint32_t level,
                                                                    const glm::vec3& cameraPosition,
                                                                    const glm::vec3& lightDirection)
{
    ClipmapLevel out{};
    out.worldExtent = kClipmapLevel0Extent * std::pow(kClipmapLevelRatio, static_cast<float>(level));
    out.texelWorldSize = out.worldExtent / static_cast<float>(kClipmapTexelsPerAxis);
    out.pageWorldSize = out.texelWorldSize * static_cast<float>(kPageSize);
    out.center = snapToPageGrid(cameraPosition, lightDirection, out.pageWorldSize);

    glm::vec3 direction(0.0f);
    glm::vec3 right(0.0f);
    glm::vec3 lightUp(0.0f);
    lightBasis(lightDirection, direction, right, lightUp);

    // P4-6：光相机沿光线退到 kClipmapOccluderReach 米之外，深度覆盖「退距 + 一级对角线」。
    // 以前只退 2r、深度只有距相机 r .. 3r 这一段（等于级中心 ±0.87 个边长），头顶十米的
    // 屋顶按 45 度太阳方向就是 14 米深度差，直接被近平面裁掉 —— 那一级整片跟着判成受光，
    // 中庭 68% 的像素就这么没了（见 build\P4-6-HANDOFF.md 与决策 84）。
    // r 取这一级正方形外接球半径（边长 x sqrt(3) / 2），保证斜着看也不裁。
    const float halfExtent = out.worldExtent * 0.5f;
    const float radius = halfExtent * 1.7320508f;
    const glm::vec3 eye = out.center - direction * kClipmapOccluderReach;
    const glm::mat4 lightView = glm::lookAt(eye, out.center, lightUp);

    // 手写正交投影：x/y 按这一级的半边归一化，近平面贴在光相机上（t_near = 0），
    // 所以 z_ndc = t / depthRange（t = 到光相机的距离），所有落在这条光线上、
    // 距离不超过 depthRange 的投影者都被收进来。着色器会从合并矩阵的 z 行反解
    // 出 depthRange 来归一化 bias（见 shaders/lighting.frag 的 SampleVirtualShadow）。
    const float depthRange = kClipmapOccluderReach + radius * 2.0f;
    glm::mat4 lightProjection(1.0f);
    lightProjection[0][0] = 1.0f / halfExtent;
    lightProjection[1][1] = 1.0f / halfExtent;
    lightProjection[2][2] = -1.0f / depthRange;
    lightProjection[3][2] = 0.0f;

    out.lightViewProjection = lightProjection * lightView;
    return out;
}

void VirtualShadowMap::updateClipmaps(const glm::vec3& cameraPosition, const glm::vec3& lightDirection)
{
    if (clipmaps.size() != kClipmapLevels)
    {
        clipmaps.resize(kClipmapLevels);
        // 第一帧不是滚动，直接把旧中心标成「无效」的哨兵。
        for (ClipmapLevel& level : clipmaps)
            level.center = glm::vec3(std::numeric_limits<float>::max());
    }

    glm::vec3 direction(0.0f);
    glm::vec3 right(0.0f);
    glm::vec3 lightUp(0.0f);
    lightBasis(lightDirection, direction, right, lightUp);

    for (uint32_t index = 0; index < kClipmapLevels; ++index)
    {
        const ClipmapLevel previous = clipmaps[index];
        ClipmapLevel level = computeClipmapLevel(index, cameraPosition, lightDirection);

        // 滚动只能整页跳：把位移拆到光空间两条轴上，每个分量都得是 pageWorldSize 的整数倍。
        if (previous.worldExtent > 0.0f)
        {
            const glm::vec3 delta = level.center - previous.center;
            if (glm::length(delta) > 1.0e-5f)
            {
                ++clipmapScrolls;
                const float alongRight = glm::dot(right, delta);
                const float alongUp = glm::dot(lightUp, delta);
                const float stepsRight = alongRight / level.pageWorldSize;
                const float stepsUp = alongUp / level.pageWorldSize;
                if (std::abs(stepsRight - std::round(stepsRight)) > 1.0e-3f ||
                    std::abs(stepsUp - std::round(stepsUp)) > 1.0e-3f)
                {
                    clipmapAligned = false;
                }
            }
        }

        clipmaps[index] = level;
    }

    ++clipmapFrame;
    // 每 60 帧一行（只有 --dump-vsm-resources 时才会调到这里）。
    if (clipmapFrame % 60ull == 1ull)
    {
        clipmapReported = true;
        std::printf("[VSM] clipmap | frame=%llu scrolls=%llu aligned=%d | "
                    "level0 extent=%.2fm texel=%.3fmm page=%.3f m | "
                    "level1 extent=%.2fm page=%.2f m | level2 extent=%.2fm page=%.2f m | "
                    "density=%.1f texel/m (CSM %.1f, ratio %.2fx) | center0=(%.3f, %.3f, %.3f)\n",
                    static_cast<unsigned long long>(clipmapFrame),
                    static_cast<unsigned long long>(clipmapScrolls), clipmapAligned ? 1 : 0,
                    clipmaps[0].worldExtent, clipmaps[0].texelWorldSize * 1000.0f,
                    clipmaps[0].pageWorldSize, clipmaps[1].worldExtent,
                    clipmaps[1].pageWorldSize, clipmaps[2].worldExtent, clipmaps[2].pageWorldSize,
                    1.0f / clipmaps[0].texelWorldSize,
                    static_cast<float>(kClipmapTexelsPerAxis) / 39.6632f,
                    39.6632f / clipmaps[0].worldExtent,
                    clipmaps[0].center.x, clipmaps[0].center.y, clipmaps[0].center.z);
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
    if (pageOriginX(31) != kAtlasBlockSize - kPageSize || pageOriginY(31) != 0)
        return false;
    if (pageOriginX(32) != 0 || pageOriginY(32) != kPageSize)
        return false;
    if (pageOriginX(1023) != kAtlasBlockSize - kPageSize || pageOriginY(1023) != kAtlasBlockSize - kPageSize)
        return false;
    if (pageOriginX(1024) != kAtlasBlockSize || pageOriginY(1024) != 0)
        return false;
    if (pageOriginX(3071) != kAtlasBlockSize - kPageSize || pageOriginY(3071) != kAtlasResolution - kPageSize)
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

    // 6) P4-3：一级的 texel 数 = 32 页 x 128 texel，三级页表装得进物理页。
    if (kClipmapTexelsPerAxis != kPageTableAxis * kPageSize)
        return false;
    if (kClipmapLevels * kPageTableEntries > kPageCount)
        return false;

    // 7) 级比 4：4.8 / 19.2 / 76.8 米；每级 texel 尺寸 = 边长 / 4096；页边长 = 128 texel。
    const glm::vec3 lightDirection(0.3f, -0.8f, 0.5f);
    const glm::vec3 cameraPosition(12.3456f, 3.21f, -7.77f);
    const ClipmapLevel level0 = computeClipmapLevel(0u, cameraPosition, lightDirection);
    const ClipmapLevel level1 = computeClipmapLevel(1u, cameraPosition, lightDirection);
    const ClipmapLevel level2 = computeClipmapLevel(2u, cameraPosition, lightDirection);
    if (std::abs(level0.worldExtent - 4.8f) > 1.0e-4f)
        return false;
    if (std::abs(level1.worldExtent - 19.2f) > 1.0e-3f)
        return false;
    if (std::abs(level2.worldExtent - 76.8f) > 1.0e-2f)
        return false;
    const ClipmapLevel levels[3] = { level0, level1, level2 };
    for (const ClipmapLevel& level : levels)
    {
        if (std::abs(level.texelWorldSize * static_cast<float>(kClipmapTexelsPerAxis) - level.worldExtent) > 1.0e-4f)
            return false;
        if (std::abs(level.pageWorldSize - level.texelWorldSize * static_cast<float>(kPageSize)) > 1.0e-9f)
            return false;
        // 吸附幂等：已经在页网格上的中心再吸一次不该动。
        const glm::vec3 again = snapToPageGrid(level.center, lightDirection, level.pageWorldSize);
        if (glm::length(again - level.center) > 1.0e-3f)
            return false;
        // 吸附的位移不超过一页（两条轴上各不超过半页）。
        if (glm::length(level.center - cameraPosition) > level.pageWorldSize * 1.5f)
            return false;
        // 级比严格为 4。
    }
    if (std::abs(level1.worldExtent / level0.worldExtent - kClipmapLevelRatio) > 1.0e-4f)
        return false;
    if (std::abs(level2.worldExtent / level1.worldExtent - kClipmapLevelRatio) > 1.0e-4f)
        return false;

    // 8) 验收①的分母：第 0 级的 texel 密度必须 >= CSM 的 8 倍。
    //    CSM 是 4096 texel 铺 2 x 19.8316 = 39.6632 米。
    const float csmTexelPerMeter = static_cast<float>(kClipmapTexelsPerAxis) / 39.6632f;
    const float vsmTexelPerMeter = 1.0f / level0.texelWorldSize;
    if (vsmTexelPerMeter < csmTexelPerMeter * 8.0f)
        return false;

    // 9) 光空间矩阵：相机所在点必须在第 0 级的 [0,1]^3 里（x/y 在框内，深度在近远之间）。
    const glm::vec4 clip = level0.lightViewProjection * glm::vec4(level0.center, 1.0f);
    if (clip.x < -1.0f || clip.x > 1.0f || clip.y < -1.0f || clip.y > 1.0f)
        return false;
    if (clip.z < 0.0f || clip.z > 1.0f)
        return false;


    // P4-4：脏页位图的字长与页范围换算。
    if (kDirtyWordsPerLevel * 64u != kPageTableEntries)
        return false;
    if (kClipmapLevels * kPageTableEntries != 3072u)
        return false;

    const glm::vec3 probeLight(0.0f, -1.0f, 0.0f);
    const ClipmapLevel probe = computeClipmapLevel(0u, glm::vec3(0.0f), probeLight);
    uint32_t minX = 0, minY = 0, maxX = 0, maxY = 0;

    // 级中心处的一个极小盒子必须落在中间那几页上（32 页的一半附近）。
    if (!pageRangeForBounds(probe, probeLight, probe.center, glm::vec3(0.001f), minX, minY, maxX, maxY))
        return false;
    if (minX > 16u || maxX < 15u || minY > 16u || maxY < 15u)
        return false;

    // 盖住整级的盒子必须裁到 0..31，不能越界。
    if (!pageRangeForBounds(probe, probeLight, probe.center, glm::vec3(probe.worldExtent), minX, minY, maxX, maxY))
        return false;
    if (minX != 0u || minY != 0u || maxX != kPageTableAxis - 1u || maxY != kPageTableAxis - 1u)
        return false;

    // 沿两条光轴各移出一级之外四个边长的盒子，必须判定为不在这级里。
    glm::vec3 outside(0.0f);
    {
        glm::vec3 direction(0.0f);
        glm::vec3 right(0.0f);
        glm::vec3 lightUp(0.0f);
        lightBasis(probeLight, direction, right, lightUp);
        outside = probe.center + right * (probe.worldExtent * 4.0f) + lightUp * (probe.worldExtent * 4.0f);
    }
    if (pageRangeForBounds(probe, probeLight, outside, glm::vec3(0.01f), minX, minY, maxX, maxY))
        return false;

    return true;
}
