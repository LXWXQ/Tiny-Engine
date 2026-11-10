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
                     const glm::vec2& uv, float canopyMask)
{
    Vertex out{};
    out.position = position;
    out.normal = normal;
    out.uv = uv;
    out.color = glm::vec3(canopyMask);
    return out;
}

void addQuad(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal,
             float canopyMask)
{
    const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(vertex(a, normal, { 0.0f, 0.0f }, canopyMask));
    mesh.vertices.push_back(vertex(b, normal, { 1.0f, 0.0f }, canopyMask));
    mesh.vertices.push_back(vertex(c, normal, { 1.0f, 1.0f }, canopyMask));
    mesh.vertices.push_back(vertex(d, normal, { 0.0f, 1.0f }, canopyMask));
    mesh.indices.insert(mesh.indices.end(),
                        { baseIndex, baseIndex + 1, baseIndex + 2,
                          baseIndex, baseIndex + 2, baseIndex + 3 });
}

void addTriangle(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& normal, float canopyMask)
{
    const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec2 uv0(0.0f, 0.0f);
    const glm::vec2 uv1(1.0f, 0.0f);
    const glm::vec2 uv2(0.5f, 1.0f);
    mesh.vertices.push_back(vertex(a, normal, uv0, canopyMask));
    mesh.vertices.push_back(vertex(b, normal, uv1, canopyMask));
    mesh.vertices.push_back(vertex(c, normal, uv2, canopyMask));
    mesh.indices.insert(mesh.indices.end(),
                        { baseIndex, baseIndex + 1, baseIndex + 2 });
}

void buildGrassMesh(MeshData& grass)
{
    // 三张绕 Y 旋转 60 度的竖直卡片。高度 1，宽度 1，UV 的 y 从根到梢。
    for (int plane = 0; plane < 3; ++plane)
    {
        const float angle = kPi * static_cast<float>(plane) / 3.0f;
        const glm::vec3 axis(std::cos(angle), 0.0f, std::sin(angle));
        const glm::vec3 normal(axis.z, 0.0f, -axis.x);
        const glm::vec3 bl = axis * -0.5f;
        const glm::vec3 br = axis * 0.5f;
        addQuad(grass, bl, br, br + glm::vec3(0.0f, 1.0f, 0.0f),
                bl + glm::vec3(0.0f, 1.0f, 0.0f), normal, 0.0f);
    }
}

