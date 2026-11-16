#include "Scene/CharacterSimulation.h"

#include "Scene/SimulationClock.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace scene
{

CharacterSimStep stepCharacter(const CollisionWorld& world,
                               const CharacterSimSettings& settings,
                               const CharacterSimState& previous,
                               const glm::vec3& moveDirection,
                               bool jump,
                               float dt,
                               float speedMultiplier)
{
    CharacterSimStep out;
    out.state = previous;

    if (!std::isfinite(dt) || dt <= 0.0f)
        return out;

    dt = std::min(dt, settings.maxStepDt);
    speedMultiplier = std::max(speedMultiplier, 0.0f);

    glm::vec3 move(moveDirection.x, 0.0f, moveDirection.z);
    if (glm::dot(move, move) > 1.0e-8f)
        move = glm::normalize(move);
    else
        move = glm::vec3(0.0f);
    out.moveDirection = move;

    float verticalVelocity = previous.verticalVelocity;
    bool grounded = previous.grounded;

    const bool jumped = jump && grounded;
    if (jumped)
    {
        verticalVelocity = std::max(0.0f, settings.jumpSpeed);
        grounded = false;
    }
    if (grounded)
        verticalVelocity = 0.0f;
    else
        verticalVelocity -= settings.gravity * dt;

    const glm::vec3 delta(move.x * settings.moveSpeed * speedMultiplier * dt,
                          verticalVelocity * dt,
                          move.z * settings.moveSpeed * speedMultiplier * dt);

    out.solved = moveCharacter(world, settings.shape, settings.move, previous.position, delta,
                               previous.grounded && !jumped);

    out.state.position = out.solved.position;
    out.state.grounded = out.solved.grounded;
    out.state.verticalVelocity = out.state.grounded ? 0.0f : verticalVelocity;

    out.blocked = out.solved.blocked;
    out.jumped = jumped;
    out.justLanded = !previous.grounded && out.state.grounded &&
                     previous.verticalVelocity < -1.0f;
    out.horizontalSpeed =
        glm::length(glm::vec2(out.state.position.x - previous.position.x,
                              out.state.position.z - previous.position.z)) / dt;
    return out;
}

namespace
{

// 解析世界：平地 + 一级台阶 + 一堵墙。和 CharacterCollision 的自检世界同构，
// 但这里要的是"跑整段轨迹"，所以单独放一份。
struct ReplayWorld final : CollisionWorld
{
    std::size_t collectBoxes(const glm::vec3& center, const glm::vec3& halfExtents,
                             CollisionBox* out, std::size_t capacity) const override
    {
        std::size_t count = 0;
        for (const CollisionBox& box : boxes)
        {
            if (std::abs(box.center.x - center.x) > box.halfExtents.x + halfExtents.x ||
                std::abs(box.center.y - center.y) > box.halfExtents.y + halfExtents.y ||
                std::abs(box.center.z - center.z) > box.halfExtents.z + halfExtents.z)
                continue;
            if (count < capacity)
                out[count] = box;
            ++count;
        }
        return count;
    }

    bool sampleGround(float, float, float& outHeight, glm::vec3& outNormal) const override
    {
        outHeight = 0.0f;
        outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        return true;
    }

    std::vector<CollisionBox> boxes;
};

uint64_t fnv1a(uint64_t hash, const void* data, std::size_t bytes)
{
    const auto* first = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i)
    {
        hash ^= static_cast<uint64_t>(first[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t hashState(const CharacterSimState& state, uint32_t tick)
{
    uint64_t hash = 1469598103934665603ull;
    hash = fnv1a(hash, &tick, sizeof(tick));
    hash = fnv1a(hash, &state.position, sizeof(state.position));
    hash = fnv1a(hash, &state.verticalVelocity, sizeof(state.verticalVelocity));
    const uint8_t grounded = state.grounded ? 1u : 0u;
    hash = fnv1a(hash, &grounded, sizeof(grounded));
    return hash;
}

struct Segment
{
    const char* name;
    float speedMultiplier;
    bool jump;          // 在 tick 30 起跳
    glm::vec3 direction;
    float seconds;
};

} // namespace

bool characterSimulationSelfTest()
{
    bool ok = true;
    const auto expect = [&ok](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "[Replay] characterSimulation FAIL: " << message << '\n';
            ok = false;
        }
    };

    ReplayWorld world;
    CollisionBox step;
    step.center = glm::vec3(2.0f, 0.2f, 0.0f);
    step.halfExtents = glm::vec3(0.5f, 0.2f, 1.0f);
    world.boxes.push_back(step);
    CollisionBox wall;
    wall.center = glm::vec3(6.0f, 1.0f, 0.0f);
    wall.halfExtents = glm::vec3(0.2f, 1.0f, 2.0f);
    world.boxes.push_back(wall);

    CharacterSimSettings settings;
    settings.shape.radius = 0.35f;
    settings.shape.height = 1.8f;
    settings.move.maxSlopeAngle = 50.0f;
    settings.move.stepHeight = 0.5f;
    settings.move.groundSnapDistance = 0.4f;

    const Segment segments[] = {
        { "walk", 1.0f, false, glm::vec3(1.0f, 0.0f, 0.0f), 2.0f },
        { "jump", 1.0f, true, glm::vec3(1.0f, 0.0f, 0.0f), 2.0f },
        { "sprint-diagonal", 1.35f, false, glm::vec3(0.7071f, 0.0f, 0.7071f), 2.0f },
    };

    uint32_t totalTicks = 0;
    uint64_t combinedHash = 1469598103934665603ull;

    for (const Segment& segment : segments)
    {
        std::vector<uint64_t> hashes[2];
        uint32_t ticks[2] = { 0, 0 };

        for (int rate = 0; rate < 2; ++rate)
        {
            SimulationClock clock;
            clock.setFixedStep(1.0f / 60.0f);
            CharacterSimState state;

            const float baseDelta = (rate == 0) ? (1.0f / 60.0f) : (1.0f / 144.0f);
            float elapsed = 0.0f;
            uint32_t frame = 0;
            while (elapsed < segment.seconds)
            {
                // 144Hz 那条故意带抖动：60Hz 帧、100Hz 帧交替，模拟仍必须逐 tick 对齐。
                float delta = baseDelta;
                if (rate == 1)
                    delta = ((frame % 3) == 0) ? (1.0f / 60.0f) : (1.0f / 100.0f);
                ++frame;
                elapsed += delta;

                InputCmd cmd;
                cmd.moveWorld = segment.direction;
                cmd.sprint = segment.speedMultiplier > 1.0f;
                cmd.jump = segment.jump && clock.tick() + 1 == 30;

                clock.advance(delta, cmd,
                    [&](const InputCmd& current, float fixedDt)
                    {
                        const bool jumpNow = current.jump;
                        const CharacterSimStep stepped =
                            stepCharacter(world, settings, state, current.moveWorld, jumpNow,
                                          fixedDt, segment.speedMultiplier);
                        state = stepped.state;
                        hashes[rate].push_back(hashState(state, clock.tick()));
                    });
            }
            ticks[rate] = clock.tick();
        }

        const std::size_t common = std::min(hashes[0].size(), hashes[1].size());
        expect(common >= 100, "each segment should produce at least 100 common ticks");
        std::size_t mismatch = 0;
        for (std::size_t i = 0; i < common; ++i)
        {
            if (hashes[0][i] != hashes[1][i])
            {
                ++mismatch;
                if (mismatch == 1)
                {
                    std::cerr << "[Replay] first mismatch in segment " << segment.name
                              << " at tick " << (i + 1) << '\n';
                }
            }
        }
        expect(mismatch == 0, "60Hz and 144Hz must produce identical per-tick hashes");
        expect(ticks[0] + 1 >= ticks[1] && ticks[1] + 1 >= ticks[0],
               "tick counts may differ by at most one");

        for (std::size_t i = 0; i < common; ++i)
            combinedHash = fnv1a(combinedHash, &hashes[0][i], sizeof(uint64_t));
        totalTicks += static_cast<uint32_t>(common);
    }

    if (ok)
    {
        std::cout << "[Replay] characterSimulation PASS | segments=3 ticks=" << totalTicks
                  << " frame-rates=60/144 combined=0x" << std::hex << combinedHash
                  << std::dec << '\n';
    }
    return ok;
}

} // namespace scene
