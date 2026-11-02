#pragma once

#include "Terrain/TerrainConfig.h"

#include <cstdint>

namespace terrain
{

// Stateless 2D Perlin + fBm. All randomness is derived from the config seed;
// no rand()/global state is used, so identical inputs are bit-for-bit stable.
class TerrainNoise
{
public:
    static uint32_t hash2D(int32_t x, int32_t z, uint32_t seed);
    static float perlin(float x, float z, uint32_t seed);

    // Returns a normalized height in [0, 1]. The caller must validate config.
    static float fbm(float x, float z, const TerrainConfig& config);

private:
    static uint32_t mix32(uint32_t value);
    static float fade(float t);
    static float gradientDot(uint32_t hash, float x, float z);
};

} // namespace terrain