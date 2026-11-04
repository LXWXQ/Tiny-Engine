#pragma once

#include "Voxel/VoxelTypes.h"
#include "Terrain/TerrainMeshBuilder.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// F3：一个 32^3 的方块 chunk。
//
// 存储是稠密的 uint8 数组（32 KB / chunk）—— 体素世界的局部性极强，稠密数组
// 的一次索引比任何稀疏结构都快，而且挖掘/放置只改一个字节。
namespace voxel
{

class Chunk
{
public:
    static constexpr int32_t kSize = 32;
    static constexpr int32_t kSizeShift = 5;   // 2^5 = 32
    static constexpr int32_t kSizeMask = kSize - 1;
    static constexpr uint32_t kBlockCount = static_cast<uint32_t>(kSize) * kSize * kSize;

    Chunk() { blocks.fill(static_cast<uint8_t>(BlockId::Air)); }

    static int32_t toLocal(int32_t world) { return ((world % kSize) + kSize) % kSize; }
    static int32_t toChunkCoord(int32_t world) { return world >> kSizeShift; }
    static bool inBounds(int32_t x, int32_t y, int32_t z)
    {
        return x >= 0 && y >= 0 && z >= 0 && x < kSize && y < kSize && z < kSize;
    }

    uint8_t get(int32_t x, int32_t y, int32_t z) const
    {
        if (!inBounds(x, y, z))
            return static_cast<uint8_t>(BlockId::Air);
        return blocks[static_cast<size_t>(index(x, y, z))];
    }

    void set(int32_t x, int32_t y, int32_t z, uint8_t id)
    {
        if (!inBounds(x, y, z))
            return;
        blocks[static_cast<size_t>(index(x, y, z))] = id;
    }

    void fill(uint8_t id) { blocks.fill(id); }

    // 简单值噪声地形：草皮 / 泥土 / 石头三层 + 平滑起伏。
    // seed 与 chunk 坐标共同决定高度，保证相邻 chunk 的边界连续。
    void generate(uint64_t seed, int32_t chunkX, int32_t chunkZ, int32_t baseY = 0);

    uint32_t solidCount() const;

private:
    static int32_t index(int32_t x, int32_t y, int32_t z)
    {
        return (y * kSize + z) * kSize + x;
    }

    std::array<uint8_t, kBlockCount> blocks{};
};

// 网格化：面剔除（邻格不透明就不出面），每个面 2 个三角形。
// 顶点色 = 方块颜色 x 面的方向明暗，让方块在没有贴图时也有立体感。
// outMesh 复用 terrain::TerrainMeshData，因为 Vertex 布局完全一致（pos/color/normal/uv），
// Scene 的 Model 上传路径可以直接消费。
struct MeshStats
{
    uint32_t faces = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
};

bool buildMesh(const Chunk& chunk,
               terrain::TerrainMeshData& outMesh,
               MeshStats* outStats = nullptr);

// F3 自检：验证面剔除数量、边界连续性、挖掘/放置的局部性。
bool selfTest();

} // namespace voxel
