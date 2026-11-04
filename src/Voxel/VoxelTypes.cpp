#include "Voxel/VoxelTypes.h"

#include <array>
#include <cstring>

namespace voxel
{

namespace
{

const std::array<BlockInfo, kBlockCount> kBlocks = { {
    { "Air",    0.0f,  0.0f,  0.0f,  false, false },
    { "Grass",  0.35f, 0.55f, 0.22f, true,  true },
    { "Dirt",   0.45f, 0.32f, 0.20f, true,  true },
    { "Stone",  0.50f, 0.50f, 0.52f, true,  true },
    { "Sand",   0.82f, 0.76f, 0.55f, true,  true },
    { "Wood",   0.38f, 0.25f, 0.14f, true,  true },
    { "Leaves", 0.25f, 0.48f, 0.20f, true,  false },
    { "Plank",  0.62f, 0.45f, 0.26f, true,  true },
    { "Brick",  0.55f, 0.24f, 0.18f, true,  true },
    { "Glass",  0.70f, 0.82f, 0.88f, true,  false },
} };

} // namespace

const BlockInfo& getBlockInfo(BlockId id)
{
    const uint32_t index = static_cast<uint32_t>(id);
    if (index >= kBlockCount)
        return kBlocks[0];
    return kBlocks[index];
}

const char* blockName(uint8_t id)
{
    return getBlockInfo(static_cast<BlockId>(id)).name;
}

BlockId blockIdFromName(const char* name)
{
    if (name == nullptr)
        return BlockId::Air;
    for (uint32_t i = 0; i < kBlockCount; ++i)
    {
        if (std::strcmp(kBlocks[i].name, name) == 0)
            return static_cast<BlockId>(i);
    }
    return BlockId::Air;
}

} // namespace voxel
