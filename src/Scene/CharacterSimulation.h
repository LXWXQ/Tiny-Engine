#pragma once

// 角色步进（D2）：把"一 tick 里角色怎么动"从 Scene 里彻底抽出来。
//
// 为什么要单独一个文件：C1 只保证了"模拟按 1/60 推进"，但 updateCharacterController
// 里还混着 ECS 查询、Transform 写入与动画更新 —— 同一串输入在不同机器上仍然可能
// 因为"哪一帧查了地面"而分叉，也就没法逐 tick 对比。这里只保留纯计算：
//
//   stepCharacter(world, settings, previous, move, jump, dt) -> 新的位置/竖直速度/接地状态
//
// 它不读 ImGui、不读时间、不碰 registry，输入相同就一定输出相同（自检里用两个
// 不同帧率驱动同一串输入来验证）。
//
// 重力/跳跃仍在这里算：跳跃是"边沿 + 只在接地时生效"，和位移耦合得很紧，拆开反而
// 容易两边各算一次。

#include "Scene/CharacterCollision.h"

#include <glm/glm.hpp>

namespace scene
{

struct CharacterSimState
{
    glm::vec3 position{ 0.0f };   // 脚底
    float verticalVelocity = 0.0f;
    bool grounded = false;
};

struct CharacterSimSettings
{
    CharacterShape shape;
    CharacterMoveSettings move;
    float moveSpeed = 6.0f;
    float jumpSpeed = 6.0f;
    float gravity = 18.0f;
    float maxStepDt = 0.1f;       // 与 Scene 侧一致：单 tick 不允许超过 0.1s
};

struct CharacterSimStep
{
    CharacterSimState state;
    CharacterMoveOutput solved;
    glm::vec3 moveDirection{ 0.0f };
    bool blocked = false;
    bool jumped = false;
    bool justLanded = false;
    float horizontalSpeed = 0.0f;
};

CharacterSimStep stepCharacter(const CollisionWorld& world,
                               const CharacterSimSettings& settings,
                               const CharacterSimState& previous,
                               const glm::vec3& moveDirection,
                               bool jump,
                               float dt,
                               float speedMultiplier = 1.0f);

// 契约自检：同一串输入在 60Hz 与 144Hz（带抖动）下逐 tick 位置逐位相同。
bool characterSimulationSelfTest();

} // namespace scene
