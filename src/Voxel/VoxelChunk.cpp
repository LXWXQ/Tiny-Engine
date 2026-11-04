#include "Voxel/VoxelChunk.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace voxel
{

namespace
{

uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// [0,1) 的栅格哈希。
float gridValue(int32_t x, int32_t z, uint64_t seed)
{
    const uint64_t h = splitmix64(static_cast<uint64_t>(static_cast<int64_t>(x)) * 0x9E3779B1ull ^
                                  (static_cast<uint64_t>(static_cast<int64_t>(z)) << 32) ^ seed);
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// 双线性插值的值噪声；freq = 每多少格一个控制点。
float valueNoise(float x, float z, float freq, uint64_t seed)
{
    const float fx = x / freq;
    const float fz = z / freq;
    const int32_t x0 = static_cast<int32_t>(std::floor(fx));
    const int32_t z0 = static_cast<int32_t>(std::floor(fz));
    float tx = fx - static_cast<float>(x0);
    float tz = fz - static_cast<float>(z0);
    tx = tx * tx * (3.0f - 2.0f * tx);   // smoothstep
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float v00 = gridValue(x0, z0, seed);
    const float v10 = gridValue(x0 + 1, z0, seed);
    const float v01 = gridValue(x0, z0 + 1, seed);
    const float v11 = gridValue(x0 + 1, z0 + 1, seed);
    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * tz;
}

// 世界坐标 -> 地表高度（chunk 局部 y）。相邻 chunk 用同一个函数，所以边界天然连续。
int32_t surfaceHeight(int32_t worldX, int32_t worldZ, int32_t baseY, uint64_t seed)
{
    const float fx = static_cast<float>(worldX);
    const float fz = static_cast<float>(worldZ);
    const float hills = valueNoise(fx, fz, 24.0f, seed) * 7.0f;
    const float detail = valueNoise(fx, fz, 7.0f, seed ^ 0xA5A5A5A5ull) * 2.0f;
    const float h = 6.0f + hills + detail;   // 6..15
    return baseY + static_cast<int32_t>(h);
}

struct FaceDef
{
    int dx, dy, dz;
    glm::vec3 normal;
    glm::vec3 corners[4];   // 单位立方体上的 4 个角（0/1），逆时针
    float shade;
};

const FaceDef kFaces[6] = {
    // +Y 顶
    { 0, 1, 0, { 0, 1, 0 }, { {0,1,0}, {1,1,0}, {1,1,1}, {0,1,1} }, 1.00f },
    // -Y 底
    { 0, -1, 0, { 0, -1, 0 }, { {0,0,0}, {0,0,1}, {1,0,1}, {1,0,0} }, 0.55f },
    // +X
    { 1, 0, 0, { 1, 0, 0 }, { {1,0,0}, {1,0,1}, {1,1,1}, {1,1,0} }, 0.82f },
    // -X
    { -1, 0, 0, { -1, 0, 0 }, { {0,0,0}, {0,1,0}, {0,1,1}, {0,0,1} }, 0.74f },
    // +Z
    { 0, 0, 1, { 0, 0, 1 }, { {0,0,1}, {0,1,1}, {1,1,1}, {1,0,1} }, 0.68f },
    // -Z
    { 0, 0, -1, { 0, 0, -1 }, { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0} }, 0.90f },
};

} // namespace

void Chunk::generate(uint64_t seed, int32_t chunkX, int32_t chunkZ, int32_t baseY)
{
    blocks.fill(static_cast<uint8_t>(BlockId::Air));
    const int32_t originX = chunkX * kSize;
    const int32_t originZ = chunkZ * kSize;

    for (int32_t z = 0; z < kSize; ++z)
    {
        for (int32_t x = 0; x < kSize; ++x)
        {
            const int32_t height = surfaceHeight(originX + x, originZ + z, baseY, seed);
            const int32_t localTop = height - baseY;
            for (int32_t y = 0; y < kSize; ++y)
            {
                if (y > localTop)
                    break;
                uint8_t id = static_cast<uint8_t>(BlockId::Stone);
                if (y == localTop)
                    id = static_cast<uint8_t>(BlockId::Grass);
                else if (y >= localTop - 3)
                    id = static_cast<uint8_t>(BlockId::Dirt);
                set(x, y, z, id);
            }
        }
    }
}

uint32_t Chunk::solidCount() const
{
    uint32_t count = 0;
    for (uint8_t id : blocks)
    {
        if (!isAir(id))
            ++count;
    }
    return count;
}

bool buildMesh(const Chunk& chunk,
               terrain::TerrainMeshData& outMesh,
               MeshStats* outStats)
{
    outMesh.vertices.clear();
    outMesh.indices.clear();
    outMesh.subMeshes.clear();
    if (outStats)
        *outStats = MeshStats{};

    outMesh.subMeshes.push_back(terrain::TerrainSubMesh{});
    terrain::TerrainSubMesh& sub = outMesh.subMeshes.back();
    sub.firstIndex = 0;

    glm::vec3 boundsMin{ 1e30f };
    glm::vec3 boundsMax{ -1e30f };

    for (int32_t y = 0; y < Chunk::kSize; ++y)
    {
        for (int32_t z = 0; z < Chunk::kSize; ++z)
        {
            for (int32_t x = 0; x < Chunk::kSize; ++x)
            {
                const uint8_t id = chunk.get(x, y, z);
                if (isAir(id))
                    continue;
                const BlockInfo& info = blockInfo(id);

                for (const FaceDef& face : kFaces)
                {
                    const uint8_t neighbour = chunk.get(x + face.dx, y + face.dy, z + face.dz);
                    // 邻格不透明才挡面；玻璃/树叶不透明=false，所以它们会互相出面。
                    if (isOpaque(neighbour))
                        continue;

                    const uint32_t baseVertex = static_cast<uint32_t>(outMesh.vertices.size());
                    const glm::vec3 color{ info.r * face.shade,
                                           info.g * face.shade,
                                           info.b * face.shade };
                    for (const glm::vec3& corner : face.corners)
                    {
                        terrain::TerrainVertex vertex;
                        vertex.position = glm::vec3(static_cast<float>(x),
                                                    static_cast<float>(y),
                                                    static_cast<float>(z)) + corner;
                        vertex.color = color;
                        vertex.normal = face.normal;
                        // UV 直接映射方块表面前 0..1，供需要贴图时使用；
                        // 当前体素材质只用顶点色，UV 留给后续图集。
                        vertex.uv = glm::vec2(corner.x + corner.z, corner.y);
                        outMesh.vertices.push_back(vertex);
                        boundsMin = glm::min(boundsMin, vertex.position);
                        boundsMax = glm::max(boundsMax, vertex.position);
                    }
                    outMesh.indices.push_back(baseVertex + 0u);
                    outMesh.indices.push_back(baseVertex + 1u);
                    outMesh.indices.push_back(baseVertex + 2u);
                    outMesh.indices.push_back(baseVertex + 0u);
                    outMesh.indices.push_back(baseVertex + 2u);
                    outMesh.indices.push_back(baseVertex + 3u);
                    if (outStats)
                        ++outStats->faces;
                }
            }
        }
    }

    sub.indexCount = static_cast<uint32_t>(outMesh.indices.size());
    sub.surfaceIndexCount = sub.indexCount;
    sub.boundsMin = (boundsMin.x > boundsMax.x) ? glm::vec3(0.0f) : boundsMin;
    sub.boundsMax = (boundsMin.x > boundsMax.x) ? glm::vec3(0.0f) : boundsMax;

    if (outStats)
    {
        outStats->vertices = static_cast<uint32_t>(outMesh.vertices.size());
        outStats->indices = static_cast<uint32_t>(outMesh.indices.size());
    }
    return true;
}

bool selfTest()
{
    Chunk chunk;
    MeshStats stats;
    terrain::TerrainMeshData mesh;

    // 1) 空气 chunk 不出面。
    buildMesh(chunk, mesh, &stats);
    if (stats.faces != 0u || !mesh.indices.empty())
    {
        std::cerr << "[Voxel] selfTest FAIL: empty chunk emitted geometry\n";
        return false;
    }

    // 2) 单块 = 6 个面 / 24 顶点 / 36 索引。
    chunk.set(5, 5, 5, static_cast<uint8_t>(BlockId::Stone));
    buildMesh(chunk, mesh, &stats);
    if (stats.faces != 6u || stats.vertices != 24u || stats.indices != 36u)
    {
        std::cerr << "[Voxel] selfTest FAIL: single block faces=" << stats.faces
                  << " vertices=" << stats.vertices << " indices=" << stats.indices << "\n";
        return false;
    }

    // 3) 相邻两块共享的面被剔除 -> 10 个面。
    chunk.set(6, 5, 5, static_cast<uint8_t>(BlockId::Stone));
    buildMesh(chunk, mesh, &stats);
    if (stats.faces != 10u)
    {
        std::cerr << "[Voxel] selfTest FAIL: adjacent blocks faces=" << stats.faces
                  << " (expected 10)\n";
        return false;
    }

    // 4) 挖掉邻块后，原本被遮挡的面重新出现 -> 回到 6 面。
    chunk.set(6, 5, 5, static_cast<uint8_t>(BlockId::Air));
    buildMesh(chunk, mesh, &stats);
    if (stats.faces != 6u)
    {
        std::cerr << "[Voxel] selfTest FAIL: dig did not restore faces\n";
        return false;
    }

    // 5) 地形生成：相邻 chunk 的边界高度必须一致（同一个高度函数）。
    Chunk a;
    Chunk b;
    a.generate(1234ull, 0, 0, 0);
    b.generate(1234ull, 1, 0, 0);
    // 相邻 chunk 的接缝不是"同一列"（a 的 31 列是世界 31，b 的 0 列是世界 32），
    // 所以判据是：每列实际高度必须等于世界坐标高度函数算出的值 —— 两个 chunk
    // 共用同一张世界坐标高度图，块边界这才天然连续。
    bool boundaryMatch = true;
    for (int32_t z = 0; z < Chunk::kSize && boundaryMatch; ++z)
    {
        for (int32_t x = 0; x < Chunk::kSize && boundaryMatch; ++x)
        {
            int32_t topA = -1;
            for (int32_t y = Chunk::kSize - 1; y >= 0; --y)
            {
                if (!isAir(a.get(x, y, z))) { topA = y; break; }
            }
            if (topA != surfaceHeight(x, z, 0, 1234ull))
                boundaryMatch = false;
        }
    }
    if (!boundaryMatch)
    {
        std::cerr << "[Voxel] selfTest FAIL: chunk column height does not match world height function" << std::endl;
        return false;
    }
    {
        // b 的 0 列必须是世界坐标 32 的高度，而不是被当成 0 重新起算。
        int32_t topB = -1;
        for (int32_t y = Chunk::kSize - 1; y >= 0; --y)
        {
            if (!isAir(b.get(0, y, 16))) { topB = y; break; }
        }
        if (topB != surfaceHeight(Chunk::kSize, 16, 0, 1234ull))
        {
            std::cerr << "[Voxel] selfTest FAIL: chunk origin not applied to height function" << std::endl;
            return false;
        }
    }
    if (!boundaryMatch)
    {
        std::cerr << "[Voxel] selfTest FAIL: chunk boundary heights differ\n";
        return false;
    }

    std::cout << "[Voxel] selfTest PASS | face culling 6/10/6, boundary continuous, solid="
              << a.solidCount() << "\n";
    return true;
}

} // namespace voxel
