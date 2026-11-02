#include "Terrain/TerrainSelfTest.h"
#include "Terrain/TerrainHeightField.h"
#include "Terrain/TerrainMeshBuilder.h"
#include "Terrain/TerrainTextureGenerator.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <string>

namespace terrain
{

namespace
{
bool approxEqual(float a, float b, float epsilon)
{
    return std::fabs(a - b) <= epsilon;
}

bool normalsAreValid(const TerrainMeshData& mesh)
{
    for (const TerrainVertex& vertex : mesh.vertices)
    {
        if (!std::isfinite(vertex.normal.x) || !std::isfinite(vertex.normal.y) ||
            !std::isfinite(vertex.normal.z))
            return false;

        const float length = glm::length(vertex.normal);
        if (!approxEqual(length, 1.0f, 1e-4f))
            return false;
    }
    return true;
}

bool collectBoundaryVertices(const TerrainMeshData& mesh,
                             const TerrainSubMesh& subMesh,
                             int fixedAxis, float boundary,
                             std::map<int64_t, TerrainVertex>& out)
{
    const int varyingAxis = (fixedAxis == 0) ? 2 : 0;
    const uint32_t end = std::min(subMesh.firstIndex + subMesh.surfaceIndexCount,
                                  static_cast<uint32_t>(mesh.indices.size()));
    for (uint32_t i = subMesh.firstIndex; i < end; ++i)
    {
        const uint32_t vertexIndex = mesh.indices[i];
        if (vertexIndex >= mesh.vertices.size())
            return false;
        const TerrainVertex& vertex = mesh.vertices[vertexIndex];
        if (std::fabs(vertex.position[fixedAxis] - boundary) > 1e-4f)
            continue;
        const int64_t key = static_cast<int64_t>(
            std::llround(static_cast<double>(vertex.position[varyingAxis]) * 10000.0));
        out.emplace(key, vertex);
    }
    return !out.empty();
}

bool boundaryVertexMatches(const TerrainVertex& a, const TerrainVertex& b)
{
    constexpr float kEpsilon = 1e-4f;
    return glm::length(a.position - b.position) <= kEpsilon &&
           glm::length(a.normal - b.normal) <= kEpsilon &&
           glm::length(a.uv - b.uv) <= kEpsilon &&
           glm::length(a.color - b.color) <= kEpsilon;
}

bool windingFacesPositiveY(const TerrainMeshData& mesh)
{
    if (mesh.indices.size() % 3u != 0u)
        return false;

    const auto checkRange = [&mesh](uint32_t first, uint32_t count) -> bool
    {
        const uint32_t end = std::min(first + count,
                                      static_cast<uint32_t>(mesh.indices.size()));
        for (uint32_t i = first; i + 2u < end; i += 3u)
        {
            const uint32_t i0 = mesh.indices[i + 0u];
            const uint32_t i1 = mesh.indices[i + 1u];
            const uint32_t i2 = mesh.indices[i + 2u];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() ||
                i2 >= mesh.vertices.size())
                return false;

            const glm::vec3& a = mesh.vertices[i0].position;
            const glm::vec3& b = mesh.vertices[i1].position;
            const glm::vec3& c = mesh.vertices[i2].position;
            const glm::vec3 crossValue = glm::cross(b - a, c - a);
            if (!(crossValue.y > 1e-8f))
            {
                std::cerr << "[Terrain] winding fail | tri=" << (i / 3u)
                          << " indices=" << i0 << "," << i1 << "," << i2
                          << " a=(" << a.x << "," << a.y << "," << a.z << ")"
                          << " b=(" << b.x << "," << b.y << "," << b.z << ")"
                          << " c=(" << c.x << "," << c.y << "," << c.z << ")"
                          << " crossY=" << crossValue.y << "\n";
                return false;
            }
        }
        return true;
    };

    if (mesh.subMeshes.empty())
        return checkRange(0u, static_cast<uint32_t>(mesh.indices.size()));
    for (const TerrainSubMesh& subMesh : mesh.subMeshes)
    {
        if (!checkRange(subMesh.firstIndex, subMesh.surfaceIndexCount))
            return false;
    }
    return true;
}
} // namespace

