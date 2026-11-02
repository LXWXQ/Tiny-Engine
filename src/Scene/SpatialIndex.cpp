#include "Scene/SpatialIndex.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool entityLess(entt::entity a, entt::entity b)
    {
        return entt::to_integral(a) < entt::to_integral(b);
    }

    void sortUnique(std::vector<entt::entity>& entities)
    {
        std::sort(entities.begin(), entities.end(), entityLess);
        entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
    }
}

SpatialIndex::SpatialIndex(float cellSize)
    : cellSize(cellSize > 0.0f ? cellSize : 16.0f)
{
}

void SpatialIndex::clear()
{
    cells.clear();
    oversized.clear();
    regions.clear();
    regionOversized.clear();
    entityEntries.clear();
    entries = 0;
}

SpatialIndex::Cell SpatialIndex::cellOf(const glm::vec3& p, float size)
{
    return Cell{
        static_cast<int>(std::floor(p.x / size)),
        static_cast<int>(std::floor(p.y / size)),
        static_cast<int>(std::floor(p.z / size))
    };
}

SpatialIndex::RegionCell SpatialIndex::regionOf(const glm::vec3& p, float size)
{
    return RegionCell{
        static_cast<int32_t>(std::floor(p.x / size)),
        static_cast<int32_t>(std::floor(p.z / size))
    };
}

void SpatialIndex::setRegionSize(float size)
{
    if (size <= 0.0f || size == regionSize)
        return;
    regionSize = size;
    rebuildRegions();
}

void SpatialIndex::addRegions(Entry& entry)
{
    if (!entry.bounds.valid)
        return;

    const RegionCell minRegion = regionOf(entry.bounds.min, regionSize);
    const RegionCell maxRegion = regionOf(entry.bounds.max, regionSize);
    const int64_t sx = static_cast<int64_t>(maxRegion.x) - minRegion.x + 1;
    const int64_t sz = static_cast<int64_t>(maxRegion.z) - minRegion.z + 1;
    const int64_t count = sx * sz;

    if (count <= 0 || count > 1024)
    {
        entry.regionOversized = true;
        regionOversized.push_back(entry.entity);
        return;
    }

    entry.regionCells.reserve(static_cast<size_t>(count));
    for (int32_t z = minRegion.z; z <= maxRegion.z; ++z)
        for (int32_t x = minRegion.x; x <= maxRegion.x; ++x)
        {
            const RegionCell region{ x, z };
            regions[region].push_back(entry.entity);
            entry.regionCells.push_back(region);
        }
}

void SpatialIndex::rebuildRegions()
{
    regions.clear();
    regionOversized.clear();
    for (auto& [key, entry] : entityEntries)
    {
        (void)key;
        entry.regionCells.clear();
        entry.regionOversized = false;
        addRegions(entry);
    }
}

void SpatialIndex::insert(entt::entity entity, const AABB& bounds)
{
    remove(entity);
    if (!bounds.valid)
        return;

    const Cell minCell = cellOf(bounds.min, cellSize);
    const Cell maxCell = cellOf(bounds.max, cellSize);

    const int64_t sx = static_cast<int64_t>(maxCell.x) - minCell.x + 1;
    const int64_t sy = static_cast<int64_t>(maxCell.y) - minCell.y + 1;
    const int64_t sz = static_cast<int64_t>(maxCell.z) - minCell.z + 1;
    const int64_t cellCount = sx * sy * sz;

    Entry entry;
    entry.entity = entity;
    entry.bounds = bounds;

    // Huge objects go into an oversized list so a single object cannot explode
    // the grid. Oversized entries are always included as query candidates.
    if (cellCount <= 0 || cellCount > 4096)
    {
        oversized.push_back(entity);
        entry.oversized = true;
    }
    else
    {
        entry.cells.reserve(static_cast<size_t>(cellCount));
        for (int z = minCell.z; z <= maxCell.z; ++z)
            for (int y = minCell.y; y <= maxCell.y; ++y)
                for (int x = minCell.x; x <= maxCell.x; ++x)
                {
                    const Cell cell{ x, y, z };
                    cells[cell].push_back(entity);
                    entry.cells.push_back(cell);
                }
    }

    addRegions(entry);
    entityEntries[static_cast<uint32_t>(entt::to_integral(entity))] = std::move(entry);
    entries = entityEntries.size();
}

