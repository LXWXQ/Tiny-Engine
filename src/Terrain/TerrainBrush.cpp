#include "Terrain/TerrainBrush.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>

namespace terrain
{

namespace
{
    float smoothFalloff(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float gridHeight(const HeightField& field, uint32_t x, uint32_t z)
    {
        return field.at(std::min(x, field.width - 1u),
                        std::min(z, field.height - 1u));
    }
} // namespace

bool applyTerrainBrush(HeightField& heightField,
                       const glm::mat4& localToWorld,
                       const TerrainBrushSettings& brush,
                       TerrainBrushResult& outResult,
                       std::string& errorMessage)
{
    outResult = {};
    errorMessage.clear();

    if (heightField.width < 2u || heightField.height < 2u ||
        heightField.heights.size() !=
            static_cast<size_t>(heightField.width) * heightField.height)
    {
        errorMessage = "terrain brush requires a valid height grid";
        return false;
    }
    if (!std::isfinite(brush.center.x) || !std::isfinite(brush.center.z) ||
        !std::isfinite(brush.radius) || !std::isfinite(brush.strength) ||
        brush.radius <= 0.0f)
    {
        errorMessage = "terrain brush center/radius/strength is invalid";
        return false;
    }

    const float stepX = heightField.sizeX / static_cast<float>(heightField.width - 1u);
    const float stepZ = heightField.sizeZ / static_cast<float>(heightField.height - 1u);
    if (!std::isfinite(stepX) || !std::isfinite(stepZ) ||
        stepX <= 0.0f || stepZ <= 0.0f)
    {
        errorMessage = "terrain brush heightfield spacing is invalid";
        return false;
    }

    const glm::mat4 worldToLocal = glm::inverse(localToWorld);
    const glm::vec3 centerLocal =
        glm::vec3(worldToLocal * glm::vec4(brush.center, 1.0f));

    const std::vector<float> source = heightField.heights;
    const float strength = std::max(0.0f, brush.strength);
    const float normalizedDelta = heightField.heightScale > 1.0e-6f
        ? strength / heightField.heightScale : 0.0f;
    const float blend = std::clamp(strength, 0.0f, 1.0f);
    const float flattenTarget = heightField.sample(centerLocal.x, centerLocal.z);

    const float halfX = heightField.sizeX * 0.5f;
    const float halfZ = heightField.sizeZ * 0.5f;
    const float radius2 = brush.radius * brush.radius;

    for (uint32_t z = 0; z < heightField.height; ++z)
    {
        const float localZ = -halfZ + static_cast<float>(z) * stepZ;
        for (uint32_t x = 0; x < heightField.width; ++x)
        {
            const float localX = -halfX + static_cast<float>(x) * stepX;
            const glm::vec3 world = glm::vec3(localToWorld *
                glm::vec4(localX, 0.0f, localZ, 1.0f));
            const float dx = world.x - brush.center.x;
            const float dz = world.z - brush.center.z;
            const float distance2 = dx * dx + dz * dz;
            if (distance2 >= radius2)
                continue;

            const float distance = std::sqrt(distance2);
            const float weight = smoothFalloff(1.0f - distance / brush.radius);
            if (weight <= 0.0f)
                continue;

            const size_t index = static_cast<size_t>(z) * heightField.width + x;
            const float current = source[index];
            float next = current;
            switch (brush.mode)
            {
            case TerrainBrushMode::Raise:
                next = current + normalizedDelta * weight;
                break;
            case TerrainBrushMode::Lower:
                next = current - normalizedDelta * weight;
                break;
            case TerrainBrushMode::Smooth:
            {
                float sum = 0.0f;
                float count = 0.0f;
                for (int nz = -1; nz <= 1; ++nz)
                {
                    for (int nx = -1; nx <= 1; ++nx)
                    {
                        const int sx = static_cast<int>(x) + nx;
                        const int sz = static_cast<int>(z) + nz;
                        if (sx < 0 || sz < 0 ||
                            sx >= static_cast<int>(heightField.width) ||
                            sz >= static_cast<int>(heightField.height))
                            continue;
                        sum += source[static_cast<size_t>(sz) * heightField.width +
                                      static_cast<size_t>(sx)];
                        count += 1.0f;
                    }
                }
                if (count > 0.0f)
                {
                    const float average = sum / count;
                    next = current + (average - current) * weight * blend;
                }
                break;
            }
            case TerrainBrushMode::Flatten:
            {
                // flattenTarget is normalized. Interpolate toward it using the
                // brush weight, so repeated strokes converge smoothly.
                next = current + (flattenTarget - current) * weight * blend;
                break;
            }
            default:
                break;
            }

            next = std::clamp(next, 0.0f, 1.0f);
            if (std::abs(next - current) > 1.0e-7f)
            {
                heightField.heights[index] = next;
                ++outResult.modifiedVertices;
            }
        }
    }

    heightField.refreshMinMax();
    outResult.minValue = heightField.minValue;
    outResult.maxValue = heightField.maxValue;
    return true;
}

} // namespace terrain
