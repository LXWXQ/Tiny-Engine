#pragma once

// F3：方块体素（七日杀 / 我的世界式）。
//
// 为什么是方块而不是 marching cubes / surface nets：挖掘与建造的"手感"来自
// 离散的方块格 —— 挖掉一块就是一个整齐的坑，放一块就是一个整齐的方块。
// 等值面网格虽然更"自然"，但改一格会让周围十几格的顶点全变，且挖出来的洞
// 是圆滑的，和"挖矿盖房"的期待不符。
//
// 数据侧只关心"这一格是什么方块"，颜色 / 拓扑都由这一张表推导。

#include <cstdint>

namespace voxel
{

// 方块 id。0 恒为空气，网格生成时"邻格是空气才出面"。
enum class BlockId : uint8_t
{
    Air = 0,
    Grass = 1,
    Dirt = 2,
    Stone = 3,
    Sand = 4,
    Wood = 5,
    Leaves = 6,
    Plank = 7,
    Brick = 8,
    Glass = 9,
    Count
};

constexpr uint32_t kBlockCount = static_cast<uint32_t>(BlockId::Count);

struct BlockInfo
{
    const char* name;
    // 顶点色。体素网格不采样贴图，直接用顶点色乘材质 —— 一个 chunk 一次
    // 绘制，顶点色足够表达方块种类，也省掉图集的 UV 对齐工作。
    float r, g, b;
    bool solid;     // 参与碰撞
    bool opaque;    // true = 完全挡住邻面；false（玻璃/树叶）不遮挡邻面
};

const BlockInfo& getBlockInfo(BlockId id);
inline BlockInfo const& blockInfo(uint8_t id)
{
    return getBlockInfo(static_cast<BlockId>(id));
}

inline bool isAir(uint8_t id) { return id == static_cast<uint8_t>(BlockId::Air); }
inline bool isSolid(uint8_t id) { return blockInfo(id).solid; }
inline bool isOpaque(uint8_t id) { return blockInfo(id).opaque; }

const char* blockName(uint8_t id);

// 从名字反查 id，供编辑器下拉框与存档用；找不到返回 Air。
BlockId blockIdFromName(const char* name);

} // namespace voxel
