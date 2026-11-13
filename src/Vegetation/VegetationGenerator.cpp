#include "Vegetation/VegetationGenerator.h"

#include <algorithm>
#include <cmath>

namespace vegetation
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

uint32_t mixBits(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

float hash01(uint32_t value)
{
    return static_cast<float>(mixBits(value) & 0x00ffffffU) / 16777215.0f;
}

uint32_t cellSeed(int seed, int ix, int iz, uint32_t salt)
{
    uint32_t h = static_cast<uint32_t>(seed) * 0x9e3779b9U + salt;
    h ^= static_cast<uint32_t>(ix) * 0x85ebca6bU;
    h ^= static_cast<uint32_t>(iz) * 0xc2b2ae35U;
    return mixBits(h);
}

Vertex vertex(const glm::vec3& position, const glm::vec3& normal,
              const glm::vec2& uv, float canopyMask, float shade)
{
    Vertex out{};
    out.position = position;
    out.normal = normal;
    out.uv = uv;
    // r = canopy mask (tree only), g = vertical shade used by the fragment shader.
    out.color = glm::vec3(canopyMask, shade, 0.0f);
    return out;
}

void addQuad(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal,
             float canopyMask, float shade)
{
    const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(vertex(a, normal, { 0.0f, 0.0f }, canopyMask, shade));
    mesh.vertices.push_back(vertex(b, normal, { 1.0f, 0.0f }, canopyMask, shade));
    mesh.vertices.push_back(vertex(c, normal, { 1.0f, 1.0f }, canopyMask, shade));
    mesh.vertices.push_back(vertex(d, normal, { 0.0f, 1.0f }, canopyMask, shade));
    mesh.indices.insert(mesh.indices.end(),
                        { baseIndex, baseIndex + 1, baseIndex + 2,
                          baseIndex, baseIndex + 2, baseIndex + 3 });
}

void addTriangle(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& normal, float canopyMask,
                 float shade)
{
    const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec2 uv0(0.0f, 0.0f);
    const glm::vec2 uv1(1.0f, 0.0f);
    const glm::vec2 uv2(0.5f, 1.0f);
    mesh.vertices.push_back(vertex(a, normal, uv0, canopyMask, shade));
    mesh.vertices.push_back(vertex(b, normal, uv1, canopyMask, shade));
    mesh.vertices.push_back(vertex(c, normal, uv2, canopyMask, shade));
    mesh.indices.insert(mesh.indices.end(),
                        { baseIndex, baseIndex + 1, baseIndex + 2 });
}

void addTrunk(MeshData& mesh, int sides, float bottomRadius, float topRadius,
              float height, float shade)
{
    for (int side = 0; side < sides; ++side)
    {
        const float a0 = kTwoPi * static_cast<float>(side) / static_cast<float>(sides);
        const float a1 = kTwoPi * static_cast<float>(side + 1) / static_cast<float>(sides);
        const glm::vec3 d0(std::cos(a0), 0.0f, std::sin(a0));
        const glm::vec3 d1(std::cos(a1), 0.0f, std::sin(a1));
        const glm::vec3 n = glm::normalize(d0 + d1);
        const glm::vec3 b0 = d0 * bottomRadius;
        const glm::vec3 b1 = d1 * bottomRadius;
        const glm::vec3 t0 = d0 * topRadius + glm::vec3(0.0f, height, 0.0f);
        const glm::vec3 t1 = d1 * topRadius + glm::vec3(0.0f, height, 0.0f);
        addQuad(mesh, b0, b1, t1, t0, n, 0.0f, shade);
    }
}

void addShadowDisc(MeshData& mesh, float radius, float y)
{
    // A cheap contact-shadow card. It is part of the tree mesh, so it follows
    // every instance and breaks up the "floating tree" look even when the
    // directional shadow map does not cover the tree.
    constexpr int kSides = 12;
    const uint32_t centerIndex = static_cast<uint32_t>(mesh.vertices.size());
    Vertex center = vertex(glm::vec3(0.0f, y, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                           glm::vec2(0.5f), 0.0f, 0.0f);
    center.color.b = 1.0f;
    mesh.vertices.push_back(center);
    for (int side = 0; side < kSides; ++side)
    {
        const float angle = kTwoPi * static_cast<float>(side) / static_cast<float>(kSides);
        const float cx = std::cos(angle);
        const float cz = std::sin(angle);
        Vertex rim = vertex(glm::vec3(cx * radius, y, cz * radius),
                            glm::vec3(0.0f, 1.0f, 0.0f),
                            glm::vec2(0.5f + 0.5f * cx, 0.5f + 0.5f * cz), 0.0f, 0.0f);
        rim.color.b = 1.0f;
        mesh.vertices.push_back(rim);
    }
    for (int side = 0; side < kSides; ++side)
    {
        const uint32_t a = centerIndex + 1u + static_cast<uint32_t>(side);
        const uint32_t b = centerIndex + 1u + static_cast<uint32_t>((side + 1) % kSides);
        mesh.indices.insert(mesh.indices.end(), { centerIndex, a, b });
    }
}

void buildGrassMesh(MeshData& grass)
{
    // Six crossed cards give the clump volume from every view direction.
    // Offset each card along its normal so the clump has a little footprint
    // instead of every blade growing from one exact point.
    for (int plane = 0; plane < 6; ++plane)
    {
        const float angle = kPi * static_cast<float>(plane) / 6.0f;
        const glm::vec3 axis(std::cos(angle), 0.0f, std::sin(angle));
        const glm::vec3 normal(axis.z, 0.0f, -axis.x);
        const glm::vec3 offset = normal * 0.11f;
        const glm::vec3 bl = axis * -0.5f + offset;
        const glm::vec3 br = axis * 0.5f + offset;
        addQuad(grass, bl, br, br + glm::vec3(0.0f, 1.0f, 0.0f),
                bl + glm::vec3(0.0f, 1.0f, 0.0f), normal, 0.0f, 0.0f);
    }
}

void addBlob(MeshData& mesh, const glm::vec3& center, const glm::vec3& radii,
             float shade)
{
    const size_t firstVertex = mesh.vertices.size();
    constexpr int kSides = 10;
    constexpr int kRings = 4;
    auto point = [&](int ring, int side)
    {
        const float theta = kPi * static_cast<float>(ring) / static_cast<float>(kRings);
        const float phi = kTwoPi * static_cast<float>(side) / static_cast<float>(kSides);
        const float ringRadius = std::sin(theta);
        return center + glm::vec3(std::cos(phi) * radii.x * ringRadius,
                                  std::cos(theta) * radii.y,
                                  std::sin(phi) * radii.z * ringRadius);
    };
    auto normalAt = [&](const glm::vec3& p)
    {
        const glm::vec3 q = p - center;
        return glm::normalize(glm::vec3(q.x / (radii.x * radii.x),
                                        q.y / (radii.y * radii.y),
                                        q.z / (radii.z * radii.z)));
    };

    const glm::vec3 top = center + glm::vec3(0.0f, radii.y, 0.0f);
    const glm::vec3 bottom = center + glm::vec3(0.0f, -radii.y, 0.0f);
    for (int side = 0; side < kSides; ++side)
    {
        const int next = (side + 1) % kSides;
        const glm::vec3 p0 = point(1, side);
        const glm::vec3 p1 = point(1, next);
        addTriangle(mesh, top, p0, p1, normalAt((top + p0 + p1) / 3.0f), 1.0f, shade);
    }
    for (int ring = 1; ring < kRings - 1; ++ring)
    {
        for (int side = 0; side < kSides; ++side)
        {
            const int next = (side + 1) % kSides;
            const glm::vec3 p0 = point(ring, side);
            const glm::vec3 p1 = point(ring, next);
            const glm::vec3 p2 = point(ring + 1, next);
            const glm::vec3 p3 = point(ring + 1, side);
            addQuad(mesh, p0, p1, p2, p3,
                    normalAt((p0 + p1 + p2 + p3) / 4.0f), 1.0f, shade);
        }
    }
    for (int side = 0; side < kSides; ++side)
    {
        const int next = (side + 1) % kSides;
        const glm::vec3 p0 = point(kRings - 1, side);
        const glm::vec3 p1 = point(kRings - 1, next);
        addTriangle(mesh, bottom, p1, p0, normalAt((bottom + p0 + p1) / 3.0f), 1.0f, shade);
    }

    // 每个三角形的顶点分别使用椭球法线，避免整面共享一个法线导致树冠
    // 像多面体球；共享位置的顶点法线一致，视觉上更接近叶团。
    for (size_t i = firstVertex; i < mesh.vertices.size(); ++i)
        mesh.vertices[i].normal = normalAt(mesh.vertices[i].position);
}

void buildConiferMesh(MeshData& conifer)
{
    addTrunk(conifer, 6, 0.045f, 0.028f, 0.56f, 0.05f);
    addShadowDisc(conifer, 0.26f, 0.012f);

    // Six overlapping, slightly irregular tiers. The jitter breaks the
    // perfectly circular silhouette that made the old trees read as cones.
    constexpr int kCanopySides = 10;
    struct CanopyLayer
    {
        float baseY;
        float radius;
        float topY;
    };
    const CanopyLayer layers[6] = {
        { 0.14f, 0.39f, 0.40f },
        { 0.26f, 0.35f, 0.56f },
        { 0.38f, 0.30f, 0.72f },
        { 0.50f, 0.25f, 0.86f },
        { 0.62f, 0.19f, 0.95f },
        { 0.72f, 0.13f, 1.00f },
    };
    const size_t canopyStart = conifer.vertices.size();
    for (int layerIndex = 0; layerIndex < 6; ++layerIndex)
    {
        const CanopyLayer& layer = layers[layerIndex];
        const float shade = std::min(1.0f, 0.20f + 0.16f * static_cast<float>(layerIndex));
        const glm::vec3 apex(std::cos(static_cast<float>(layerIndex) * 2.1f) * 0.025f,
                             layer.topY,
                             std::sin(static_cast<float>(layerIndex) * 1.7f) * 0.025f);
        for (int side = 0; side < kCanopySides; ++side)
        {
            const int next = (side + 1) % kCanopySides;
            const float a0 = kTwoPi * static_cast<float>(side) / static_cast<float>(kCanopySides);
            const float a1 = kTwoPi * static_cast<float>(next) / static_cast<float>(kCanopySides);
            const float wobble0 = 0.84f + 0.30f * hash01(static_cast<uint32_t>(
                layerIndex * 131 + side * 17 + 7));
            const float wobble1 = 0.84f + 0.30f * hash01(static_cast<uint32_t>(
                layerIndex * 131 + next * 17 + 7));
            const glm::vec3 p0(std::cos(a0) * layer.radius * wobble0, layer.baseY,
                               std::sin(a0) * layer.radius * wobble0);
            const glm::vec3 p1(std::cos(a1) * layer.radius * wobble1, layer.baseY,
                               std::sin(a1) * layer.radius * wobble1);
            glm::vec3 n = glm::normalize(glm::cross(p1 - p0, apex - p0));
            const glm::vec3 outward((p0.x + p1.x) * 0.5f, 0.0f,
                                    (p0.z + p1.z) * 0.5f);
            if (glm::dot(n, outward) < 0.0f)
                n = -n;
            addTriangle(conifer, p0, p1, apex, n, 1.0f, shade);
        }
    }

    // 锥层同样使用平滑的径向法线；否则每层只有 10 个大平面，近看很硬。
    for (size_t i = canopyStart; i < conifer.vertices.size(); ++i)
    {
        Vertex& v = conifer.vertices[i];
        if (v.color.b > 0.5f)
            continue;
        v.normal = glm::normalize(glm::vec3(v.position.x, 0.45f, v.position.z));
    }
}

void buildBroadleafMesh(MeshData& broadleaf)
{
    addTrunk(broadleaf, 6, 0.050f, 0.034f, 0.52f, 0.05f);
    addShadowDisc(broadleaf, 0.32f, 0.012f);

    // Seven overlapping low-poly crown clusters; this reads as a broadleaf
    // canopy instead of another stacked cone.
    struct Blob
    {
        glm::vec3 center;
        glm::vec3 radii;
        float shade;
    };
    const Blob blobs[7] = {
        { {  0.00f, 0.72f,  0.00f }, { 0.30f, 0.22f, 0.28f }, 0.58f },
        { {  0.21f, 0.76f,  0.06f }, { 0.23f, 0.18f, 0.21f }, 0.76f },
        { { -0.19f, 0.74f, -0.07f }, { 0.22f, 0.17f, 0.20f }, 0.66f },
        { {  0.03f, 0.90f,  0.02f }, { 0.21f, 0.17f, 0.20f }, 0.94f },
        { { -0.02f, 0.62f, -0.02f }, { 0.26f, 0.16f, 0.24f }, 0.44f },
        { {  0.18f, 0.66f,  0.16f }, { 0.17f, 0.12f, 0.16f }, 0.52f },
        { { -0.16f, 0.64f,  0.17f }, { 0.16f, 0.11f, 0.15f }, 0.48f },
    };
    for (const Blob& blob : blobs)
        addBlob(broadleaf, blob.center, blob.radii, blob.shade);
}

float sampleSlopeDegrees(float x, float z, float height,
                         const std::function<bool(float, float, float&)>& ground)
{
    constexpr float epsilon = 0.75f;
    float left = 0.0f, right = 0.0f, back = 0.0f, front = 0.0f;
    if (!ground(x - epsilon, z, left) || !ground(x + epsilon, z, right) ||
        !ground(x, z - epsilon, back) || !ground(x, z + epsilon, front))
        return 180.0f;
    const float slopeX = (right - left) / (2.0f * epsilon);
    const float slopeZ = (front - back) / (2.0f * epsilon);
    return glm::degrees(std::atan(std::sqrt(slopeX * slopeX + slopeZ * slopeZ)));
}
} // namespace

bool buildMeshes(MeshData& grass, MeshData& conifer, MeshData& broadleaf,
                 std::string* errorMessage)
{
    grass.vertices.clear();
    grass.indices.clear();
    conifer.vertices.clear();
    conifer.indices.clear();
    broadleaf.vertices.clear();
    broadleaf.indices.clear();
    buildGrassMesh(grass);
    buildConiferMesh(conifer);
    buildBroadleafMesh(broadleaf);
    if (grass.vertices.empty() || grass.indices.empty() ||
        conifer.vertices.empty() || conifer.indices.empty() ||
        broadleaf.vertices.empty() || broadleaf.indices.empty())
    {
        if (errorMessage)
            *errorMessage = "vegetation mesh generation produced empty geometry";
        return false;
    }
    return true;
}

Placement generate(const glm::vec3& camera, const Params& params,
                   const std::function<bool(float, float, float&)>& ground)
{
    Placement placement;
    if (!ground)
        return placement;

    const int seed = params.seed;
    const float waterLevel = params.waterEnabled ? params.waterLevel : -1.0e9f;

    const float grassRadius = std::max(params.grassDistance, 1.0f);
    const float grassDensity = std::max(params.grassDensity, 0.0f);
    if (grassDensity > 0.0001f)
    {
        const float spacing = 1.0f / std::sqrt(grassDensity);
        const int cells = static_cast<int>(std::ceil(grassRadius / spacing));
        const int cameraCellX = static_cast<int>(std::floor(camera.x / spacing));
        const int cameraCellZ = static_cast<int>(std::floor(camera.z / spacing));
        const float radiusSq = grassRadius * grassRadius;
        for (int dz = -cells; dz <= cells; ++dz)
        {
            for (int dx = -cells; dx <= cells; ++dx)
            {
                if (placement.grass.size() >= kMaxGrassInstances)
                    break;
                const int ix = cameraCellX + dx;
                const int iz = cameraCellZ + dz;
                const uint32_t h = cellSeed(seed, ix, iz, 0x51ed270bU);
                const float jitterX = hash01(h ^ 0x68bc21ebU);
                const float jitterZ = hash01(h ^ 0x02e5be93U);
                const float x = (static_cast<float>(ix) + jitterX) * spacing;
                const float z = (static_cast<float>(iz) + jitterZ) * spacing;
                const float distX = x - camera.x;
                const float distZ = z - camera.z;
                if (distX * distX + distZ * distZ > radiusSq)
                    continue;

                float height = 0.0f;
                if (!ground(x, z, height) || height < waterLevel + 0.05f)
                    continue;
                // 草不逐株做四向坡度采样：10k 株草会因此多出 40k 次地形
                // 查询，是生成卡顿的主要来源。改为整体下沉，允许草长在坡上；
                // 下沉量要能盖住坡面高差，否则卡片根部仍会悬空。
                height -= 0.11f;

                const float shape = hash01(h ^ 0x7f4a7c15U);
                const float scale = params.grassHeight * (0.72f + 0.56f * shape);
                const float tint = 0.78f + 0.34f * hash01(h ^ 0x94d049bbU);
                const float warmth = hash01(h ^ 0x2c1b3c6dU);

                Instance instance;
                instance.positionScale = glm::vec4(x, height - 0.015f, z, scale);
                instance.rotationTint = glm::vec4(
                    kTwoPi * hash01(h ^ 0x1b56c4e9U), 1.0f,
                    kTwoPi * hash01(h ^ 0x2545f491U), tint);
                instance.color = glm::vec4(
                    params.grassColor.r * tint * (0.92f + 0.14f * warmth),
                    params.grassColor.g * tint * (0.94f + 0.10f * (1.0f - warmth)),
                    params.grassColor.b * tint, 1.0f);
                placement.grass.push_back(instance);
            }
        }
    }

    const float treeRadius = std::max(params.treeDistance, 1.0f);
    const float treeDensity = std::max(params.treeDensity, 0.0f);
    if (treeDensity > 0.0001f)
    {
        const float spacing = std::sqrt(1000.0f / treeDensity);
        const int cells = static_cast<int>(std::ceil(treeRadius / spacing));
        const int cameraCellX = static_cast<int>(std::floor(camera.x / spacing));
        const int cameraCellZ = static_cast<int>(std::floor(camera.z / spacing));
        const float radiusSq = treeRadius * treeRadius;

        // Tree roots need a little more care than grass: sample the four
        // cardinal neighbours and use the lowest ground point, then sink the
        // root slightly. This keeps trunks planted on slopes instead of
        // hovering above downhill terrain.
        auto treeGround = [&](float x, float z, float& height) -> bool
        {
            if (!ground(x, z, height))
                return false;
            constexpr float r = 0.45f;
            const float offsets[4][2] = {
                { r, 0.0f }, { -r, 0.0f }, { 0.0f, r }, { 0.0f, -r }
            };
            for (const auto& offset : offsets)
            {
                float neighbour = 0.0f;
                if (ground(x + offset[0], z + offset[1], neighbour))
                    height = std::min(height, neighbour);
            }
            height -= 0.04f;
            return true;
        };

        for (int dz = -cells; dz <= cells; ++dz)
        {
            for (int dx = -cells; dx <= cells; ++dx)
            {
                if (placement.trees.size() >= kMaxTreeInstances)
                    break;
                const int ix = cameraCellX + dx;
                const int iz = cameraCellZ + dz;
                const uint32_t h = cellSeed(seed, ix, iz, 0x9e3779b9U);
                const float jitterX = hash01(h ^ 0x3c6ef372U);
                const float jitterZ = hash01(h ^ 0x5a17b1a3U);
                const float x = (static_cast<float>(ix) + jitterX) * spacing;
                const float z = (static_cast<float>(iz) + jitterZ) * spacing;
                const float distX = x - camera.x;
                const float distZ = z - camera.z;
                const float distSq = distX * distX + distZ * distZ;
                if (distSq > radiusSq || distSq < 9.0f)
                    continue;

                float height = 0.0f;
                if (!treeGround(x, z, height) || height < waterLevel + 0.10f)
                    continue;
                const float slope = sampleSlopeDegrees(x, z, height, ground);
                if (slope > 32.0f)
                    continue;

                const float shape = hash01(h ^ 0x7f4a7c15U);
                const float scale = params.treeHeight * (0.74f + 0.52f * shape);
                const float tint = 0.78f + 0.34f * hash01(h ^ 0x94d049bbU);
                const float warmth = hash01(h ^ 0x6a09e667U);
                const bool broadleaf = hash01(h ^ 0x3c6ef372U) < 0.38f;

                Instance instance;
                instance.positionScale = glm::vec4(x, height, z, scale);
                instance.rotationTint = glm::vec4(
                    kTwoPi * hash01(h ^ 0x1b56c4e9U), 0.88f + 0.24f * shape,
                    kTwoPi * hash01(h ^ 0x2545f491U), tint);
                instance.color = glm::vec4(
                    params.treeColor.r * tint * (0.90f + 0.16f * warmth),
                    params.treeColor.g * tint * (0.92f + 0.12f * (1.0f - warmth)),
                    params.treeColor.b * tint * 1.04f,
                    broadleaf ? 1.0f : 0.0f);
                placement.trees.push_back(instance);
            }
        }
    }

    return placement;
}

} // namespace vegetation
