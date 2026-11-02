#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace terrain
{

// Pure CPU procedural texture generation. The Vulkan-facing TerrainMaterial
// uploads albedo/normal/height/roughness RGBA buffers to the terrain set.
struct TerrainTextureSet
{
    static constexpr uint32_t kLayerCount = 4; // sand, grass, rock, snow

    uint32_t width = 0;
    uint32_t height = 0;

    std::array<std::vector<uint8_t>, kLayerCount> albedoRGBA;
    std::array<std::vector<uint8_t>, kLayerCount> normalRGBA;
    std::array<std::vector<uint8_t>, kLayerCount> heightRGBA;
    std::array<std::vector<uint8_t>, kLayerCount> roughnessRGBA;
};

// Generates deterministic, seamlessly tileable texture tiles. 256 is the
// production resolution; tests may request a smaller power-of-two size.
bool generateTerrainTextureSet(TerrainTextureSet& outTextures,
                               uint32_t resolution,
                               std::string& errorMessage);

} // namespace terrain