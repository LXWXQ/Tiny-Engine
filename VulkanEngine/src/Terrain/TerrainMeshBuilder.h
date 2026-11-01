#pragma once

#include "Terrain/TerrainConfig.h"
#include "Terrain/TerrainHeightField.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace terrain
{

struct TerrainVertex
{
    glm::vec3 position{ 0.0f };
    glm::vec3 color{ 1.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    glm::vec2 uv{ 0.0f };
};

struct TerrainSubMesh
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t surfaceIndexCount = 0;
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    uint32_t lod = 0;
    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };
};

struct TerrainMeshData
{
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<TerrainSubMesh> subMeshes;
};

class TerrainMeshBuilder
{
public:
    // Builds a regular-grid terrain mesh. The CPU-only mesh data is converted
    // into Model::Builder by Scene, keeping this module independent of Vulkan.
    // Optional cooperative cancellation flag is polled once per vertex row
    // and once per index row.
    static bool build(const TerrainConfig& config,
                      const HeightField& heightField,
                      TerrainMeshData& outMesh,
                      std::string& errorMessage,
                      const std::atomic<bool>* cancelFlag = nullptr);
};

} // namespace terrain