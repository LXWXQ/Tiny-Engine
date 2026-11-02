#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace render
{

uint32_t selectTerrainLod(float distanceToBounds,
                            float chunkExtent,
                            float distanceScale,
                            uint32_t lodLevelCount);

// LOD hysteresis keeps a chunk on its current level inside a 15% dead band.
uint32_t selectTerrainLodHysteresis(float distanceToBounds,
                                     float chunkExtent,
                                     float distanceScale,
                                     uint32_t lodLevelCount,
                                     int32_t currentLod);

struct TerrainLodState
{
    int32_t stableLod = -1;
    int32_t transitionFrom = -1;
    int32_t transitionTo = -1;
    float transitionElapsed = 0.0f;
};

struct TerrainLodSelection
{
    uint32_t lodA = 0;
    uint32_t lodB = 0;
    float blend = 0.0f;
    bool blending = false;
};

// Advances the per-chunk LOD state machine. While blending, draw both lodA and
// lodB with complementary screen-space dither masks (blend = incoming coverage).
TerrainLodSelection updateTerrainLodSelection(float distanceToBounds,
                                               float chunkExtent,
                                               float distanceScale,
                                               uint32_t lodLevelCount,
                                               float deltaTime,
                                               TerrainLodState& state);

void worldBounds(const glm::vec3& localMin,
                 const glm::vec3& localMax,
                 const glm::mat4& world,
                 glm::vec3& outMin,
                 glm::vec3& outMax);

// Pure CPU frustum-AABB test used for per-chunk submesh culling. Kept outside
// GeometrySystem so Scene::selfTest can validate it without Vulkan.
class SubMeshCuller
{
public:
    explicit SubMeshCuller(const glm::mat4& viewProjection);

    bool intersects(const glm::vec3& localMin,
                    const glm::vec3& localMax,
                    const glm::mat4& world) const;

private:
    glm::vec4 planes[6];
};

} // namespace render