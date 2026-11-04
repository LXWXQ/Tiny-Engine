#include "Voxel/VoxelChunk.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

#include <glm/glm.hpp>

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

namespace
{

// 贪心合并要知道每个面在哪个轴上、平面内两个轴是谁、朝哪一侧。
struct FaceAxis
{
    int axis;
    int u;
    int v;
    int sign;
};

const FaceAxis kFaceAxes[6] = {
    { 1, 0, 2, +1 },   // +Y
    { 1, 0, 2, -1 },   // -Y
    { 0, 1, 2, +1 },   // +X
    { 0, 1, 2, -1 },   // -X
    { 2, 0, 1, +1 },   // +Z
    { 2, 0, 1, -1 },   // -Z
};

bool solidAt(const Chunk& chunk, int32_t x, int32_t y, int32_t z)
{
    return !isAir(chunk.get(x, y, z));
}

// 顶点 AO：看这个角在空气侧的两条边与对角，被挡得越多越暗（3 档 + 1 档兜底）。
float vertexAo(const Chunk& chunk, const int32_t pos[3], const FaceAxis& ax, int cu, int cv)
{
    const int du = (cu == 0) ? -1 : 1;
    const int dv = (cv == 0) ? -1 : 1;

    int32_t base[3] = { pos[0], pos[1], pos[2] };
    base[ax.axis] += ax.sign;

    int32_t side1[3] = { base[0], base[1], base[2] };
    side1[ax.u] += du;
    int32_t side2[3] = { base[0], base[1], base[2] };
    side2[ax.v] += dv;
    int32_t corner[3] = { base[0], base[1], base[2] };
    corner[ax.u] += du;
    corner[ax.v] += dv;

    const int s1 = solidAt(chunk, side1[0], side1[1], side1[2]) ? 1 : 0;
    const int s2 = solidAt(chunk, side2[0], side2[1], side2[2]) ? 1 : 0;
    const int cc = solidAt(chunk, corner[0], corner[1], corner[2]) ? 1 : 0;

    const int level = (s1 && s2) ? 0 : (3 - (s1 + s2 + cc));
    return 0.55f + 0.15f * static_cast<float>(level);
}

} // namespace

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

    std::array<uint8_t, Chunk::kSize * Chunk::kSize> mask{};

    for (int32_t f = 0; f < 6; ++f)
    {
        const FaceDef& face = kFaces[f];
        const FaceAxis& ax = kFaceAxes[f];

        for (int32_t slice = 0; slice < Chunk::kSize; ++slice)
        {
            mask.fill(0u);
            bool any = false;
            for (int32_t vi = 0; vi < Chunk::kSize; ++vi)
            {
                for (int32_t ui = 0; ui < Chunk::kSize; ++ui)
                {
                    int32_t p[3] = { 0, 0, 0 };
                    p[ax.axis] = slice;
                    p[ax.u] = ui;
                    p[ax.v] = vi;
                    const uint8_t id = chunk.get(p[0], p[1], p[2]);
                    if (isAir(id))
                        continue;
                    int32_t n[3] = { p[0], p[1], p[2] };
                    n[ax.axis] += ax.sign;
                    if (isOpaque(chunk.get(n[0], n[1], n[2])))
                        continue;
                    mask[static_cast<size_t>(vi) * Chunk::kSize + ui] = id;
                    any = true;
                }
            }
            if (!any)
                continue;

            for (int32_t vi = 0; vi < Chunk::kSize; ++vi)
            {
                for (int32_t ui = 0; ui < Chunk::kSize; ++ui)
                {
                    const uint8_t id = mask[static_cast<size_t>(vi) * Chunk::kSize + ui];
                    if (id == 0u)
                        continue;

                    int32_t w = 1;
                    while (ui + w < Chunk::kSize &&
                           mask[static_cast<size_t>(vi) * Chunk::kSize + ui + w] == id)
                        ++w;

                    int32_t h = 1;
                    bool grow = true;
                    while (vi + h < Chunk::kSize && grow)
                    {
                        for (int32_t k = 0; k < w; ++k)
                        {
                            if (mask[static_cast<size_t>(vi + h) * Chunk::kSize + ui + k] != id)
                            {
                                grow = false;
                                break;
                            }
                        }
                        if (grow)
                            ++h;
                    }

                    for (int32_t dv = 0; dv < h; ++dv)
                        for (int32_t du = 0; du < w; ++du)
                            mask[static_cast<size_t>(vi + dv) * Chunk::kSize + ui + du] = 0u;

                    glm::vec3 lo(0.0f);
                    glm::vec3 hi(0.0f);
                    lo[ax.axis] = static_cast<float>(slice);
                    hi[ax.axis] = static_cast<float>(slice + 1);
                    lo[ax.u] = static_cast<float>(ui);
                    hi[ax.u] = static_cast<float>(ui + w);
                    lo[ax.v] = static_cast<float>(vi);
                    hi[ax.v] = static_cast<float>(vi + h);

                    const uint32_t baseVertex = static_cast<uint32_t>(outMesh.vertices.size());
                    for (const glm::vec3& corner : face.corners)
                    {
                        terrain::TerrainVertex vertex;
                        vertex.position = glm::mix(lo, hi, corner);

                        const int cu = (corner[ax.u] > 0.5f) ? 1 : 0;
                        const int cv = (corner[ax.v] > 0.5f) ? 1 : 0;
                        int32_t pos[3] = { 0, 0, 0 };
                        pos[ax.axis] = slice;
                        pos[ax.u] = (cu == 0) ? ui : (ui + w - 1);
                        pos[ax.v] = (cv == 0) ? vi : (vi + h - 1);
                        const float ao = vertexAo(chunk, pos, ax, cu, cv);

                        // F3-f：颜色由图集提供，顶点色只保留面明暗 x AO。
                        const float shade = face.shade * ao;
                        vertex.color = glm::vec3(shade, shade, shade);
                        vertex.normal = face.normal;
                        // 图集寻址：uv.x = tile * 64 + 面内跨度u，uv.y = 面内跨度v。
                        const float tileBase = static_cast<float>(id > 0 ? id - 1 : 0) * 64.0f;
                        vertex.uv = glm::vec2(tileBase + corner[ax.u] * static_cast<float>(w),
                                              corner[ax.v] * static_cast<float>(h));
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

    // 2b) F3-f：顶点 uv 编码的是图集图块（Stone = BlockId 3 -> tile 2，基准 128），
    //     面内跨度落在 [0, 1]，片元据此还原图块坐标。
    for (const terrain::TerrainVertex& vertex : mesh.vertices)
    {
        if (vertex.uv.x < 128.0f || vertex.uv.x > 192.0f || vertex.uv.y < 0.0f || vertex.uv.y > 1.0f)
        {
            std::cerr << "[Voxel] selfTest FAIL: atlas uv out of range (" << vertex.uv.x
                      << ", " << vertex.uv.y << ")" << std::endl;
            return false;
        }
    }

    // 3) 相邻两块：共享面被剔除，贪心把 2x1 顶面/底面各并成一个 quad -> 6 个面。
    chunk.set(6, 5, 5, static_cast<uint8_t>(BlockId::Stone));
    buildMesh(chunk, mesh, &stats);
    if (stats.faces != 6u)
    {
        std::cerr << "[Voxel] selfTest FAIL: adjacent blocks faces=" << stats.faces
                  << " (expected 6 after greedy merge)" << std::endl;
        return false;
    }

    // 3b) 2x2 平板：六个方向各合并成一个矩形，仍是 6 个面（不合并会是 16 个）。
    Chunk slab;
    for (int32_t z = 0; z < 2; ++z)
    {
        for (int32_t x = 0; x < 2; ++x)
            slab.set(x, 0, z, static_cast<uint8_t>(BlockId::Plank));
    }
    buildMesh(slab, mesh, &stats);
    if (stats.faces != 6u)
    {
        std::cerr << "[Voxel] selfTest FAIL: greedy slab faces=" << stats.faces
                  << " (expected 6)" << std::endl;
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

    std::cout << "[Voxel] selfTest PASS | face culling + greedy 6/6/6/6, boundary continuous, solid="
              << a.solidCount() << "\n";
    return true;
}

} // namespace voxel
