#pragma once

#include <cstdint>
#include <cmath>
#include <limits>
#include <string>

namespace terrain
{

enum class TerrainSurfaceStyle : uint32_t
{
    Natural = 0,
    Arid = 1,
    Alpine = 2,
    Lush = 3
};

inline const char* terrainSurfaceStyleLabel(uint32_t style)
{
    switch (style)
    {
    case 1: return "Arid";
    case 2: return "Alpine";
    case 3: return "Lush";
    default: return "Natural";
    }
}

// Pure, serializable terrain parameters. This type deliberately has no
// dependency on Vulkan, EnTT or the editor so it can be unit-tested and later
// reused by an asynchronous/chunked generator.
struct TerrainConfig
{
    static constexpr uint32_t kMinResolution = 2;
    static constexpr uint32_t kMaxResolution = 257;

    uint32_t seed = 12345;

    uint32_t resolutionX = 257;
    uint32_t resolutionZ = 257;

    float sizeX = 100.0f;
    float sizeZ = 100.0f;

    // World-space center of the noise sampling domain. Mesh vertices remain
    // local XZ around zero; streaming chunk entities set this to their world
    // center so independently generated neighbors share one continuous field.
    float sampleOriginX = 0.0f;
    float sampleOriginZ = 0.0f;

    float baseHeight = 0.0f;
    float heightScale = 25.0f;

    float noiseFrequency = 0.01f;
    uint32_t octaveCount = 6;

    float persistence = 0.5f;
    float lacunarity = 2.0f;

    float ridgeWeight = 0.0f;
    float detailWeight = 0.25f;
    float edgeFalloff = 0.0f;

    // Stage 6 shape controls. Defaults preserve the MVP height function.
    float heightCurve = 1.0f;
    float islandWeight = 0.0f;
    float islandRadius = 0.8f;
    float ridgeSharpness = 1.0f;
    float detailFrequency = 2.37f;

    // Fragment-shader texture-layer normal/macro strength.
    float materialDetailStrength = 0.65f;

    float uvTiling = 1.0f;

    // Single terrain stretches vertex-color ramp to its local min/max. Streamed
    // chunks must use the same absolute [0,1] ramp or adjacent chunks get
    // different tints.
    bool localColorRange = true;

    // Diagnostic/A-B switch. Stitched LOD boundaries work without skirts, but
    // skirts remain enabled by default as a conservative fallback.
    bool skirtsEnabled = true;

    // Stage 7 chunk foundation. One Model owns chunkGridX*chunkGridZ contiguous
    // index ranges; each range gets its own local AABB for frustum culling.
    uint32_t chunkGridX = 2;
    uint32_t chunkGridZ = 2;

    // Stage 7 LOD: prebuild 1..3 index ranges per chunk. Distance scale only
    // changes runtime LOD selection and does not require a mesh rebuild.
    uint32_t lodLevelCount = 3;
    float lodDistanceScale = 1.0f;

    // Shader-only material variant. Changing it never requires mesh rebuild.
    TerrainSurfaceStyle surfaceStyle = TerrainSurfaceStyle::Natural;

    // B-08: bounded terrain material-set index (0..3). Set 0 is the default
    // external/procedural set; sets 1..3 are loaded on demand by TerrainMaterial.
    static constexpr uint32_t kMaxMaterialSets = 4;
    uint32_t materialSetIndex = 0;

