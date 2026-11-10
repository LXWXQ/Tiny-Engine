#pragma once

#include "Scene/Model.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace vegetation
{

// 与 shaders/vegetation.vert 的实例布局一致：3 个 vec4。
// positionScale.xyz = 世界基点；.w = 世界高度缩放
// rotationTint.x = 绕 Y 的偏航；.y = 额外宽度缩放；.z = 风相位；.w = 颜色抖动
// color = albedo 乘色
struct Instance
{
    glm::vec4 positionScale{ 0.0f };
    glm::vec4 rotationTint{ 0.0f };
    glm::vec4 color{ 1.0f };
};
static_assert(sizeof(Instance) == 48, "vegetation instance must stay tightly packed");

struct Params
{
    float grassDensity = 0.8f;
    float grassDistance = 50.0f;
    float grassHeight = 0.55f;
    float treeDensity = 1.2f;
    float treeDistance = 120.0f;
    float treeHeight = 7.0f;
    float windStrength = 0.6f;
    int seed = 1337;
    glm::vec4 grassColor{ 0.24f, 0.36f, 0.10f, 1.0f };
    glm::vec4 treeColor{ 0.12f, 0.24f, 0.08f, 1.0f };
    glm::vec4 trunkColor{ 0.22f, 0.14f, 0.08f, 1.0f };
    bool waterEnabled = true;
    float waterLevel = 2.0f;
};

inline constexpr uint32_t kMaxGrassInstances = 49152;
inline constexpr uint32_t kMaxTreeInstances = 4096;

struct Placement
{
    std::vector<Instance> grass;
    std::vector<Instance> trees;
};

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// 程序化网格：草 = 3 张交叉 quad；树 = 6 棱树干 + 3 层锥形树冠。
bool buildMeshes(MeshData& grass, MeshData& tree, std::string* errorMessage = nullptr);

// 围绕相机放置草和树。ground(x,z,height) 应只返回地形（不返回体素/建筑），
// 由调用方注入 Scene::sampleTerrainOnlyGroundHeightWorld。函数内部做水面、
// 坡度、距离与确定性抖动筛选；同样的 seed + 相机位置结果一致。
Placement generate(const glm::vec3& camera, const Params& params,
                   const std::function<bool(float, float, float&)>& ground);

} // namespace vegetation
