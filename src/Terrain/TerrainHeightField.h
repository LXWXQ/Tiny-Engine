#pragma once

#include "Terrain/TerrainConfig.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace terrain
{

// A regular height grid. heights stores normalized [0, 1] values; world Y is
// reconstructed as baseHeight + heightScale * h.
struct HeightField
{
    uint32_t width = 0;
    uint32_t height = 0;

    float sizeX = 0.0f;
    float sizeZ = 0.0f;
    float sampleOriginX = 0.0f;
    float sampleOriginZ = 0.0f;
    float baseHeight = 0.0f;
    float heightScale = 0.0f;

    std::vector<float> heights;

    float minValue = 0.0f;
    float maxValue = 0.0f;

    // Diagnostic provenance; not serialized and never affects sampling.
    bool generatedOnGpu = false;

    // Optional cooperative cancellation flag is polled once per row.
    bool build(const TerrainConfig& config, std::string& errorMessage,
               const std::atomic<bool>* cancelFlag = nullptr);

    // Clamped grid access. Shared by mesh normal generation and tests.
    float at(uint32_t x, uint32_t z) const;

    // Bilinear sample in world XZ. Returns normalized height [0, 1].
    float sample(float worldX, float worldZ) const;

    float worldHeightAt(float worldX, float worldZ) const
    {
        return baseHeight + heightScale * sample(worldX, worldZ);
    }

    // Recompute minValue/maxValue after an external producer (GPU readback)
    // fills heights directly.
    void refreshMinMax() { computeMinMax(); }

private:
    void computeMinMax();
};

} // namespace terrain