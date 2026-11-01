#include "Render/SubMeshCulling.h"

#include <algorithm>
#include <limits>

namespace render
{

uint32_t selectTerrainLod(float distanceToBounds,
                          float chunkExtent,
                          float distanceScale,
                          uint32_t lodLevelCount)
{
    if (lodLevelCount <= 1u)
        return 0u;

    const float extent = std::max(chunkExtent, 1.0f);
    uint32_t selected = 0u;
    for (uint32_t lod = 1u; lod < lodLevelCount; ++lod)
    {
        const float threshold = extent * distanceScale *
            (1.25f * static_cast<float>(1u << (lod - 1u)));
        if (distanceToBounds <= threshold)
            break;
        selected = lod;
    }
    return selected;
}


namespace
{
constexpr float kLodTransitionSeconds = 0.24f;
constexpr float kLodHysteresis = 0.15f;

float lodThreshold(uint32_t lod, float extent, float distanceScale)
{
    return extent * distanceScale * 1.25f *
           static_cast<float>(1u << (lod - 1u));
}
} // namespace

uint32_t selectTerrainLodHysteresis(float distanceToBounds,
                                     float chunkExtent,
                                     float distanceScale,
                                     uint32_t lodLevelCount,
                                     int32_t currentLod)
{
    const uint32_t raw = selectTerrainLod(
        distanceToBounds, chunkExtent, distanceScale, lodLevelCount);
    if (currentLod < 0 ||
        static_cast<uint32_t>(currentLod) >= lodLevelCount)
        return raw;

    const float extent = std::max(chunkExtent, 1.0f);
    uint32_t target = static_cast<uint32_t>(currentLod);
    if (raw > target)
    {
        while (target < raw)
        {
            const float enter = lodThreshold(target + 1u, extent, distanceScale) *
                                (1.0f + kLodHysteresis);
            if (distanceToBounds <= enter)
                break;
            ++target;
        }
    }
    else if (raw < target)
    {
        while (target > raw)
        {
            const float exit = lodThreshold(target, extent, distanceScale) *
                               (1.0f - kLodHysteresis);
            if (distanceToBounds >= exit)
                break;
            --target;
        }
    }
    return target;
}

TerrainLodSelection updateTerrainLodSelection(float distanceToBounds,
                                               float chunkExtent,
                                               float distanceScale,
                                               uint32_t lodLevelCount,
                                               float deltaTime,
                                               TerrainLodState& state)
{
    if (lodLevelCount == 0u)
        return {};
    if (state.stableLod < 0 ||
        static_cast<uint32_t>(state.stableLod) >= lodLevelCount)
    {
        state = {};
        state.stableLod = static_cast<int32_t>(selectTerrainLod(
            distanceToBounds, chunkExtent, distanceScale, lodLevelCount));
        state.transitionFrom = state.stableLod;
        state.transitionTo = state.stableLod;
    }

    if (state.transitionFrom >= 0 && state.transitionFrom != state.transitionTo)
    {
        state.transitionElapsed += std::max(deltaTime, 0.0f);
        const float blend = std::min(
            state.transitionElapsed / kLodTransitionSeconds, 1.0f);
        if (blend >= 1.0f)
        {
            state.stableLod = state.transitionTo;
            state.transitionFrom = state.stableLod;
            state.transitionTo = state.stableLod;
            state.transitionElapsed = 0.0f;
        }
        else
        {
            TerrainLodSelection selection;
            selection.lodA = static_cast<uint32_t>(state.transitionFrom);
            selection.lodB = static_cast<uint32_t>(state.transitionTo);
            selection.blend = blend;
            selection.blending = true;
            return selection;
        }
    }

    const uint32_t target = selectTerrainLodHysteresis(
        distanceToBounds, chunkExtent, distanceScale, lodLevelCount,
        state.stableLod);
    if (target != static_cast<uint32_t>(state.stableLod))
    {
        state.transitionFrom = state.stableLod;
        state.transitionTo = static_cast<int32_t>(target);
        state.transitionElapsed = 0.0f;

        TerrainLodSelection selection;
        selection.lodA = static_cast<uint32_t>(state.transitionFrom);
        selection.lodB = static_cast<uint32_t>(state.transitionTo);
        selection.blend = 0.0f;
        selection.blending = true;
        return selection;
    }

    TerrainLodSelection selection;
    selection.lodA = static_cast<uint32_t>(state.stableLod);
    selection.lodB = selection.lodA;
    selection.blend = 1.0f;
    selection.blending = false;
    return selection;
}

void worldBounds(const glm::vec3& localMin,
                 const glm::vec3& localMax,
                 const glm::mat4& world,
                 glm::vec3& outMin,
                 glm::vec3& outMax)
{
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());

    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 corner(
            (i & 1) ? localMax.x : localMin.x,
            (i & 2) ? localMax.y : localMin.y,
            (i & 4) ? localMax.z : localMin.z);
        const glm::vec3 worldCorner = glm::vec3(world * glm::vec4(corner, 1.0f));
        outMin = glm::min(outMin, worldCorner);
        outMax = glm::max(outMax, worldCorner);
    }
}

SubMeshCuller::SubMeshCuller(const glm::mat4& viewProjection)
{
    const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
    const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
    const glm::vec4 row2(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
    const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);

    planes[0] = row3 + row0;
    planes[1] = row3 - row0;
    planes[2] = row3 + row1;
    planes[3] = row3 - row1;
    planes[4] = row2;
    planes[5] = row3 - row2;
}

bool SubMeshCuller::intersects(const glm::vec3& localMin,
                               const glm::vec3& localMax,
                               const glm::mat4& world) const
{
    glm::vec3 worldMin;
    glm::vec3 worldMax;
    worldBounds(localMin, localMax, world, worldMin, worldMax);

    for (const glm::vec4& plane : planes)
    {
        const glm::vec3 positivePoint(
            plane.x >= 0.0f ? worldMax.x : worldMin.x,
            plane.y >= 0.0f ? worldMax.y : worldMin.y,
            plane.z >= 0.0f ? worldMax.z : worldMin.z);
        if (glm::dot(glm::vec3(plane), positivePoint) + plane.w < 0.0f)
            return false;
    }
    return true;
}

} // namespace render