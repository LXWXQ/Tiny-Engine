#pragma once

#include "Core/FrameInFlight.h"
#include "RHI/Device.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

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

    // P4-3：三级 clipmap 的世界尺度。级比固定 4：4.8 / 19.2 / 76.8 米。
    // 第 0 级 4096 texel 铺 4.8 米 = 853 texel/m，而 CSM 的 4096 texel 铺
    // 2 x 19.83 = 39.66 米 = 103 texel/m，正好是 8.3 倍 —— 这就是验收①
    // 「同显存下近处 texel 密度 >= CSM 8 倍」的那个分母。
    static constexpr float kClipmapLevel0Extent = 4.8f;
    static constexpr float kClipmapLevelRatio = 4.0f;
    // 一级的边长（texel）：32 页 x 128 texel。
    static constexpr uint32_t kClipmapTexelsPerAxis = kPageTableAxis * kPageSize;

    // 一级 clipmap 的几何：一级就是一个在光空间里轴对齐的正方形，边长 worldExtent 米，
    // 用 4096 texel 画完；它的中心吸附到页网格上（pageWorldSize 的整数倍），
    // 所以相机连续移动时页面不会逐帧抖动，只在跨页时整页滚动。
    struct ClipmapLevel
    {
        float worldExtent = 0.0f;      // 覆盖的世界边长（米）
        float texelWorldSize = 0.0f;   // 一个 texel 多宽（米）
        float pageWorldSize = 0.0f;    // 一页的世界边长 = kPageSize * texelWorldSize
        glm::vec3 center{ 0.0f };      // 吸附之后的中心（世界坐标）
        glm::mat4 lightViewProjection{ 1.0f };  // 世界 -> 该级的 [0,1]^3
    };

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

    // P4-3：按相机位置与光方向重算三级 clipmap（中心吸附到页网格）。
    void updateClipmaps(const glm::vec3& cameraPosition, const glm::vec3& lightDirection);
    const ClipmapLevel& getClipmap(uint32_t level) const { return clipmaps[level]; }
    const std::vector<ClipmapLevel>& getClipmaps() const { return clipmaps; }

    // 滚动统计（P4-3 的证据）：相机移动时每级中心只会整页跳，所以每次变化的位移
    // 必须是 pageWorldSize 的整数倍；aligned 一旦为 false 就说明吸附算错了。
    uint64_t getClipmapScrolls() const { return clipmapScrolls; }
    bool isClipmapAligned() const { return clipmapAligned; }

    // 吸附到页网格：把世界坐标沿光空间的两条轴分别取整到 pageWorldSize 的整数倍。
    // 拆成静态函数是为了让自检不依赖设备。
    static glm::vec3 snapToPageGrid(const glm::vec3& position, const glm::vec3& lightDirection,
                                    float pageWorldSize);
    // 单级 clipmap 的纯计算版本（自检用；updateClipmaps 逐级调它）。
    static ClipmapLevel computeClipmapLevel(uint32_t level, const glm::vec3& cameraPosition,
                                           const glm::vec3& lightDirection);

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

    std::vector<ClipmapLevel> clipmaps;
    bool clipmapReported = false;
    uint64_t clipmapFrame = 0;
    uint64_t clipmapScrolls = 0;
    bool clipmapAligned = true;

    std::unique_ptr<tiny::SparsePageAllocator> allocator;
    VkBuffer pageTableBuffers[kFramesInFlight] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VmaAllocation pageTableAllocations[kFramesInFlight] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    void* pageTableMapped[kFramesInFlight] = { nullptr, nullptr };
    uint64_t pageTableStride = 0;

    Stats stats{};
};