bool selfTest()
{
    bool ok = true;
    const auto fail = [&ok](const char* message)
    {
        std::cerr << "[Terrain] selfTest FAIL: " << message << "\n";
        ok = false;
    };

    TerrainConfig config;
    config.resolutionX = 65;
    config.resolutionZ = 65;

    std::string error;
    if (!config.validate(error))
        fail("default config rejected");

    // Determinism: the same params/seed must produce identical height values.
    HeightField first;
    HeightField second;
    if (!first.build(config, error) || !second.build(config, error))
        fail("height-field generation failed");
    else
    {
        if (first.heights.size() != second.heights.size())
            fail("deterministic height-field size mismatch");
        else
        {
            for (size_t i = 0; i < first.heights.size(); ++i)
            {
                if (first.heights[i] != second.heights[i])
                {
                    fail("same seed produced different heights");
                    break;
                }
            }
        }

        bool allFinite = true;
        bool allNormalized = true;
        for (float h : first.heights)
        {
            allFinite = allFinite && std::isfinite(h);
            allNormalized = allNormalized && h >= 0.0f && h <= 1.0f;
        }
        if (!allFinite) fail("height field contains NaN/Inf");
        if (!allNormalized) fail("height field outside [0, 1]");
    }

    // Cooperative cancellation must be observed before heavy work starts.
    {
        std::atomic<bool> cancel{ true };
        HeightField cancelledHeight;
        error.clear();
        if (cancelledHeight.build(config, error, &cancel))
            fail("cancelled height-field build was not rejected");
        if (error.find("cancelled") == std::string::npos)
            fail("cancelled height-field build missed cancellation error");

        TerrainMeshData cancelledMesh;
        error.clear();
        if (TerrainMeshBuilder::build(config, first, cancelledMesh, error, &cancel))
            fail("cancelled mesh build was not rejected");
        if (error.find("cancelled") == std::string::npos)
            fail("cancelled mesh build missed cancellation error");
    }

    TerrainConfig otherSeed = config;
    otherSeed.seed = config.seed + 1u;
    HeightField different;
    if (!different.build(otherSeed, error))
        fail("different-seed generation failed");
    else if (first.heights == different.heights)
        fail("different seeds produced identical height fields");

    // Reject all out-of-range values relevant to generation.
    {
        TerrainConfig invalid = config;
        invalid.resolutionX = 1;
        if (invalid.validate(error)) fail("resolutionX=1 was accepted");

        invalid = config;
        invalid.resolutionX = TerrainConfig::kMaxResolution + 1u;
        if (invalid.validate(error)) fail("resolutionX above max was accepted");

        invalid = config;
        invalid.sizeX = 0.0f;
        if (invalid.validate(error)) fail("sizeX=0 was accepted");

        invalid = config;
        invalid.noiseFrequency = std::numeric_limits<float>::quiet_NaN();
        if (invalid.validate(error)) fail("NaN frequency was accepted");

        invalid = config;
        invalid.octaveCount = 0;
        if (invalid.validate(error)) fail("octaveCount=0 was accepted");

        invalid = config;
        invalid.persistence = 1.0f;
        if (invalid.validate(error)) fail("persistence=1 was accepted");

        invalid = config;
        invalid.heightCurve = 5.0f;
        if (invalid.validate(error)) fail("heightCurve above max was accepted");

        invalid = config;
        invalid.islandRadius = 0.05f;
        if (invalid.validate(error)) fail("islandRadius below min was accepted");

        invalid = config;
        invalid.materialDetailStrength = 1.1f;
        if (invalid.validate(error)) fail("materialDetailStrength above 1 was accepted");

        invalid = config;
        invalid.surfaceStyle = static_cast<TerrainSurfaceStyle>(4);
        if (invalid.validate(error)) fail("surfaceStyle above 3 was accepted");

        invalid = config;
        invalid.chunkGridX = 0;
        if (invalid.validate(error)) fail("chunkGridX=0 was accepted");

        invalid = config;
        invalid.chunkGridZ = 5;
        if (invalid.validate(error)) fail("chunkGridZ above 4 was accepted");

        invalid = config;
        invalid.lodLevelCount = 0;
        if (invalid.validate(error)) fail("lodLevelCount=0 was accepted");

        invalid = config;
        invalid.lodLevelCount = 4;
        if (invalid.validate(error)) fail("lodLevelCount above 3 was accepted");

        invalid = config;
        invalid.lodDistanceScale = 0.1f;
        if (invalid.validate(error)) fail("lodDistanceScale below min was accepted");
    }

    // Stage 6 shape controls: height curve + island mask must stay bounded
    // and push the corners toward zero while leaving the centre raised.
    {
        TerrainConfig shapedConfig = config;
        shapedConfig.seed = 4242u;
        shapedConfig.heightCurve = 1.7f;
        shapedConfig.islandWeight = 1.0f;
        shapedConfig.islandRadius = 0.55f;
        shapedConfig.ridgeWeight = 0.7f;
        shapedConfig.ridgeSharpness = 2.5f;
        shapedConfig.detailFrequency = 4.0f;

        HeightField shaped;
        if (!shaped.build(shapedConfig, error))
        {
            fail("stage-6 shaped height field failed");
        }
        else
        {
            bool shapedFinite = true;
            for (float h : shaped.heights)
                shapedFinite = shapedFinite && std::isfinite(h) && h >= 0.0f && h <= 1.0f;
            if (!shapedFinite) fail("stage-6 shaped height outside [0, 1]");

            const float center = shaped.at(shaped.width / 2u, shaped.height / 2u);
            const float corner = shaped.at(0, 0);
            if (!(center > corner + 0.05f))
                fail("island mask did not suppress the terrain corner");
        }
    }

    // Shader-only material detail must not force geometry regeneration.
    {
        TerrainConfig materialOnly = config;
        materialOnly.materialDetailStrength = 0.2f;
        materialOnly.uvTiling = 2.5f;
        materialOnly.surfaceStyle = TerrainSurfaceStyle::Arid;
        materialOnly.lodDistanceScale = 1.8f;
        if (!materialOnly.geometryEquals(config) || materialOnly == config)
            fail("shader-only material/LOD-distance fields incorrectly affected geometry equality");

        TerrainConfig geometryOnly = config;
        geometryOnly.lodLevelCount = 2;
        if (geometryOnly.geometryEquals(config))
            fail("LOD level count did not affect geometry equality");
    }

    // Stage 6 material textures: deterministic, tileable-sized RGBA buffers.
    {
        TerrainTextureSet firstTextures;
        TerrainTextureSet secondTextures;
        if (!generateTerrainTextureSet(firstTextures, 64u, error) ||
            !generateTerrainTextureSet(secondTextures, 64u, error))
        {
            fail("terrain texture generation failed");
        }
        else
        {
            const size_t expectedBytes = static_cast<size_t>(64u) * 64u * 4u;
            bool textureSetValid = true;
            for (uint32_t layer = 0; layer < TerrainTextureSet::kLayerCount; ++layer)
            {
                textureSetValid = textureSetValid &&
                    firstTextures.albedoRGBA[layer].size() == expectedBytes &&
                    firstTextures.normalRGBA[layer].size() == expectedBytes &&
                    firstTextures.heightRGBA[layer].size() == expectedBytes &&
                    firstTextures.roughnessRGBA[layer].size() == expectedBytes &&
                    firstTextures.albedoRGBA[layer] == secondTextures.albedoRGBA[layer] &&
                    firstTextures.normalRGBA[layer] == secondTextures.normalRGBA[layer] &&
                    firstTextures.heightRGBA[layer] == secondTextures.heightRGBA[layer] &&
                    firstTextures.roughnessRGBA[layer] == secondTextures.roughnessRGBA[layer];
            }
            if (!textureSetValid)
                fail("terrain texture set is nondeterministic or wrong size");

            for (uint32_t layer = 0; layer < TerrainTextureSet::kLayerCount; ++layer)
            {
                const auto& albedo = firstTextures.albedoRGBA[layer];
                bool hasVariation = false;
                for (size_t i = 4; i < albedo.size(); i += 4)
                {
                    if (albedo[i] != albedo[0] || albedo[i + 1] != albedo[1] ||
                        albedo[i + 2] != albedo[2])
                    {
                        hasVariation = true;
                        break;
                    }
                }
                if (!hasVariation)
                    fail("terrain albedo texture is constant");

                const auto& heightTex = firstTextures.heightRGBA[layer];
                const auto& roughnessTex = firstTextures.roughnessRGBA[layer];
                bool heightVar = false;
                bool roughnessVar = false;
                for (size_t i = 4; i < expectedBytes; i += 4)
                {
                    heightVar = heightVar || heightTex[i] != heightTex[0];
                    roughnessVar = roughnessVar || roughnessTex[i] != roughnessTex[0];
                }
                if (!heightVar || !roughnessVar)
                    fail("terrain height/roughness texture is constant");
            }
        }
    }

    // Mesh topology and normals on a non-square grid.
    TerrainConfig meshConfig = config;
    meshConfig.resolutionX = 65;
    meshConfig.resolutionZ = 49;
    meshConfig.sizeX = 40.0f;
    meshConfig.sizeZ = 60.0f;
    meshConfig.chunkGridX = 3;
    meshConfig.chunkGridZ = 2;
    meshConfig.lodLevelCount = 3;
    meshConfig.lodDistanceScale = 1.0f;
    meshConfig.skirtsEnabled = false;

    HeightField meshHeight;
    TerrainMeshData mesh;
    if (!meshHeight.build(meshConfig, error) ||
        !TerrainMeshBuilder::build(meshConfig, meshHeight, mesh, error))
    {
        fail(("mesh generation failed: " + error).c_str());
    }
    else
    {
        if (mesh.vertices.empty() || mesh.indices.empty() ||
            mesh.indices.size() % 3u != 0u)
            fail("LOD mesh is empty or has non-triangle indices");

        const uint32_t logicalChunks = meshConfig.chunkGridX * meshConfig.chunkGridZ;
        const uint32_t expectedSubMeshes = logicalChunks * meshConfig.lodLevelCount;
        if (mesh.subMeshes.size() != expectedSubMeshes)
            fail("terrain chunk/LOD submesh count mismatch");

        uint32_t expectedFirstIndex = 0;
        uint32_t lodSubMeshCounts[3] = {};
        uint64_t lodSurfaceIndexCounts[3] = {};
        for (const TerrainSubMesh& subMesh : mesh.subMeshes)
        {
            if (subMesh.firstIndex != expectedFirstIndex ||
                subMesh.indexCount == 0u ||
                subMesh.surfaceIndexCount == 0u ||
                subMesh.surfaceIndexCount > subMesh.indexCount ||
                subMesh.surfaceIndexCount != subMesh.indexCount || // skirts disabled in this test
                subMesh.firstIndex + subMesh.indexCount > mesh.indices.size())
            {
                fail("terrain chunk/LOD index range mismatch");
                break;
            }
            if (subMesh.lod >= meshConfig.lodLevelCount ||
                subMesh.chunkX >= meshConfig.chunkGridX ||
                subMesh.chunkZ >= meshConfig.chunkGridZ)
            {
                fail("terrain chunk/LOD metadata invalid");
                break;
            }
            if (subMesh.boundsMin.x > subMesh.boundsMax.x ||
                subMesh.boundsMin.y > subMesh.boundsMax.y ||
                subMesh.boundsMin.z > subMesh.boundsMax.z)
            {
                fail("terrain chunk bounds invalid");
                break;
            }
            lodSubMeshCounts[subMesh.lod] += 1u;
            lodSurfaceIndexCounts[subMesh.lod] += subMesh.surfaceIndexCount;
            expectedFirstIndex += subMesh.indexCount;
        }
        if (expectedFirstIndex != mesh.indices.size())
            fail("terrain chunk ranges do not cover all indices");
        for (uint32_t lod = 0; lod < meshConfig.lodLevelCount; ++lod)
        {
            if (lodSubMeshCounts[lod] != logicalChunks)
                fail("terrain LOD submesh count mismatch");
        }
        if (!(lodSurfaceIndexCounts[0] > lodSurfaceIndexCounts[1] &&
              lodSurfaceIndexCounts[1] > lodSurfaceIndexCounts[2]))
            fail("terrain LOD index counts do not decrease");

        bool indicesValid = true;
        for (uint32_t index : mesh.indices)
            indicesValid = indicesValid && index < mesh.vertices.size();
        if (!indicesValid) fail("index out of range");
        if (!normalsAreValid(mesh)) fail("normal is not finite/unit length");
        if (!windingFacesPositiveY(mesh)) fail("triangle winding is not +Y CCW");
    }

    // B-02: every LOD exposes the full-resolution chunk boundary. Adjacent
    // chunks must match for every LOD pair, with skirts disabled.
    {
        meshConfig.localColorRange = false;
        TerrainMeshData stitched;
        if (!TerrainMeshBuilder::build(meshConfig, meshHeight, stitched, error))
        {
            fail("stitched LOD mesh generation failed");
        }
        else
        {
            const uint32_t lodCount = meshConfig.lodLevelCount;
            const auto subMeshAt = [&](uint32_t chunkX, uint32_t chunkZ, uint32_t lod)
                -> const TerrainSubMesh*
            {
                const size_t index =
                    (static_cast<size_t>(chunkZ) * meshConfig.chunkGridX + chunkX) *
                    lodCount + lod;
                return index < stitched.subMeshes.size()
                    ? &stitched.subMeshes[index] : nullptr;
            };

            const float stepX = meshConfig.sizeX /
                static_cast<float>(meshConfig.resolutionX - 1u);
            const float stepZ = meshConfig.sizeZ /
                static_cast<float>(meshConfig.resolutionZ - 1u);
            const uint32_t boundaryCellX =
                (meshConfig.resolutionX - 1u) / meshConfig.chunkGridX;
            const uint32_t chunkCellsZ =
                (meshConfig.resolutionZ - 1u) / meshConfig.chunkGridZ;
            const float sharedX = -meshConfig.sizeX * 0.5f +
                static_cast<float>(boundaryCellX) * stepX;
            const float sharedZ = -meshConfig.sizeZ * 0.5f +
                static_cast<float>(chunkCellsZ) * stepZ;

            bool seamsMatch = true;
            uint32_t seamLodPairsTested = 0;
            uint32_t seamEdgesTested = 0;
            for (uint32_t lodA = 0; lodA < lodCount && seamsMatch; ++lodA)
            {
                for (uint32_t lodB = 0; lodB < lodCount && seamsMatch; ++lodB)
                {
                    const TerrainSubMesh* right = subMeshAt(0, 0, lodA);
                    const TerrainSubMesh* left = subMeshAt(1, 0, lodB);
                    const TerrainSubMesh* bottom = subMeshAt(0, 0, lodA);
                    const TerrainSubMesh* top = subMeshAt(0, 1, lodB);
                    if (!right || !left || !bottom || !top)
                    {
                        seamsMatch = false;
                        break;
                    }

                    std::map<int64_t, TerrainVertex> rightEdge;
                    std::map<int64_t, TerrainVertex> leftEdge;
                    std::map<int64_t, TerrainVertex> bottomEdge;
                    std::map<int64_t, TerrainVertex> topEdge;
                    if (!collectBoundaryVertices(stitched, *right, 0, sharedX, rightEdge) ||
                        !collectBoundaryVertices(stitched, *left, 0, sharedX, leftEdge) ||
                        !collectBoundaryVertices(stitched, *bottom, 2, sharedZ, bottomEdge) ||
                        !collectBoundaryVertices(stitched, *top, 2, sharedZ, topEdge))
                    {
                        seamsMatch = false;
                        break;
                    }

                    if (rightEdge.size() != static_cast<size_t>(chunkCellsZ + 1u) ||
                        leftEdge.size() != rightEdge.size() ||
                        bottomEdge.size() != static_cast<size_t>(boundaryCellX + 1u) ||
                        topEdge.size() != bottomEdge.size())
                    {
                        seamsMatch = false;
                        break;
                    }

                    auto compareMaps = [&](const auto& a, const auto& b)
                    {
                        if (a.size() != b.size())
                            return false;
                        auto ia = a.begin();
                        auto ib = b.begin();
                        for (; ia != a.end(); ++ia, ++ib)
                        {
                            if (ia->first != ib->first ||
                                !boundaryVertexMatches(ia->second, ib->second))
                                return false;
                        }
                        return true;
                    };
                    seamsMatch = compareMaps(rightEdge, leftEdge) &&
                                 compareMaps(bottomEdge, topEdge);
                    if (seamsMatch)
                    {
                        ++seamLodPairsTested;
                        seamEdgesTested += 2u;
                    }
                }
            }
            if (!seamsMatch)
                fail("topological LOD boundary mismatch across adjacency/LOD pairs");
            else
                std::cout << "[Terrain] LOD stitching PASS | lodPairs="
                          << seamLodPairsTested << " edges=" << seamEdgesTested
                          << " skirts=off boundary=full-res\n";
        }
    }

    // A flat height field must produce exactly +Y normals.
    {
        TerrainConfig flatConfig = config;
        flatConfig.resolutionX = 7;
        flatConfig.resolutionZ = 5;
        flatConfig.heightScale = 10.0f;

        HeightField flatHeight;
        flatHeight.width = flatConfig.resolutionX;
        flatHeight.height = flatConfig.resolutionZ;
        flatHeight.sizeX = flatConfig.sizeX;
        flatHeight.sizeZ = flatConfig.sizeZ;
        flatHeight.baseHeight = flatConfig.baseHeight;
        flatHeight.heightScale = flatConfig.heightScale;
        flatHeight.minValue = 0.5f;
        flatHeight.maxValue = 0.5f;
        flatHeight.heights.assign(
            static_cast<size_t>(flatHeight.width) * flatHeight.height, 0.5f);

        TerrainMeshData flatMesh;
        if (!TerrainMeshBuilder::build(flatConfig, flatHeight, flatMesh, error))
            fail("flat mesh generation failed");
        else
        {
            for (const TerrainVertex& vertex : flatMesh.vertices)
            {
                if (!approxEqual(vertex.normal.x, 0.0f, 1e-6f) ||
                    !approxEqual(vertex.normal.y, 1.0f, 1e-6f) ||
                    !approxEqual(vertex.normal.z, 0.0f, 1e-6f))
                {
                    fail("flat height field produced a non-+Y normal");
                    break;
                }
            }
        }
    }

    // Non-square world spacing: h(x)=x/(resX-1). At the centre the X
    // derivative must use sizeX, not sizeZ or an index-space delta.
    {
        TerrainConfig slopeConfig = config;
        slopeConfig.resolutionX = 3;
        slopeConfig.resolutionZ = 3;
        slopeConfig.sizeX = 10.0f;
        slopeConfig.sizeZ = 30.0f;
        slopeConfig.heightScale = 2.0f;
        slopeConfig.chunkGridX = 1;
        slopeConfig.chunkGridZ = 1;
        slopeConfig.lodLevelCount = 1;

        HeightField slopeHeight;
        slopeHeight.width = 3;
        slopeHeight.height = 3;
        slopeHeight.sizeX = slopeConfig.sizeX;
        slopeHeight.sizeZ = slopeConfig.sizeZ;
        slopeHeight.baseHeight = slopeConfig.baseHeight;
        slopeHeight.heightScale = slopeConfig.heightScale;
        slopeHeight.minValue = 0.0f;
        slopeHeight.maxValue = 1.0f;
        slopeHeight.heights = {
            0.0f, 0.5f, 1.0f,
            0.0f, 0.5f, 1.0f,
            0.0f, 0.5f, 1.0f
        };

        TerrainMeshData slopeMesh;
        if (!TerrainMeshBuilder::build(slopeConfig, slopeHeight, slopeMesh, error))
            fail("non-square slope mesh generation failed");
        else
        {
            const TerrainVertex& center = slopeMesh.vertices[1u * 3u + 1u];
            const glm::vec3 expected = glm::normalize(glm::vec3(-0.2f, 1.0f, 0.0f));
            if (glm::length(center.normal - expected) > 1e-5f)
                fail("non-square derivative did not use world spacing");
        }
    }

    const uint32_t vertexCount = mesh.vertices.empty()
        ? 0u : static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t indexCount = mesh.indices.empty()
        ? 0u : static_cast<uint32_t>(mesh.indices.size());

    if (ok)
    {
        std::cout << "[Terrain] selfTest PASS"
                  << " | grid=" << config.resolutionX << "x" << config.resolutionZ
                  << " vertices=" << vertexCount
                  << " indices=" << indexCount
                  << " range=[" << first.minValue << ", " << first.maxValue << "]\n";
    }
    else
    {
        std::cout << "[Terrain] selfTest FAIL\n";
    }
    return ok;
}

} // namespace terrain