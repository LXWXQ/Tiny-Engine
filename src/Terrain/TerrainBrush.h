#pragma once

#include "Terrain/TerrainHeightField.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace terrain
{

enum class TerrainBrushMode : uint32_t
{
    Raise = 0,
    Lower = 1,
    Smooth = 2,
    Flatten = 3
};

inline const char* terrainBrushModeLabel(TerrainBrushMode mode)
{
    switch (mode)
    {
    case TerrainBrushMode::Raise: return "Raise";
    case TerrainBrushMode::Lower: return "Lower";
    case TerrainBrushMode::Smooth: return "Smooth";
    case TerrainBrushMode::Flatten: return "Flatten";
    }
    return "Unknown";
}

struct TerrainBrushSettings
{
    // World-space brush center. Only XZ distance is used; the height comes from
    // the heightfield being edited.
    glm::vec3 center{ 0.0f };
    float radius = 4.0f;

    // Raise/Lower: world-space height delta at the brush center.
    // Smooth/Flatten: blend amount in [0, 1].
    float strength = 0.5f;

    TerrainBrushMode mode = TerrainBrushMode::Raise;
};

struct TerrainBrushResult
{
    uint32_t modifiedVertices = 0;
    float minValue = 0.0f;
    float maxValue = 0.0f;
};

// Applies a brush to an existing regular height grid. localToWorld is the
// terrain entity's current world transform; brush.center/radius are world-space.
// Heights remain normalized [0, 1] and minValue/maxValue are refreshed.
bool applyTerrainBrush(HeightField& heightField,
                       const glm::mat4& localToWorld,
                       const TerrainBrushSettings& brush,
                       TerrainBrushResult& outResult,
                       std::string& errorMessage);

} // namespace terrain