void buildTreeMesh(MeshData& tree)
{
    // 树干：六棱柱，底半径 0.045、顶半径 0.028，高度 0.56。
    constexpr int kTrunkSides = 6;
    constexpr float kTrunkBottom = 0.0f;
    constexpr float kTrunkTop = 0.56f;
    for (int side = 0; side < kTrunkSides; ++side)
    {
        const float a0 = kTwoPi * static_cast<float>(side) / kTrunkSides;
        const float a1 = kTwoPi * static_cast<float>(side + 1) / kTrunkSides;
        const glm::vec3 d0(std::cos(a0), 0.0f, std::sin(a0));
        const glm::vec3 d1(std::cos(a1), 0.0f, std::sin(a1));
        const glm::vec3 n = glm::normalize(d0 + d1);
        const glm::vec3 b0 = d0 * 0.045f + glm::vec3(0.0f, kTrunkBottom, 0.0f);
        const glm::vec3 b1 = d1 * 0.045f + glm::vec3(0.0f, kTrunkBottom, 0.0f);
        const glm::vec3 t0 = d0 * 0.028f + glm::vec3(0.0f, kTrunkTop, 0.0f);
        const glm::vec3 t1 = d1 * 0.028f + glm::vec3(0.0f, kTrunkTop, 0.0f);
        addQuad(tree, b0, b1, t1, t0, n, 0.0f);
    }

    // 三层锥形树冠，彼此重叠，轮廓像低模针叶树；每层 8 个侧面。
    constexpr int kCanopySides = 8;
    struct CanopyLayer
    {
        float baseY;
        float radius;
        float topY;
    };
    const CanopyLayer layers[3] = {
        { 0.30f, 0.30f, 0.62f },
        { 0.48f, 0.24f, 0.82f },
        { 0.64f, 0.17f, 1.00f },
    };
    for (const CanopyLayer& layer : layers)
    {
        const glm::vec3 apex(0.0f, layer.topY, 0.0f);
        for (int side = 0; side < kCanopySides; ++side)
        {
            const float a0 = kTwoPi * static_cast<float>(side) / kCanopySides;
            const float a1 = kTwoPi * static_cast<float>(side + 1) / kCanopySides;
            const glm::vec3 p0(std::cos(a0) * layer.radius, layer.baseY,
                               std::sin(a0) * layer.radius);
            const glm::vec3 p1(std::cos(a1) * layer.radius, layer.baseY,
                               std::sin(a1) * layer.radius);
            glm::vec3 n = glm::normalize(glm::cross(p1 - p0, apex - p0));
            const glm::vec3 outward((p0.x + p1.x) * 0.5f, 0.0f,
                                    (p0.z + p1.z) * 0.5f);
            if (glm::dot(n, outward) < 0.0f)
                n = -n;
            addTriangle(tree, p0, p1, apex, n, 1.0f);
        }
    }
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

bool buildMeshes(MeshData& grass, MeshData& tree, std::string* errorMessage)
{
    grass.vertices.clear();
    grass.indices.clear();
    tree.vertices.clear();
    tree.indices.clear();
    buildGrassMesh(grass);
    buildTreeMesh(tree);
    if (grass.vertices.empty() || grass.indices.empty() ||
        tree.vertices.empty() || tree.indices.empty())
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

    // 草：围绕相机的圆盘，抖动网格 + 每格随机偏移；内圈也保留，近处才有草。
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
                const float slope = sampleSlopeDegrees(x, z, height, ground);
                if (slope > 38.0f)
                    continue;

                const float shape = hash01(h ^ 0x7f4a7c15U);
                const float scale = params.grassHeight * (0.72f + 0.56f * shape);
                const float tint = 0.82f + 0.30f * hash01(h ^ 0x94d049bbU);

                Instance instance;
                instance.positionScale = glm::vec4(x, height, z, scale);
                instance.rotationTint = glm::vec4(
                    kTwoPi * hash01(h ^ 0x1b56c4e9U), 1.0f,
                    kTwoPi * hash01(h ^ 0x2545f491U), tint);
                instance.color = glm::vec4(
                    params.grassColor.r * tint, params.grassColor.g * tint,
                    params.grassColor.b * tint, 1.0f);
                placement.grass.push_back(instance);
            }
        }
    }

    // 树：每 1000 平方米 treeDensity 棵；比草稀疏，生成半径更大。
    const float treeRadius = std::max(params.treeDistance, 1.0f);
    const float treeDensity = std::max(params.treeDensity, 0.0f);
    if (treeDensity > 0.0001f)
    {
        const float spacing = std::sqrt(1000.0f / treeDensity);
        const int cells = static_cast<int>(std::ceil(treeRadius / spacing));
        const int cameraCellX = static_cast<int>(std::floor(camera.x / spacing));
        const int cameraCellZ = static_cast<int>(std::floor(camera.z / spacing));
        const float radiusSq = treeRadius * treeRadius;
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
                if (!ground(x, z, height) || height < waterLevel + 0.10f)
                    continue;
                const float slope = sampleSlopeDegrees(x, z, height, ground);
                if (slope > 32.0f)
                    continue;

                const float shape = hash01(h ^ 0x7f4a7c15U);
                const float scale = params.treeHeight * (0.78f + 0.48f * shape);
                const float tint = 0.86f + 0.24f * hash01(h ^ 0x94d049bbU);

                Instance instance;
                instance.positionScale = glm::vec4(x, height, z, scale);
                instance.rotationTint = glm::vec4(
                    kTwoPi * hash01(h ^ 0x1b56c4e9U), 0.92f + 0.16f * shape,
                    kTwoPi * hash01(h ^ 0x2545f491U), tint);
                instance.color = glm::vec4(
                    params.treeColor.r * tint, params.treeColor.g * tint,
                    params.treeColor.b * tint, 1.0f);
                placement.trees.push_back(instance);
            }
        }
    }

    return placement;
}

} // namespace vegetation
