#include "Terrain/TerrainHeightField.h"
#include "Terrain/TerrainNoise.h"

#include <algorithm>
#include <cmath>

namespace terrain
{

namespace
{
float smoothstep01(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

bool HeightField::build(const TerrainConfig& config, std::string& errorMessage,
                        const std::atomic<bool>* cancelFlag)
{
    const auto cancelled = [cancelFlag]
    {
        return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
    };

    if (cancelled())
    {
        errorMessage = "terrain generation cancelled";
        return false;
    }
    if (!config.validate(errorMessage))
        return false;

    generatedOnGpu = false;
    width = config.resolutionX;
    height = config.resolutionZ;
    sizeX = config.sizeX;
    sizeZ = config.sizeZ;
    sampleOriginX = config.sampleOriginX;
    sampleOriginZ = config.sampleOriginZ;
    baseHeight = config.baseHeight;
    heightScale = config.heightScale;
    heights.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);

    const float stepX = sizeX / static_cast<float>(width - 1u);
    const float stepZ = sizeZ / static_cast<float>(height - 1u);

    for (uint32_t z = 0; z < height; ++z)
    {
        if (cancelled())
        {
            heights.clear();
            errorMessage = "terrain generation cancelled";
            return false;
        }
        const float localZ = -sizeZ * 0.5f + static_cast<float>(z) * stepZ;
        const float worldZ = sampleOriginZ + localZ;
        for (uint32_t x = 0; x < width; ++x)
        {
            const float localX = -sizeX * 0.5f + static_cast<float>(x) * stepX;
            const float worldX = sampleOriginX + localX;
            float normalized = TerrainNoise::fbm(worldX, worldZ, config);

            // Optional radial island mask. The inner radius stays untouched and
            // the mask falls smoothly to zero at the square terrain boundary.
            if (config.islandWeight > 0.0f)
            {
                const float nx = worldX / (sizeX * 0.5f);
                const float nz = worldZ / (sizeZ * 0.5f);
                const float radial = std::sqrt(nx * nx + nz * nz);
                const float islandMask = 1.0f -
                    smoothstep01(config.islandRadius, 1.0f, radial);
                normalized *= (1.0f - config.islandWeight) +
                              config.islandWeight * islandMask;
                normalized = std::clamp(normalized, 0.0f, 1.0f);
            }

            if (config.edgeFalloff > 0.0f)
            {
                const float nx = std::fabs(worldX) / (sizeX * 0.5f);
                const float nz = std::fabs(worldZ) / (sizeZ * 0.5f);
                const float edgeDistance = std::clamp(1.0f - std::max(nx, nz), 0.0f, 1.0f);
                const float mask = smoothstep01(0.0f, 0.35f, edgeDistance);
                normalized *= (1.0f - config.edgeFalloff) + config.edgeFalloff * mask;
                normalized = std::clamp(normalized, 0.0f, 1.0f);
            }

            heights[static_cast<size_t>(z) * width + x] = normalized;
        }
    }

    computeMinMax();
    errorMessage.clear();
    return true;
}

void HeightField::computeMinMax()
{
    if (heights.empty())
    {
        minValue = maxValue = 0.0f;
        return;
    }

    auto [minIt, maxIt] = std::minmax_element(heights.begin(), heights.end());
    minValue = *minIt;
    maxValue = *maxIt;
}

float HeightField::at(uint32_t x, uint32_t z) const
{
    if (heights.empty() || width == 0 || height == 0)
        return 0.0f;

    x = std::min(x, width - 1u);
    z = std::min(z, height - 1u);
    return heights[static_cast<size_t>(z) * width + x];
}

float HeightField::sample(float worldX, float worldZ) const
{
    if (width == 0 || height == 0 || heights.empty())
        return 0.0f;
    if (width == 1 || height == 1)
        return at(0, 0);

    const float u = std::clamp((worldX + sizeX * 0.5f) / sizeX, 0.0f, 1.0f) *
                    static_cast<float>(width - 1u);
    const float v = std::clamp((worldZ + sizeZ * 0.5f) / sizeZ, 0.0f, 1.0f) *
                    static_cast<float>(height - 1u);

    const uint32_t x0 = std::min(static_cast<uint32_t>(u), width - 1u);
    const uint32_t z0 = std::min(static_cast<uint32_t>(v), height - 1u);
    const uint32_t x1 = std::min(x0 + 1u, width - 1u);
    const uint32_t z1 = std::min(z0 + 1u, height - 1u);
    const float tx = u - static_cast<float>(x0);
    const float tz = v - static_cast<float>(z0);

    const float h00 = at(x0, z0);
    const float h10 = at(x1, z0);
    const float h01 = at(x0, z1);
    const float h11 = at(x1, z1);
    const float hx0 = h00 + tx * (h10 - h00);
    const float hx1 = h01 + tx * (h11 - h01);
    return hx0 + tz * (hx1 - hx0);
}

} // namespace terrain