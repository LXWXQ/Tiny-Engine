#pragma once

#include "Core/FrameInFlight.h"
#include "RHI/Device.h"

#include <cstdint>
#include <memory>
#include <string>

class Device;
namespace tiny { class SparsePageAllocator; }

// P4-2：虚拟阴影图的资源骨架（深度图集 / 页表 / 每页 HZB）。
//
// 这一层不画任何东西，也不改帧循环里的任何读数 —— 它只把「页」变成可采样、可建
// 金字塔、可被页表指向的纹理，供 P4-3（clipmap 滚动）、P4-4（页标记）、P4-6
// （页表采样）使用。布局与 docs/80-roadmap/ue-borrowing-plan.md 的 2.9 节对齐：
//
//   - 深度图集 8192^2，页 128^2，64x64 = 4096 页。页号 = 物理索引，一页就是图集上
//     左上角为 (pageOriginX, pageOriginY) 的一块瓦片；
//   - 页表每级 clipmap 一张 32x32（1024 项），3 级共 3072 项 —— 4096 页里留 1024 页
//     余量给「同一级多要一圈」或临时页；
//   - 每页 HZB 7 级（log2(128)）。不另开一张金字塔图：第 L 级（L >= 1）的瓦片按
//     64/32/16/8/4/2/1 依次横排在**同一页瓦片的第一条行带**上
//     （64+32+16+8+4+2+1 = 127 <= 128），所以 HZB 图集与深度图集同尺寸同页号，
//     寻址只是一次加法，也没有第二套页分配。
//
// 为什么不给 HZB 单独一套页（2026-11-26）：那要多一套分配器、多一张页表、多一次
// 失效同步，而它省下的显存只是「每页 128x128 里没被金字塔用满的那一半」。P4-2 的
// 目标是先把形状立住，等 P4-3 真跑起来看显存曲线再决定要不要收紧。
class VirtualShadowMap
{
public:
    static constexpr uint32_t kPageSize = 128;
    static constexpr uint32_t kAtlasPagesPerAxis = 64;
    static constexpr uint32_t kPageCount = kAtlasPagesPerAxis * kAtlasPagesPerAxis;      // 4096
    static constexpr uint32_t kAtlasResolution = kAtlasPagesPerAxis * kPageSize;         // 8192
    static constexpr uint32_t kClipmapLevels = 3;
    static constexpr uint32_t kPageTableAxis = 32;
    static constexpr uint32_t kPageTableEntries = kPageTableAxis * kPageTableAxis;        // 1024
    static constexpr uint32_t kHzbLevels = 7;                                             // log2(128)
    static constexpr uint32_t kInvalidPage = 0xFFFFFFFFu;

    struct Stats
    {
        uint32_t pageCount = 0;
        uint32_t pageSize = 0;
        uint32_t atlasResolution = 0;
        uint32_t clipmapLevels = 0;
        uint32_t pageTableEntries = 0;
        uint32_t pageTableUsed = 0;        // clipmapLevels * pageTableEntries
        uint32_t pageTableSpare = 0;       // pageCount - pageTableUsed
        uint32_t hzbLevels = 0;
        uint32_t hzbRowUsage = 0;          // 一页瓦片第一条行带里被金字塔占掉的 texel 数
        uint64_t depthBytes = 0;
        uint64_t hzbBytes = 0;
        uint64_t pageTableBytes = 0;
        uint64_t totalBytes = 0;
    };

    // 页 -> 图集瓦片左上角（同一套坐标，深度图集与 HZB 图集共用）。
    static uint32_t pageOriginX(uint32_t page) { return (page % kAtlasPagesPerAxis) * kPageSize; }
    static uint32_t pageOriginY(uint32_t page) { return (page / kAtlasPagesPerAxis) * kPageSize; }

    // HZB 第 level 级（1..7）的瓦片边长；第 0 级就是深度页本身，不在 HZB 图集里。
    static uint32_t hzbTileSize(uint32_t level) { return kPageSize >> level; }
    // 该级瓦片在页瓦片内的 x 偏移：把 64/32/16/8/4/2/1 依次往右排。
    static uint32_t hzbOffsetX(uint32_t level)
    {
        uint32_t offset = 0;
        for (uint32_t i = 1; i < level; ++i)
            offset += hzbTileSize(i);
        return offset;
    }
    // 页表项在「一级 clipmap 的 32x32 页表」里的下标。
    static uint32_t pageTableIndex(uint32_t x, uint32_t y) { return y * kPageTableAxis + x; }

    explicit VirtualShadowMap(Device& device);
    ~VirtualShadowMap();

    // allocateResources = false 时只建立 CPU 侧状态（页分配器、页表镜像）与自检，
    // 不碰显存 —— P4-2 阶段只有 --dump-vsm-resources 会要求真正建图。
    void init(bool allocateResources);
    void destroy();

    bool isAllocated() const { return depthAtlas != VK_NULL_HANDLE; }
    bool isReady() const { return initialized; }
    const Stats& getStats() const { return stats; }
    tiny::SparsePageAllocator& getAllocator();

    VkImage getDepthAtlas() const { return depthAtlas; }
    VkImageView getDepthView() const { return depthView; }
    VkImage getHzbAtlas() const { return hzbAtlas; }
    VkImageView getHzbView() const { return hzbView; }
    const VkBuffer* getPageTableBuffers() const { return pageTableBuffers; }
    uint64_t getPageTableStride() const { return pageTableStride; }

    // 页表镜像的 CPU 侧重置（每级每项写 kInvalidPage）。
    void resetPageTable();

    void reportResources() const;

    // 纯整数运算的布局自检：页 -> 瓦片、HZB 七级的偏移与行占用、页表覆盖率、
    // 各级页表的字节步长。每次启动跑，不需要 Vulkan 设备。
    static bool selfTest();

private:
    void createAtlas();
    void createPageTables();
    void destroyResources();

    Device& device;
    bool initialized = false;

    VkImage depthAtlas = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkImage hzbAtlas = VK_NULL_HANDLE;
    VmaAllocation hzbAllocation = VK_NULL_HANDLE;
    VkImageView hzbView = VK_NULL_HANDLE;

    std::unique_ptr<tiny::SparsePageAllocator> allocator;
    VkBuffer pageTableBuffers[kFramesInFlight] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VmaAllocation pageTableAllocations[kFramesInFlight] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    void* pageTableMapped[kFramesInFlight] = { nullptr, nullptr };
    uint64_t pageTableStride = 0;

    Stats stats{};
};
