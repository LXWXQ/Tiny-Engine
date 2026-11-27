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
    // 图集按 clipmap 分级切成 4096x4096 的区块（8192 / 4096 = 2 x 2，共 4 块：前 3 块给 3 级，
    // 第 4 块空着）。一整级因此是图上的一块连续矩形，重画整级时一条全级绘制就够，
    // 不必逐页发。全局页号 = 级号 * 1024 + 级内页号。
    static constexpr uint32_t kAtlasBlockSize = kPageTableAxis * kPageSize;            // 4096
    static constexpr uint32_t kAtlasBlocksPerAxis = kAtlasResolution / kAtlasBlockSize; // 2

    static uint32_t pageOriginX(uint32_t page);
    static uint32_t pageOriginY(uint32_t page);
    static void levelAtlasOrigin(uint32_t level, uint32_t& originX, uint32_t& originY);

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

    // ---- P4-4：页标记（这一帧哪些页必须重画）----
    // 一个投影者（子网格）的世界包围盒 + 身份。version 变了就说明它的变换 / 变形 /
    // 材质 alpha 模式变了，它盖住的那几页必须重画。
    struct CasterBounds
    {
        uint32_t id = 0u;              // 投影者身份（实体号 x 子网格号），跨帧稳定
        uint64_t version = 0u;         // 变换 + alpha 模式的身份
        glm::vec3 center{ 0.0f };      // 世界轴对齐包围盒的中心
        glm::vec3 halfExtent{ 0.0f };  // 世界轴对齐包围盒的半长
    };

    struct PageMarkStats
    {
        uint32_t livePages = 0;        // 本帧参与的页数 = 3 级 x 1024
        uint32_t dirtyPages = 0;
        uint32_t cleanPages = 0;
        uint32_t dirtyByLevel[kClipmapLevels] = { 0u, 0u, 0u };
        uint32_t lightEvents = 0;      // 光源变换导致整张重画的次数
        uint32_t scrollEvents = 0;     // 某一级整页滚动导致整级重画的次数
        uint32_t casterEvents = 0;     // 投影者变了导致局部重画的次数
        uint64_t frames = 0;
    };

    // 一级里覆盖某个世界 AABB 的页范围（闭区间）；完全落在这一级之外时返回 false。
    static bool pageRangeForBounds(const ClipmapLevel& level, const glm::vec3& lightDirection,
                                   const glm::vec3& center, const glm::vec3& halfExtent,
                                   uint32_t& outMinX, uint32_t& outMinY,
                                   uint32_t& outMaxX, uint32_t& outMaxY);

    // 每帧一次：三条失效条件（光源变换 / 页网格滚动 / 投影者变化）各管一块，
    // 结果是脏页位图。全零就说明上帧内容可以原样复用。
    void markPages(const glm::vec3& lightDirection, const std::vector<CasterBounds>& casters);

    // 把某一页的世界方块铺满整块 128x128 瓦片：一级的 clip 空间里这一页只占 1/32，
    // 所以先放大 32 倍再平移到原点。渲染时配合「viewport 设到该页瓦片」使用。
    static glm::mat4 pageViewProjection(const ClipmapLevel& level, uint32_t pageX, uint32_t pageY);
    void clearPageDirty();
    bool isPageDirty(uint32_t level, uint32_t pageX, uint32_t pageY) const;
    uint32_t getDirtyPageCount(uint32_t level) const;
    const PageMarkStats& getPageMarkStats() const { return pageMarkStats; }

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

    static constexpr uint32_t kDirtyWordsPerLevel = kPageTableEntries / 64;
    void setPageDirty(uint32_t level, uint32_t index);
    void markWholeLevelDirty(uint32_t level);

    uint64_t pageDirty[kClipmapLevels][kDirtyWordsPerLevel] = {};
    std::vector<CasterBounds> lastCasters;
    glm::vec3 lastMarkLightDirection{ 0.0f };
    bool hasMarkLight = false;
    glm::vec3 lastMarkCenters[kClipmapLevels] = { glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f) };
    bool hasMarkCenters = false;
    PageMarkStats pageMarkStats{};
    uint64_t pageMarkFrames = 0;

    std::unique_ptr<tiny::SparsePageAllocator> allocator;
    VkBuffer pageTableBuffers[kFramesInFlight] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VmaAllocation pageTableAllocations[kFramesInFlight] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    void* pageTableMapped[kFramesInFlight] = { nullptr, nullptr };
    uint64_t pageTableStride = 0;

    Stats stats{};
};