bool SpatialIndex::remove(entt::entity entity)
{
    const uint32_t key = static_cast<uint32_t>(entt::to_integral(entity));
    auto entryIt = entityEntries.find(key);
    if (entryIt == entityEntries.end())
        return false;

    const Entry& entry = entryIt->second;
    for (const RegionCell& region : entry.regionCells)
    {
        auto regionIt = regions.find(region);
        if (regionIt == regions.end())
            continue;
        std::vector<entt::entity>& bucket = regionIt->second;
        bucket.erase(std::remove(bucket.begin(), bucket.end(), entity), bucket.end());
        if (bucket.empty())
            regions.erase(regionIt);
    }
    if (entry.regionOversized)
    {
        regionOversized.erase(
            std::remove(regionOversized.begin(), regionOversized.end(), entity),
            regionOversized.end());
    }

    if (entry.oversized)
    {
        oversized.erase(std::remove(oversized.begin(), oversized.end(), entity),
                        oversized.end());
    }
    else
    {
        for (const Cell& cell : entry.cells)
        {
            auto cellIt = cells.find(cell);
            if (cellIt == cells.end())
                continue;
            std::vector<entt::entity>& bucket = cellIt->second;
            bucket.erase(std::remove(bucket.begin(), bucket.end(), entity), bucket.end());
            if (bucket.empty())
                cells.erase(cellIt);
        }
    }

    entityEntries.erase(entryIt);
    entries = entityEntries.size();
    return true;
}

void SpatialIndex::queryRegion(int32_t regionX, int32_t regionZ,
                               std::vector<entt::entity>& outEntities) const
{
    queryRegionRange(regionX, regionZ, regionX, regionZ, outEntities);
}

void SpatialIndex::queryRegionRange(int32_t minRegionX, int32_t minRegionZ,
                                    int32_t maxRegionX, int32_t maxRegionZ,
                                    std::vector<entt::entity>& outEntities) const
{
    outEntities.clear();
    if (minRegionX > maxRegionX || minRegionZ > maxRegionZ)
        return;

    const int64_t sx = static_cast<int64_t>(maxRegionX) - minRegionX + 1;
    const int64_t sz = static_cast<int64_t>(maxRegionZ) - minRegionZ + 1;
    const int64_t count = sx * sz;
    if (count > 4096)
    {
        collectAll(outEntities);
        sortUnique(outEntities);
        return;
    }

    for (int32_t z = minRegionZ; z <= maxRegionZ; ++z)
        for (int32_t x = minRegionX; x <= maxRegionX; ++x)
        {
            auto it = regions.find(RegionCell{ x, z });
            if (it != regions.end())
                outEntities.insert(outEntities.end(), it->second.begin(), it->second.end());
        }

    outEntities.insert(outEntities.end(), regionOversized.begin(), regionOversized.end());
    sortUnique(outEntities);
}

void SpatialIndex::collectAll(std::vector<entt::entity>& outEntities) const
{
    for (const auto& [cell, bucket] : cells)
    {
        (void)cell;
        outEntities.insert(outEntities.end(), bucket.begin(), bucket.end());
    }
    outEntities.insert(outEntities.end(), oversized.begin(), oversized.end());
}

void SpatialIndex::query(const AABB& bounds, std::vector<entt::entity>& outEntities) const
{
    outEntities.clear();
    if (!bounds.valid)
        return;

    const Cell minCell = cellOf(bounds.min, cellSize);
    const Cell maxCell = cellOf(bounds.max, cellSize);

    const int64_t sx = static_cast<int64_t>(maxCell.x) - minCell.x + 1;
    const int64_t sy = static_cast<int64_t>(maxCell.y) - minCell.y + 1;
    const int64_t sz = static_cast<int64_t>(maxCell.z) - minCell.z + 1;
    const int64_t cellCount = sx * sy * sz;

    if (cellCount <= 0 || cellCount > 4096)
    {
        collectAll(outEntities);
        sortUnique(outEntities);
        return;
    }

    for (int z = minCell.z; z <= maxCell.z; ++z)
        for (int y = minCell.y; y <= maxCell.y; ++y)
            for (int x = minCell.x; x <= maxCell.x; ++x)
            {
                auto it = cells.find(Cell{ x, y, z });
                if (it != cells.end())
                    outEntities.insert(outEntities.end(), it->second.begin(), it->second.end());
            }

    outEntities.insert(outEntities.end(), oversized.begin(), oversized.end());
    sortUnique(outEntities);
}