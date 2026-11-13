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

#include "Scene/ComponentTypes.h"
#include "Terrain/TerrainConfig.h"
#include "Terrain/TerrainSculptDelta.h"

#include <cstdint>
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

    // Terrain is serialized as parameters only. Mesh vertices, indices and
    // GPU handles are never written; loading regenerates the mesh.
    bool isTerrain = false;
    terrain::TerrainConfig terrainConfig;

    // Runtime streamed terrain manager. Chunk entities are derived from this
    // template and are intentionally not serialized as individual entities.
    // B4：手动地形（isTerrain）的雕刻差分；按生成态为基准，只存改过的格子。
    terrain::TerrainSculptDelta terrainSculpt;

    bool isTerrainWorld = false;
    terrain::TerrainConfig terrainWorldConfig;
    int32_t terrainLoadRadius = 1;
    int32_t terrainUnloadPadding = 1;
    uint32_t terrainMaxLoadsPerFrame = 1;

    // B-07 kinematic character. Runtime velocity/ground state are derived on
    // load and intentionally not serialized.
    bool isCharacter = false;
    float characterRadius = 0.35f;
    float characterHeight = 1.8f;
    float characterMoveSpeed = 6.0f;
    float characterJumpSpeed = 6.0f;
    float characterGravity = 18.0f;
    float characterMaxSlopeAngle = 50.0f;
    float characterStepHeight = 0.5f;
    float characterGroundSnapDistance = 0.4f;

    MaterialDescription material;

    // 元数据驱动的扩展组件（Spin / Orbit / 以后新增的特性）。
    // 只有 ComponentRegistry 注册过的类型才会出现在这里；运行期专用
    // 组件（TerrainChunk、VoxelChunk 等）不进序列化。
    std::vector<ComponentRecord> components;
};

// F3：体素世界只存 seed + 与"按 seed 重新生成"的差分。
// 一个 32^3 chunk 是 32768 个方块，全量存盘会让场景文件爆掉；
// 实际游玩的改动通常只有几百格，所以差分是最省且最稳的做法。
struct VoxelBlockEdit
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint8_t block = 0;
};

struct VoxelChunkDescription
{
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    std::vector<VoxelBlockEdit> edits;
};

struct VoxelWorldDescription
{
    uint64_t seed = 1337ull;
    int32_t radiusChunks = 1;
    int32_t baseY = 0;
    std::vector<VoxelChunkDescription> chunks;
};

struct SceneDescription
{
    // 文件格式版本。加字段时递增，并在读取侧做兼容。
    int version = 1;

    std::vector<EntityDescription> entities;

    // v4：场景级功能组件（阴影 / Bloom / 雾 / 水面…）。
    // 与实体组件分开存，因为它们的宿主是 Scene 内部的隐藏设置实体。
    std::vector<ComponentRecord> sceneComponents;

    // 区分“旧文件里没有 sceneComponents 字段”（从 EngineSettings 初始化）
    // 和“v4 文件写了空数组”（用户把全部场景功能都移除了）。
    bool sceneComponentsAuthored = false;

    // F3：体素世界（seed + 差分）。老场景文件没有这一段，读的时候为空。
    std::vector<VoxelWorldDescription> voxelWorlds;
};

} // namespace scene
