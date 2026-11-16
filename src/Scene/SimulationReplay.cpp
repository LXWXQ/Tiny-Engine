#include "Scene/SimulationReplay.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace scene
{

namespace
{

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

// 解析世界：平地 + 一级台阶。台阶用来让"上台阶"这一步也进入录制/回放对比。
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

struct Segment
{
    const char* name;
    glm::vec3 direction;
    bool jump;
    bool sprint;
    float seconds;
};

} // namespace

void ReplayRecorder::clear()
{
    ticks_.clear();
    hash_ = 1469598103934665603ull;
}

void ReplayRecorder::record(const InputCmd& cmd)
{
    ReplayTick tick;
    tick.tick = cmd.tick;
    tick.moveWorld = cmd.moveWorld;
    tick.jump = cmd.jump;
    tick.sprint = cmd.sprint;
    ticks_.push_back(tick);

    hash_ = fnv1a(hash_, &tick.tick, sizeof(tick.tick));
    hash_ = fnv1a(hash_, &tick.moveWorld, sizeof(tick.moveWorld));
    const uint8_t flags = static_cast<uint8_t>((tick.jump ? 1u : 0u) | (tick.sprint ? 2u : 0u));
    hash_ = fnv1a(hash_, &flags, sizeof(flags));
}

uint64_t hashCharacterState(const CharacterSimState& state, uint32_t tick)
{
    uint64_t hash = 1469598103934665603ull;
    hash = fnv1a(hash, &tick, sizeof(tick));
    hash = fnv1a(hash, &state.position, sizeof(state.position));
    hash = fnv1a(hash, &state.verticalVelocity, sizeof(state.verticalVelocity));
    const uint8_t grounded = state.grounded ? 1u : 0u;
    hash = fnv1a(hash, &grounded, sizeof(grounded));
    return hash;
}

bool simulationReplaySelfTest()
{
    bool ok = true;
    const auto expect = [&ok](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "[Replay] replaySelfTest FAIL: " << message << '\n';
            ok = false;
        }
    };

    ReplayWorld world;
    CollisionBox step;
    step.center = glm::vec3(2.0f, 0.2f, 0.0f);
    step.halfExtents = glm::vec3(0.5f, 0.2f, 1.0f);
    world.boxes.push_back(step);

    CharacterSimSettings settings;
    settings.shape.radius = 0.35f;
    settings.shape.height = 1.8f;

    const Segment segments[] = {
        { "walk", glm::vec3(1.0f, 0.0f, 0.0f), false, false, 2.0f },
        { "jump", glm::vec3(1.0f, 0.0f, 0.0f), true, false, 2.0f },
        { "sprint", glm::vec3(0.7071f, 0.0f, 0.7071f), false, true, 2.0f },
    };

    ReplayRecorder recorder;
    std::vector<uint64_t> recorded;
    std::size_t mismatches = 0;
    std::size_t totalTicks = 0;

    for (const Segment& segment : segments)
    {
        recorder.clear();
        recorded.clear();

        SimulationClock clock;
        clock.setFixedStep(1.0f / 60.0f);
        CharacterSimState state;

        float elapsed = 0.0f;
        uint32_t frame = 0;
        while (elapsed < segment.seconds)
        {
            // 144Hz 左右但故意抖动：录制侧不假设帧率，回放侧只按 tick 号重放。
            const float delta = ((frame % 3) == 0) ? (1.0f / 60.0f) : (1.0f / 100.0f);
            ++frame;
            elapsed += delta;

            InputCmd cmd;
            cmd.moveWorld = segment.direction;
            cmd.jump = segment.jump && clock.tick() + 1 == 30;
            cmd.sprint = segment.sprint;

            clock.advance(delta, cmd,
                [&](const InputCmd& current, float fixedDt)
                {
                    recorder.record(current);
                    const CharacterSimStep stepped =
                        stepCharacter(world, settings, state, current.moveWorld, current.jump,
                                      fixedDt, current.sprint ? 1.35f : 1.0f);
                    state = stepped.state;
                    recorded.push_back(hashCharacterState(state, current.tick));
                });
        }

        expect(recorder.size() == recorded.size(), "recorder must capture every tick");
        expect(recorder.size() >= 100, "each segment should record at least 100 ticks");
        totalTicks += recorder.size();

        // 回放：只吃录制下来的输入，固定步长重跑一遍。
        CharacterSimState replayedState;
        for (std::size_t i = 0; i < recorder.ticks().size(); ++i)
        {
            const ReplayTick& tick = recorder.ticks()[i];
            const CharacterSimStep stepped =
                stepCharacter(world, settings, replayedState, tick.moveWorld, tick.jump,
                              1.0f / 60.0f, tick.sprint ? 1.35f : 1.0f);
            replayedState = stepped.state;

            const uint64_t hash = hashCharacterState(replayedState, tick.tick);
            if (hash != recorded[i])
            {
                ++mismatches;
                if (mismatches == 1)
                {
                    std::cerr << "[Replay] first replay mismatch in segment " << segment.name
                              << " at tick " << tick.tick << '\n';
                }
            }
        }

        // 同一串输入录两遍，录制器哈希必须一致（防止录制器自己有状态残留）。
        const uint64_t firstHash = recorder.hash();
        ReplayRecorder again;
        for (const ReplayTick& tick : recorder.ticks())
        {
            InputCmd cmd;
            cmd.tick = tick.tick;
            cmd.moveWorld = tick.moveWorld;
            cmd.jump = tick.jump;
            cmd.sprint = tick.sprint;
            again.record(cmd);
        }
        expect(again.hash() == firstHash, "recorder hash must be stable across identical input");
    }

    expect(mismatches == 0, "replay must reproduce every recorded tick hash");

    if (ok)
    {
        std::cout << "[Replay] replaySelfTest PASS | ticks=" << totalTicks
                  << " segments=3 hash=0x" << std::hex << recorder.hash() << std::dec
                  << " match=ok\n";
    }
    return ok;
}

} // namespace scene
