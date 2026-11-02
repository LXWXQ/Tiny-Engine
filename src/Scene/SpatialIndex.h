#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct AABB
{
    glm::vec3 min{ 0.0f };
    glm::vec3 max{ 0.0f };
    bool valid = false;

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extent() const { return max - min; }
};

// D-7: uniform-grid spatial index for CPU broad phase.
// Current scene has only Sponza + skybox; a grid is enough to validate the
// insert -> AABB query -> exact frustum test path and maps well to future chunks.
class SpatialIndex
{
public:
    explicit SpatialIndex(float cellSize = 16.0f);

    void clear();
    // Upsert: an existing entity entry is removed before the new bounds are
    // inserted, so callers can move objects without leaving duplicate buckets.
    void insert(entt::entity entity, const AABB& bounds);
    bool remove(entt::entity entity);
    void query(const AABB& bounds, std::vector<entt::entity>& outEntities) const;

    // B-06: coarse XZ region buckets. A chunk entity inserted into the grid is
    // also indexed by every region its world AABB overlaps, so callers can ask
    // "which chunks are near this region?" without scanning the whole scene.
    void setRegionSize(float size);
    float getRegionSize() const { return regionSize; }
    void queryRegion(int32_t regionX, int32_t regionZ,
                     std::vector<entt::entity>& outEntities) const;
    void queryRegionRange(int32_t minRegionX, int32_t minRegionZ,
                          int32_t maxRegionX, int32_t maxRegionZ,
                          std::vector<entt::entity>& outEntities) const;

    size_t cellCount() const { return cells.size(); }
    size_t regionCellCount() const { return regions.size(); }
    size_t entryCount() const { return entries; }

private:
    struct Cell
    {
        int x = 0, y = 0, z = 0;
        bool operator==(const Cell& other) const { return x == other.x && y == other.y && z == other.z; }
    };

    struct RegionCell
    {
        int32_t x = 0, z = 0;
        bool operator==(const RegionCell& other) const { return x == other.x && z == other.z; }
    };

    struct Entry
    {
        entt::entity entity = entt::null;
        AABB bounds;
        std::vector<Cell> cells;
        bool oversized = false;
        std::vector<RegionCell> regionCells;
        bool regionOversized = false;
    };

    struct RegionCellHash
    {
        size_t operator()(const RegionCell& c) const
        {
            const uint64_t hx = static_cast<uint32_t>(c.x) & 0x1FFFFFu;
            const uint64_t hz = static_cast<uint32_t>(c.z) & 0x1FFFFFu;
            return static_cast<size_t>((hx << 21) | hz);
        }
    };

    struct CellHash
    {
        size_t operator()(const Cell& c) const
        {
            const uint64_t hx = static_cast<uint32_t>(c.x) & 0x1FFFFFu;
            const uint64_t hy = static_cast<uint32_t>(c.y) & 0x1FFFFFu;
            const uint64_t hz = static_cast<uint32_t>(c.z) & 0x1FFFFFu;
            return static_cast<size_t>((hx << 42) | (hy << 21) | hz);
        }
    };

    static Cell cellOf(const glm::vec3& p, float size);
    static RegionCell regionOf(const glm::vec3& p, float size);
    void collectAll(std::vector<entt::entity>& outEntities) const;
    void addRegions(Entry& entry);
    void rebuildRegions();

    float cellSize;
    float regionSize = 64.0f;
    size_t entries = 0;
    std::unordered_map<Cell, std::vector<entt::entity>, CellHash> cells;
    std::vector<entt::entity> oversized;
    std::unordered_map<RegionCell, std::vector<entt::entity>, RegionCellHash> regions;
    std::vector<entt::entity> regionOversized;
    std::unordered_map<uint32_t, Entry> entityEntries;
};