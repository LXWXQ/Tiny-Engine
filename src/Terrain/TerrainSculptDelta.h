#pragma once

// ===========================================================================
// B4：地形雕刻差分
//
// 地形存档只存 TerrainConfig（参数），高度场是按参数重新生成的。一旦用户用
// 笔刷改过地形，重新生成就会把改动抹掉（tech-debt #10）。把整块高度场写盘又
// 太贵：257x257 的 float 是 264KB，而一次半径 4m 的笔刷通常只动几百个格子。
//
// 所以存档里只存「与生成态的差分」：被改过的格子下标 + 归一化高度。
// 加载时先生成基础高度场，再把差分盖上去，最后按这份高度场重建网格。
// ===========================================================================

#include "Terrain/TerrainHeightField.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace terrain
{

struct TerrainSculptDelta
{
    // heights 的下标（row-major：index = z * width + x），严格递增。
    std::vector<uint32_t> indices;
    // 归一化高度 [0, 1]，与 indices 等长。
    std::vector<float> heights;

    bool empty() const { return indices.empty(); }
    std::size_t size() const { return indices.size(); }
};

// 比较两份同尺寸高度场，把数值不同的格子按升序写进 out。
// 尺寸/高度数组不一致时返回 false 并写 errorMessage。
bool collectSculptDelta(const HeightField& before, const HeightField& after,
                        TerrainSculptDelta& out, std::string& errorMessage);

// 把差分盖到高度场上：下标越界、长度不等、下标非递增都拒绝；
// 高度被夹到 [0,1]，成功后刷新 minValue/maxValue。
bool applySculptDelta(HeightField& field, const TerrainSculptDelta& delta,
                      std::string& errorMessage);

// 纯逻辑自检（不需要 Vulkan / 文件系统）。
bool terrainSculptDeltaSelfTest();

} // namespace terrain
