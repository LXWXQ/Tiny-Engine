#include "Terrain/TerrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace terrain
{

namespace
{
struct LinearColor
{
    float r;
    float g;
    float b;
};

float srgbChannelToLinear(uint32_t channel)
{
    const float value = static_cast<float>(channel) / 255.0f;
    return (value <= 0.04045f)
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

LinearColor linearColorFromHex(uint32_t rgb)
{
    return {
        srgbChannelToLinear((rgb >> 16) & 0xffu),
        srgbChannelToLinear((rgb >> 8) & 0xffu),
        srgbChannelToLinear(rgb & 0xffu)
    };
}

float smoothstep01(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

glm::vec3 terrainColor(float normalizedHeight, float normalY)
{
    const LinearColor sand = linearColorFromHex(0xc2b280u);
    const LinearColor grass = linearColorFromHex(0x4f7a3au);
    const LinearColor rock = linearColorFromHex(0x6b6560u);
    const LinearColor snow = linearColorFromHex(0xe8edf2u);

    // Color uses the field's local [min,max] normalized to 0..1. Raw fBm is
    // intentionally clamped to a narrow band around 0.5, so using absolute
    // height here would leave every default terrain grass-green. Geometry
    // still uses the unmodified normalized height; only the material ramp is
    // stretched to the generated range. These thresholds are MVP constants.
    const float sandWeight = 1.0f - smoothstep01(0.08f, 0.22f, normalizedHeight);
    glm::vec3 color = glm::mix(
        glm::vec3(grass.r, grass.g, grass.b),
        glm::vec3(sand.r, sand.g, sand.b),
        sandWeight);

    const float slope = std::clamp(1.0f - normalY, 0.0f, 1.0f);
    const float slopeRock = smoothstep01(0.24f, 0.60f, slope);
    const float highRock = smoothstep01(0.62f, 0.84f, normalizedHeight) * 0.55f;
    const float rockWeight = std::max(slopeRock, highRock);
    color = glm::mix(color, glm::vec3(rock.r, rock.g, rock.b), rockWeight);

    const float snowWeight = smoothstep01(0.80f, 0.94f, normalizedHeight) *
                             (1.0f - 0.6f * rockWeight);
    return glm::mix(color, glm::vec3(snow.r, snow.g, snow.b), snowWeight);
}
} // namespace

bool TerrainMeshBuilder::build(const TerrainConfig& config,
                               const HeightField& heightField,
                               TerrainMeshData& outMesh,
                               std::string& errorMessage,
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

    if (heightField.width != config.resolutionX ||
        heightField.height != config.resolutionZ ||
        heightField.heights.size() !=
            static_cast<size_t>(config.resolutionX) * static_cast<size_t>(config.resolutionZ))
    {
        errorMessage = "height field dimensions do not match terrain config";
        return false;
    }
    if (heightField.sizeX != config.sizeX ||
        heightField.sizeZ != config.sizeZ ||
        heightField.sampleOriginX != config.sampleOriginX ||
        heightField.sampleOriginZ != config.sampleOriginZ ||
        heightField.baseHeight != config.baseHeight ||
        heightField.heightScale != config.heightScale)
    {
        errorMessage = "height field world parameters do not match terrain config";
        return false;
    }

    outMesh.vertices.clear();
    outMesh.indices.clear();
    outMesh.subMeshes.clear();

    const float stepX = config.sizeX / static_cast<float>(config.resolutionX - 1u);
    const float stepZ = config.sizeZ / static_cast<float>(config.resolutionZ - 1u);
    const float colorRange = config.localColorRange
        ? heightField.maxValue - heightField.minValue : 1.0f;
    const float colorRangeInv = (colorRange > 1e-6f) ? (1.0f / colorRange) : 0.0f;
    const float colorBase = config.localColorRange ? heightField.minValue : 0.0f;

    const uint32_t chunkGridX = std::max(1u, std::min(config.chunkGridX,
                                                     config.resolutionX - 1u));
    const uint32_t chunkGridZ = std::max(1u, std::min(config.chunkGridZ,
                                                     config.resolutionZ - 1u));
    const uint32_t lodLevelCount = std::max(1u, std::min(config.lodLevelCount, 3u));
    const uint32_t chunkCount = chunkGridX * chunkGridZ;

    // Enough for all three LODs plus skirts in the common case. This is only
    // a reserve hint; no test depends on capacity.
    const size_t baseVertexCount = static_cast<size_t>(config.resolutionX) *
                                   static_cast<size_t>(config.resolutionZ);
    const size_t baseQuadCount = static_cast<size_t>(config.resolutionX - 1u) *
                                 static_cast<size_t>(config.resolutionZ - 1u);
    outMesh.vertices.reserve(baseVertexCount + baseVertexCount / 2u +
                             static_cast<size_t>(chunkCount) * lodLevelCount * 1024u);
    outMesh.indices.reserve(baseQuadCount * 6u * 2u +
                            static_cast<size_t>(chunkCount) * lodLevelCount * 2048u);

    const float skirtDepth = std::max(0.25f, config.heightScale * 0.08f);

    const auto appendSurfaceVertex = [&](float worldX, float worldZ) -> uint32_t
    {
        const float h = heightField.sample(worldX, worldZ);
        const float worldY = config.baseHeight + config.heightScale * h;

        const float hLeft = heightField.worldHeightAt(worldX - stepX, worldZ);
        const float hRight = heightField.worldHeightAt(worldX + stepX, worldZ);
        const float hBack = heightField.worldHeightAt(worldX, worldZ - stepZ);
        const float hFront = heightField.worldHeightAt(worldX, worldZ + stepZ);
        glm::vec3 normal(
            -(hRight - hLeft) / (2.0f * stepX),
            1.0f,
            -(hFront - hBack) / (2.0f * stepZ));
        const float lengthSquared = glm::dot(normal, normal);
        if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
        else
            normal /= std::sqrt(lengthSquared);

        const float normalizedHeight = (colorRange > 1e-6f)
            ? std::clamp((h - colorBase) * colorRangeInv, 0.0f, 1.0f)
            : 0.5f;

        TerrainVertex vertex;
        vertex.position = { worldX, worldY, worldZ };
        vertex.normal = normal;
        vertex.uv = {
            (worldX + config.sizeX * 0.5f) / config.sizeX * config.uvTiling,
            (worldZ + config.sizeZ * 0.5f) / config.sizeZ * config.uvTiling
        };
        vertex.color = terrainColor(normalizedHeight, normal.y);
        const uint32_t index = static_cast<uint32_t>(outMesh.vertices.size());
        outMesh.vertices.push_back(vertex);
        return index;
    };

    const auto appendSkirtVertex = [&](uint32_t topIndex) -> uint32_t
    {
        TerrainVertex vertex = outMesh.vertices[topIndex];
        vertex.position.y -= skirtDepth;
        vertex.color *= 0.72f;
        const uint32_t index = static_cast<uint32_t>(outMesh.vertices.size());
        outMesh.vertices.push_back(vertex);
        return index;
    };

    // Stitch two open polylines that start/end at the same corners. The outer
    // polyline is the full-resolution chunk boundary; the inner one is the
    // coarse LOD border. Advancing by normalized segment parameter keeps all
    // vertices and never creates a T-junction inside the transition band.
    const auto stitchPolylines = [&](const std::vector<uint32_t>& outer,
                                     const std::vector<uint32_t>& inner)
    {
        if (outer.size() < 2u || inner.size() < 2u)
            return;
        const size_t outerSegments = outer.size() - 1u;
        const size_t innerSegments = inner.size() - 1u;
        size_t oi = 0u;
        size_t ii = 0u;
        while (oi < outerSegments || ii < innerSegments)
        {
            const bool advanceOuter = oi < outerSegments &&
                (ii >= innerSegments ||
                 (oi + 1u) * innerSegments <= (ii + 1u) * outerSegments);
            if (advanceOuter)
            {
                outMesh.indices.push_back(outer[oi]);
                outMesh.indices.push_back(inner[ii]);
                outMesh.indices.push_back(outer[oi + 1u]);
                ++oi;
            }
            else
            {
                outMesh.indices.push_back(outer[oi]);
                outMesh.indices.push_back(inner[ii]);
                outMesh.indices.push_back(inner[ii + 1u]);
                ++ii;
            }
        }
    };

    const auto appendSkirtRing = [&](const std::vector<uint32_t>& ringTop)
    {
        std::vector<uint32_t> ringBottom;
        ringBottom.reserve(ringTop.size());
        for (uint32_t topIndex : ringTop)
            ringBottom.push_back(appendSkirtVertex(topIndex));

        for (size_t i = 0; i < ringTop.size(); ++i)
        {
            const size_t next = (i + 1u) % ringTop.size();
            outMesh.indices.push_back(ringTop[i]);
            outMesh.indices.push_back(ringBottom[i]);
            outMesh.indices.push_back(ringBottom[next]);
            outMesh.indices.push_back(ringTop[i]);
            outMesh.indices.push_back(ringBottom[next]);
            outMesh.indices.push_back(ringTop[next]);
        }
    };

    for (uint32_t chunkZ = 0; chunkZ < chunkGridZ; ++chunkZ)
    {
        const uint32_t cellZ0 = chunkZ * (config.resolutionZ - 1u) / chunkGridZ;
        const uint32_t cellZ1 = (chunkZ + 1u) * (config.resolutionZ - 1u) / chunkGridZ;
        const uint32_t chunkCellsZ = cellZ1 - cellZ0;

        for (uint32_t chunkX = 0; chunkX < chunkGridX; ++chunkX)
        {
            const uint32_t cellX0 = chunkX * (config.resolutionX - 1u) / chunkGridX;
            const uint32_t cellX1 = (chunkX + 1u) * (config.resolutionX - 1u) / chunkGridX;
            const uint32_t chunkCellsX = cellX1 - cellX0;

            for (uint32_t lod = 0; lod < lodLevelCount; ++lod)
            {
                if (cancelled())
                {
                    outMesh.vertices.clear();
                    outMesh.indices.clear();
                    outMesh.subMeshes.clear();
                    errorMessage = "terrain generation cancelled";
                    return false;
                }

                const uint32_t step = 1u << lod;
                const uint32_t firstVertex = static_cast<uint32_t>(outMesh.vertices.size());
                const uint32_t firstIndex = static_cast<uint32_t>(outMesh.indices.size());
                std::vector<uint32_t> ringTop;

                // Topological LOD rule: every chunk edge uses full heightfield
                // resolution, while the interior keeps the coarse LOD lattice.
                // This makes shared edges independent of each chunk's LOD.
                const bool canStitch =
                    chunkCellsX > 2u * step && chunkCellsZ > 2u * step;

                if (canStitch)
                {
                    const uint32_t innerCellX0 = cellX0 + step;
                    const uint32_t innerCellX1 = cellX1 - step;
                    const uint32_t innerCellZ0 = cellZ0 + step;
                    const uint32_t innerCellZ1 = cellZ1 - step;
                    const uint32_t innerSpanX = innerCellX1 - innerCellX0;
                    const uint32_t innerSpanZ = innerCellZ1 - innerCellZ0;
                    const uint32_t innerSegX = std::clamp<uint32_t>(
                        (innerSpanX + step / 2u) / step, 1u, innerSpanX);
                    const uint32_t innerSegZ = std::clamp<uint32_t>(
                        (innerSpanZ + step / 2u) / step, 1u, innerSpanZ);
                    const uint32_t innerRowVertexCount = innerSegX + 1u;

                    std::vector<uint32_t> innerIndices(
                        static_cast<size_t>(innerRowVertexCount) *
                        static_cast<size_t>(innerSegZ + 1u));
                    for (uint32_t iz = 0; iz <= innerSegZ; ++iz)
                    {
                        if (cancelled())
                        {
                            outMesh.vertices.clear();
                            outMesh.indices.clear();
                            outMesh.subMeshes.clear();
                            errorMessage = "terrain generation cancelled";
                            return false;
                        }
                        const uint32_t cellZ = innerCellZ0 +
                            iz * innerSpanZ / innerSegZ;
                        const float worldZ = -config.sizeZ * 0.5f +
                                             static_cast<float>(cellZ) * stepZ;
                        for (uint32_t ix = 0; ix <= innerSegX; ++ix)
                        {
                            const uint32_t cellX = innerCellX0 +
                                ix * innerSpanX / innerSegX;
                            const float worldX = -config.sizeX * 0.5f +
                                                 static_cast<float>(cellX) * stepX;
                            innerIndices[static_cast<size_t>(iz) * innerRowVertexCount + ix] =
                                appendSurfaceVertex(worldX, worldZ);
                        }
                    }

                    // Coarse interior quads.
                    for (uint32_t iz = 0; iz < innerSegZ; ++iz)
                    {
                        for (uint32_t ix = 0; ix < innerSegX; ++ix)
                        {
                            const uint32_t a = innerIndices[
                                static_cast<size_t>(iz) * innerRowVertexCount + ix];
                            const uint32_t b = innerIndices[
                                static_cast<size_t>(iz + 1u) * innerRowVertexCount + ix];
                            const uint32_t c = innerIndices[
                                static_cast<size_t>(iz) * innerRowVertexCount + ix + 1u];
                            const uint32_t d = innerIndices[
                                static_cast<size_t>(iz + 1u) * innerRowVertexCount + ix + 1u];
                            outMesh.indices.push_back(a);
                            outMesh.indices.push_back(b);
                            outMesh.indices.push_back(c);
                            outMesh.indices.push_back(b);
                            outMesh.indices.push_back(d);
                            outMesh.indices.push_back(c);
                        }
                    }

                    // Full-resolution outer ring, shared as one welded loop.
                    std::vector<uint32_t> outerRing;
                    outerRing.reserve(static_cast<size_t>(chunkCellsX + chunkCellsZ) * 2u);
                    const auto appendOuter = [&](uint32_t cellX, uint32_t cellZ)
                    {
                        const float worldX = -config.sizeX * 0.5f +
                                             static_cast<float>(cellX) * stepX;
                        const float worldZ = -config.sizeZ * 0.5f +
                                             static_cast<float>(cellZ) * stepZ;
                        outerRing.push_back(appendSurfaceVertex(worldX, worldZ));
                    };
                    for (uint32_t ix = 0; ix <= chunkCellsX; ++ix)
                        appendOuter(cellX0 + ix, cellZ0);
                    for (uint32_t iz = 1; iz <= chunkCellsZ; ++iz)
                        appendOuter(cellX1, cellZ0 + iz);
                    for (uint32_t k = 1; k <= chunkCellsX; ++k)
                        appendOuter(cellX0 + chunkCellsX - k, cellZ1);
                    for (uint32_t k = 1; k < chunkCellsZ; ++k)
                        appendOuter(cellX0, cellZ0 + chunkCellsZ - k);

                    // Four transition bands: outer full-res side -> inner coarse side.
                    const size_t nX = chunkCellsX;
                    const size_t nZ = chunkCellsZ;
                    std::vector<uint32_t> outerTop(
                        outerRing.begin(), outerRing.begin() + nX + 1u);
                    std::vector<uint32_t> outerRight(
                        outerRing.begin() + nX, outerRing.begin() + nX + nZ + 1u);
                    std::vector<uint32_t> outerBottom(
                        outerRing.begin() + nX + nZ,
                        outerRing.begin() + nX + nZ + nX + 1u);
                    std::vector<uint32_t> outerLeft(
                        outerRing.begin() + nX + nZ + nX, outerRing.end());
                    outerLeft.push_back(outerRing[0]);

                    std::vector<uint32_t> innerTop;
                    innerTop.reserve(innerSegX + 1u);
                    for (uint32_t ix = 0; ix <= innerSegX; ++ix)
                        innerTop.push_back(innerIndices[ix]);
                    std::vector<uint32_t> innerRight;
                    innerRight.reserve(innerSegZ + 1u);
                    for (uint32_t iz = 0; iz <= innerSegZ; ++iz)
                        innerRight.push_back(innerIndices[
                            static_cast<size_t>(iz) * innerRowVertexCount + innerSegX]);
                    std::vector<uint32_t> innerBottom;
                    innerBottom.reserve(innerSegX + 1u);
                    const size_t innerLastRow =
                        static_cast<size_t>(innerSegZ) * innerRowVertexCount;
                    for (uint32_t ix = innerSegX + 1u; ix-- > 0u;)
                        innerBottom.push_back(innerIndices[innerLastRow + ix]);
                    std::vector<uint32_t> innerLeft;
                    innerLeft.reserve(innerSegZ + 1u);
                    for (uint32_t iz = innerSegZ + 1u; iz-- > 0u;)
                        innerLeft.push_back(innerIndices[
                            static_cast<size_t>(iz) * innerRowVertexCount]);

                    stitchPolylines(outerTop, innerTop);
                    stitchPolylines(outerRight, innerRight);
                    stitchPolylines(outerBottom, innerBottom);
                    stitchPolylines(outerLeft, innerLeft);
                    ringTop = std::move(outerRing);
                }
                else
                {
                    // Very small chunk/LOD: use the full-resolution grid so the
                    // boundary contract still matches neighboring chunks.
                    const uint32_t segmentsX = std::max(1u, chunkCellsX);
                    const uint32_t segmentsZ = std::max(1u, chunkCellsZ);
                    const uint32_t rowVertexCount = segmentsX + 1u;
                    std::vector<uint32_t> localIndices(
                        static_cast<size_t>(rowVertexCount) *
                        static_cast<size_t>(segmentsZ + 1u));
                    for (uint32_t iz = 0; iz <= segmentsZ; ++iz)
                    {
                        if (cancelled())
                        {
                            outMesh.vertices.clear();
                            outMesh.indices.clear();
                            outMesh.subMeshes.clear();
                            errorMessage = "terrain generation cancelled";
                            return false;
                        }
                        const uint32_t cellZ = cellZ0 + iz * chunkCellsZ / segmentsZ;
                        const float worldZ = -config.sizeZ * 0.5f +
                                             static_cast<float>(cellZ) * stepZ;
                        for (uint32_t ix = 0; ix <= segmentsX; ++ix)
                        {
                            const uint32_t cellX = cellX0 + ix * chunkCellsX / segmentsX;
                            const float worldX = -config.sizeX * 0.5f +
                                                 static_cast<float>(cellX) * stepX;
                            localIndices[static_cast<size_t>(iz) * rowVertexCount + ix] =
                                appendSurfaceVertex(worldX, worldZ);
                        }
                    }
                    for (uint32_t iz = 0; iz < segmentsZ; ++iz)
                    {
                        for (uint32_t ix = 0; ix < segmentsX; ++ix)
                        {
                            const uint32_t a = localIndices[
                                static_cast<size_t>(iz) * rowVertexCount + ix];
                            const uint32_t b = localIndices[
                                static_cast<size_t>(iz + 1u) * rowVertexCount + ix];
                            const uint32_t c = localIndices[
                                static_cast<size_t>(iz) * rowVertexCount + ix + 1u];
                            const uint32_t d = localIndices[
                                static_cast<size_t>(iz + 1u) * rowVertexCount + ix + 1u];
                            outMesh.indices.push_back(a);
                            outMesh.indices.push_back(b);
                            outMesh.indices.push_back(c);
                            outMesh.indices.push_back(b);
                            outMesh.indices.push_back(d);
                            outMesh.indices.push_back(c);
                        }
                    }

                    ringTop.reserve(
                        static_cast<size_t>(segmentsX + segmentsZ) * 2u + 4u);
                    for (uint32_t ix = 0; ix <= segmentsX; ++ix)
                        ringTop.push_back(localIndices[ix]);
                    for (uint32_t iz = 1; iz <= segmentsZ; ++iz)
                        ringTop.push_back(localIndices[
                            static_cast<size_t>(iz) * rowVertexCount + segmentsX]);
                    for (uint32_t ix = segmentsX; ix-- > 0u;)
                        ringTop.push_back(localIndices[
                            static_cast<size_t>(segmentsZ) * rowVertexCount + ix]);
                    for (uint32_t iz = segmentsZ; iz-- > 1u;)
                        ringTop.push_back(localIndices[
                            static_cast<size_t>(iz) * rowVertexCount]);
                }

                const uint32_t surfaceEndIndex =
                    static_cast<uint32_t>(outMesh.indices.size());

                if (config.skirtsEnabled)
                    appendSkirtRing(ringTop);

                TerrainSubMesh subMesh;
                subMesh.firstIndex = firstIndex;
                subMesh.indexCount = static_cast<uint32_t>(outMesh.indices.size()) - firstIndex;
                subMesh.chunkX = chunkX;
                subMesh.chunkZ = chunkZ;
                subMesh.lod = lod;
                subMesh.surfaceIndexCount = surfaceEndIndex - firstIndex;
                subMesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
                subMesh.boundsMax = glm::vec3(-std::numeric_limits<float>::max());
                for (uint32_t i = firstVertex; i < outMesh.vertices.size(); ++i)
                {
                    subMesh.boundsMin = glm::min(subMesh.boundsMin,
                                                 outMesh.vertices[i].position);
                    subMesh.boundsMax = glm::max(subMesh.boundsMax,
                                                 outMesh.vertices[i].position);
                }
                outMesh.subMeshes.push_back(subMesh);
            }
        }
    }

    errorMessage.clear();
    return true;
}

} // namespace terrain