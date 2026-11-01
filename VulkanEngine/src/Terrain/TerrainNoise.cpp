#include "Terrain/TerrainNoise.h"

#include <algorithm>
#include <cmath>

namespace terrain
{

uint32_t TerrainNoise::mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

uint32_t TerrainNoise::hash2D(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(x) * 0x9e3779b1u;
    h = mix32(h);
    h ^= static_cast<uint32_t>(z) * 0x85ebca77u;
    return mix32(h);
}

float TerrainNoise::fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float TerrainNoise::gradientDot(uint32_t hash, float x, float z)
{
    // Eight unit gradients. Using the low bits keeps the hash deterministic
    // and avoids any dependency on std::rand / platform-specific RNG.
    constexpr float kInvSqrt2 = 0.70710678118654752440f;
    static const float gradients[8][2] = {
        {  1.0f,  0.0f },
        { -1.0f,  0.0f },
        {  0.0f,  1.0f },
        {  0.0f, -1.0f },
        {  kInvSqrt2,  kInvSqrt2 },
        { -kInvSqrt2,  kInvSqrt2 },
        {  kInvSqrt2, -kInvSqrt2 },
        { -kInvSqrt2, -kInvSqrt2 },
    };
    const float* g = gradients[hash & 7u];
    return g[0] * x + g[1] * z;
}

float TerrainNoise::perlin(float x, float z, uint32_t seed)
{
    const float floorX = std::floor(x);
    const float floorZ = std::floor(z);
    const int32_t ix = static_cast<int32_t>(floorX);
    const int32_t iz = static_cast<int32_t>(floorZ);
    const float fx = x - floorX;
    const float fz = z - floorZ;

    const uint32_t h00 = hash2D(ix,     iz,     seed);
    const uint32_t h10 = hash2D(ix + 1, iz,     seed);
    const uint32_t h01 = hash2D(ix,     iz + 1, seed);
    const uint32_t h11 = hash2D(ix + 1, iz + 1, seed);

    const float n00 = gradientDot(h00, fx,        fz);
    const float n10 = gradientDot(h10, fx - 1.0f, fz);
    const float n01 = gradientDot(h01, fx,        fz - 1.0f);
    const float n11 = gradientDot(h11, fx - 1.0f, fz - 1.0f);

    const float u = fade(fx);
    const float v = fade(fz);
    const float nx0 = n00 + u * (n10 - n00);
    const float nx1 = n01 + u * (n11 - n01);
    const float value = nx0 + v * (nx1 - nx0);
    return std::clamp(value, -1.0f, 1.0f);
}

float TerrainNoise::fbm(float x, float z, const TerrainConfig& config)
{
    float sum = 0.0f;
    float amplitudeSum = 0.0f;
    float amplitude = 1.0f;
    float frequency = config.noiseFrequency;

    for (uint32_t octave = 0; octave < config.octaveCount; ++octave)
    {
        // Deriving an octave seed through the same integer mixer keeps the
        // seed local to the hash, as required by the terrain plan.
        const uint32_t octaveSeed = mix32(config.seed ^
            (0x9e3779b9u * (octave + 1u)));
        float n = perlin(x * frequency, z * frequency, octaveSeed);

        if (config.ridgeWeight > 0.0f)
        {
            const float ridge = std::pow(
                std::max(0.0f, 1.0f - std::fabs(n)), config.ridgeSharpness);
            n = n * (1.0f - config.ridgeWeight) + ridge * config.ridgeWeight;
        }

        // Optional high-frequency detail layer. It only modulates octaves
        // after the first so the base shape remains controllable.
        if (config.detailWeight > 0.0f && octave > 0)
        {
            const uint32_t detailSeed = mix32(octaveSeed ^ 0xa511e9b3u);
            const float detail = perlin(x * frequency * config.detailFrequency + 19.19f,
                                        z * frequency * config.detailFrequency - 7.73f,
                                        detailSeed);
            n *= 1.0f + config.detailWeight * 0.35f * detail;
            n = std::clamp(n, -1.0f, 1.0f);
        }

        sum += n * amplitude;
        amplitudeSum += amplitude;
        amplitude *= config.persistence;
        frequency *= config.lacunarity;
    }

    const float weightedAverage = sum / std::max(amplitudeSum, 1e-8f);
    float height = std::clamp(0.5f + 0.5f * weightedAverage, 0.0f, 1.0f);

    // Height curve: >1 deepens valleys and sharpens peaks; <1 creates plateaus.
    if (config.heightCurve != 1.0f)
        height = std::pow(height, config.heightCurve);

    return std::clamp(height, 0.0f, 1.0f);
}

} // namespace terrain