    bool validate(std::string& errorMessage) const
    {
        errorMessage.clear();

        const auto fail = [&](const char* message)
        {
            errorMessage = message;
            return false;
        };

        if (resolutionX < kMinResolution || resolutionX > kMaxResolution)
            return fail("resolutionX must be in [2, 257]");
        if (resolutionZ < kMinResolution || resolutionZ > kMaxResolution)
            return fail("resolutionZ must be in [2, 257]");

        if (!std::isfinite(sizeX) || sizeX <= 0.0f || sizeX > 10000.0f)
            return fail("sizeX must be finite and in (0, 10000]");
        if (!std::isfinite(sizeZ) || sizeZ <= 0.0f || sizeZ > 10000.0f)
            return fail("sizeZ must be finite and in (0, 10000]");
        if (!std::isfinite(sampleOriginX) || !std::isfinite(sampleOriginZ) ||
            std::fabs(sampleOriginX) > 1.0e7f || std::fabs(sampleOriginZ) > 1.0e7f)
            return fail("sampleOriginX/Z must be finite and in [-1e7, 1e7]");

        if (!std::isfinite(baseHeight) || std::fabs(baseHeight) > 10000.0f)
            return fail("baseHeight must be finite and in [-10000, 10000]");
        if (!std::isfinite(heightScale) || heightScale < 0.0f || heightScale > 1000.0f)
            return fail("heightScale must be finite and in [0, 1000]");

        if (!std::isfinite(noiseFrequency) ||
            noiseFrequency < 0.0001f || noiseFrequency > 10.0f)
            return fail("noiseFrequency must be finite and in [0.0001, 10]");
        if (octaveCount < 1 || octaveCount > 8)
            return fail("octaveCount must be in [1, 8]");

        if (!std::isfinite(persistence) ||
            persistence < 0.05f || persistence > 0.95f)
            return fail("persistence must be finite and in [0.05, 0.95]");
        if (!std::isfinite(lacunarity) ||
            lacunarity < 1.1f || lacunarity > 4.0f)
            return fail("lacunarity must be finite and in [1.1, 4.0]");

        if (!std::isfinite(ridgeWeight) || ridgeWeight < 0.0f || ridgeWeight > 1.0f)
            return fail("ridgeWeight must be finite and in [0, 1]");
        if (!std::isfinite(detailWeight) || detailWeight < 0.0f || detailWeight > 1.0f)
            return fail("detailWeight must be finite and in [0, 1]");
        if (!std::isfinite(edgeFalloff) || edgeFalloff < 0.0f || edgeFalloff > 1.0f)
            return fail("edgeFalloff must be finite and in [0, 1]");

        if (!std::isfinite(heightCurve) || heightCurve < 0.25f || heightCurve > 4.0f)
            return fail("heightCurve must be finite and in [0.25, 4]");
        if (!std::isfinite(islandWeight) || islandWeight < 0.0f || islandWeight > 1.0f)
            return fail("islandWeight must be finite and in [0, 1]");
        if (!std::isfinite(islandRadius) || islandRadius < 0.1f || islandRadius > 1.0f)
            return fail("islandRadius must be finite and in [0.1, 1]");
        if (!std::isfinite(ridgeSharpness) || ridgeSharpness < 0.25f || ridgeSharpness > 4.0f)
            return fail("ridgeSharpness must be finite and in [0.25, 4]");
        if (!std::isfinite(detailFrequency) || detailFrequency < 1.0f || detailFrequency > 8.0f)
            return fail("detailFrequency must be finite and in [1, 8]");
        if (!std::isfinite(materialDetailStrength) || materialDetailStrength < 0.0f || materialDetailStrength > 1.0f)
            return fail("materialDetailStrength must be finite and in [0, 1]");

        if (!std::isfinite(uvTiling) || uvTiling < 0.01f || uvTiling > 100.0f)
            return fail("uvTiling must be finite and in [0.01, 100]");
        if (static_cast<uint32_t>(surfaceStyle) > 3u)
            return fail("surfaceStyle must be in [0, 3]");
        if (materialSetIndex >= kMaxMaterialSets)
            return fail("materialSetIndex must be in [0, 3]");
        if (chunkGridX < 1u || chunkGridX > 4u ||
            chunkGridZ < 1u || chunkGridZ > 4u)
            return fail("chunkGridX/Z must be in [1, 4]");
        if (lodLevelCount < 1u || lodLevelCount > 3u)
            return fail("lodLevelCount must be in [1, 3]");
        if (!std::isfinite(lodDistanceScale) ||
            lodDistanceScale < 0.25f || lodDistanceScale > 4.0f)
            return fail("lodDistanceScale must be finite and in [0.25, 4]");

        // Keep the checks explicit even though the resolution cap already makes
        // the current limits safe. If the cap changes, vertex/index counts and
        // the 32-bit index buffer must remain representable.
        const size_t vertexCount = static_cast<size_t>(resolutionX) * resolutionZ;
        const size_t quadCount = static_cast<size_t>(resolutionX - 1u) *
                                 static_cast<size_t>(resolutionZ - 1u);
        if (quadCount > (std::numeric_limits<size_t>::max() / 6u))
            return fail("terrain index count overflows size_t");
        const size_t indexCount = quadCount * 6u;
        const size_t maxCount = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
        if (vertexCount > maxCount || indexCount > maxCount)
            return fail("terrain mesh exceeds 32-bit vertex/index limits");

        return true;
    }

    // Fields that affect the generated geometry. materialDetailStrength,
    // uvTiling and surfaceStyle are shader-only, so changing them must not
    // require a mesh rebuild.
    bool geometryEquals(const TerrainConfig& other) const
    {
        return seed == other.seed &&
               resolutionX == other.resolutionX &&
               resolutionZ == other.resolutionZ &&
               sizeX == other.sizeX &&
               sizeZ == other.sizeZ &&
               sampleOriginX == other.sampleOriginX &&
               sampleOriginZ == other.sampleOriginZ &&
               baseHeight == other.baseHeight &&
               heightScale == other.heightScale &&
               noiseFrequency == other.noiseFrequency &&
               octaveCount == other.octaveCount &&
               persistence == other.persistence &&
               lacunarity == other.lacunarity &&
               ridgeWeight == other.ridgeWeight &&
               detailWeight == other.detailWeight &&
               edgeFalloff == other.edgeFalloff &&
               heightCurve == other.heightCurve &&
               islandWeight == other.islandWeight &&
               islandRadius == other.islandRadius &&
               ridgeSharpness == other.ridgeSharpness &&
               detailFrequency == other.detailFrequency &&
               localColorRange == other.localColorRange &&
               skirtsEnabled == other.skirtsEnabled &&
               chunkGridX == other.chunkGridX &&
               chunkGridZ == other.chunkGridZ &&
               lodLevelCount == other.lodLevelCount;
    }

    bool operator==(const TerrainConfig& other) const
    {
        return geometryEquals(other) &&
               materialDetailStrength == other.materialDetailStrength &&
               uvTiling == other.uvTiling &&
               lodDistanceScale == other.lodDistanceScale &&
               surfaceStyle == other.surfaceStyle &&
               materialSetIndex == other.materialSetIndex;
    }

    bool operator!=(const TerrainConfig& other) const { return !(*this == other); }
};

} // namespace terrain