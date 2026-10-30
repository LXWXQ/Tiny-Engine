#pragma once

// ===========================================================================
// 场景的"纯数据"表示（不依赖任何 Vulkan / EnTT 类型）
//
// 为什么单独一个结构体，而不是直接序列化 entt::registry：
//
//   1. **序列化边界清晰**：registry 里混着 GPU 句柄（Model 的 VkBuffer、材质槽位），
//      这些不能存盘。SceneDescription 只装"该存的东西"，从类型上就杜绝了误存。
//   2. **便于扩展**：以后加 Prefab、场景差异（diff）、撤销整场景，都以它为单位。
//   3. **可测试**：不建 Vulkan 设备也能验证"注册表 <-> 描述 <-> JSON"往返一致。
//
// 命名约定：*Index 字段表示"在数组里的下标"，不是实体 ID ——
// 实体 ID 是运行期分配的，跨进程没有意义。
// ===========================================================================

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace scene
{

struct MaterialDescription
{
    bool present = false;

    std::string name;
    glm::vec4 baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff = 0.5f;
    int alphaMode = 0;                 // 0=OPAQUE 1=MASK 2=BLEND

    // 相对资源根目录的路径；空串表示没有这张贴图
    std::string albedoMap;
    std::string normalMap;
    std::string metallicRoughnessMap;
};

struct EntityDescription
{
    std::string name = "Entity";

    glm::vec3 translation{ 0.0f };
    glm::vec3 rotation{ 0.0f };
    glm::vec3 scale{ 1.0f };

    // 父实体在数组里的下标；-1 表示根节点。
    // 用下标而不是实体 ID，是因为 ID 跨进程不稳定。
    int parentIndex = -1;

    // 相对资源根目录的网格路径；空串表示这是个纯逻辑实体（灯光/空节点等）
    std::string meshPath;

    bool isSkybox = false;
    bool isGrid = false;
    bool isCamera = false;

    MaterialDescription material;
};

struct SceneDescription
{
    // 文件格式版本。加字段时递增，并在读取侧做兼容。
    int version = 1;

    std::vector<EntityDescription> entities;
};

} // namespace scene
