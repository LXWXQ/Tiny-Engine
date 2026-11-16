#pragma once

// 角色碰撞：扫掠盒 + 滑动 + 上台阶。
//
// 为什么手写而不是接 Jolt：引擎目前没有任何刚体需求，D1 只要"角色不穿墙、
// 能上台阶、坡上不抖"。Jolt 需要给地形/体素/模型都造碰撞体、接构建系统，
// 还会引入一套自己的坐标系与确定性语义 —— 收益不匹配成本。
//
// 这里只依赖两个查询（收集附近 AABB、采样地面高度），所以
//   - Scene 侧适配器把地形（高度场）和体素/模型（AABB）接进来；
//   - 自检侧用解析几何造一个世界，不需要 Vulkan、不需要 Scene。
//
// 单位：米。位置一律是"脚底"（盒中心 = 脚底 + height/2）。

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace scene
{

// 静态碰撞体：轴对齐盒。体素方块与模型包围盒都归到这个形状。
struct CollisionBox
{
    glm::vec3 center{ 0.0f };
    glm::vec3 halfExtents{ 0.5f };
};

struct CharacterShape
{
    float radius = 0.35f;
    float height = 1.8f;
};

struct CharacterMoveSettings
{
    float maxSlopeAngle = 50.0f;   // 超过这个角度算"站不住"
    float stepHeight = 0.5f;       // 能直接迈上去的高度
    uint32_t maxIterations = 4;    // 滑动迭代上限
    float skin = 0.001f;           // 接触后回退的余量，避免嵌进去
    float groundSnapDistance = 0.4f;  // 下坡时主动贴地的最大高度差
};

struct CharacterMoveOutput
{
    glm::vec3 position{ 0.0f };        // 脚底
    bool grounded = false;
    bool blocked = false;              // 这一步有东西挡住过
    bool exhausted = false;            // 迭代用完了还有位移没走完
    uint32_t iterations = 0;
    float groundHeight = 0.0f;
    glm::vec3 groundNormal{ 0.0f, 1.0f, 0.0f };
};

class CollisionWorld
{
public:
    virtual ~CollisionWorld() = default;

    // 收集与给定盒相交的静态盒。最多写 capacity 个，返回实际相交总数
    // （可能大于 capacity，调用方自行决定是否扩大）。
    virtual std::size_t collectBoxes(const glm::vec3& center, const glm::vec3& halfExtents,
                                     CollisionBox* out, std::size_t capacity) const = 0;

    // 脚底在 (x,z) 处的地面高度与法线。没有地面返回 false。
    virtual bool sampleGround(float x, float z, float& outHeight,
                              glm::vec3& outNormal) const = 0;
};

// 走一步。delta 是这一帧想要的总位移（水平移动 + 重力*dt 已经合好）。
CharacterMoveOutput moveCharacter(const CollisionWorld& world,
                                  const CharacterShape& shape,
                                  const CharacterMoveSettings& settings,
                                  const glm::vec3& start,
                                  const glm::vec3& delta,
                                  bool wasGrounded);

bool characterCollisionSelfTest();

} // namespace scene
