#include "Scene/CharacterCollision.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace scene
{
namespace
{

constexpr std::size_t kMaxCandidates = 32;

// 盒 vs 盒扫掠：把静态盒按移动盒的半尺寸做 Minkowski 展开，然后做 slab 测试。
bool sweepBoxAabb(const glm::vec3& center, const glm::vec3& half, const glm::vec3& delta,
                  const CollisionBox& other, float& outTime, glm::vec3& outNormal)
{
    const glm::vec3 expanded = other.halfExtents + half;
    const glm::vec3 origin = center - other.center;

    float tEnter = 0.0f;
    float tExit = 1.0f;
    int axis = -1;

    for (int i = 0; i < 3; ++i)
    {
        const float d = delta[i];
        const float o = origin[i];
        const float e = expanded[i];

        if (std::abs(d) < 1.0e-8f)
        {
            if (o < -e || o > e)
                return false;
            continue;
        }

        float t1 = (-e - o) / d;
        float t2 = (e - o) / d;
        if (t1 > t2)
            std::swap(t1, t2);

        if (t1 > tEnter)
        {
            tEnter = t1;
            axis = i;
        }
        tExit = std::min(tExit, t2);
        if (tEnter > tExit)
            return false;
    }

    // 没有分离轴说明起始就重叠，交给调用方按"已经贴着"处理。
    if (axis < 0 || tEnter < 0.0f || tEnter > 1.0f)
        return false;

    outTime = tEnter;
    outNormal = glm::vec3(0.0f);
    outNormal[axis] = (delta[axis] > 0.0f) ? -1.0f : 1.0f;
    return true;
}

bool overlapsBox(const glm::vec3& center, const glm::vec3& half, const CollisionBox& other)
{
    const glm::vec3 lo = other.center - other.halfExtents;
    const glm::vec3 hi = other.center + other.halfExtents;
    return center.x > lo.x - half.x && center.x < hi.x + half.x &&
           center.y > lo.y - half.y && center.y < hi.y + half.y &&
           center.z > lo.z - half.z && center.z < hi.z + half.z;
}

bool isWalkable(const glm::vec3& normal, float cosSlopeLimit)
{
    return normal.y >= cosSlopeLimit;
}

// 支撑面：地面 + 脚底上下 snap/stepHeight 范围内的盒子顶面，取最高者。
bool findSupport(const CollisionWorld& world, const glm::vec3& feet, float radius,
                 const CharacterMoveSettings& settings, float& outHeight,
                 glm::vec3& outNormal)
{
    bool found = false;
    float best = -std::numeric_limits<float>::infinity();
    glm::vec3 normal(0.0f, 1.0f, 0.0f);

    float groundHeight = 0.0f;
    glm::vec3 groundNormal(0.0f, 1.0f, 0.0f);
    if (world.sampleGround(feet.x, feet.z, groundHeight, groundNormal))
    {
        found = true;
        best = groundHeight;
        normal = groundNormal;
    }

    const float low = feet.y - settings.groundSnapDistance;
    const float high = feet.y + settings.stepHeight;
    if (high > low)
    {
        const glm::vec3 probeCenter(feet.x, (low + high) * 0.5f, feet.z);
        const glm::vec3 probeHalf(radius, (high - low) * 0.5f, radius);
        CollisionBox candidates[kMaxCandidates];
        const std::size_t count =
            world.collectBoxes(probeCenter, probeHalf, candidates, kMaxCandidates);
        for (std::size_t i = 0; i < std::min(count, kMaxCandidates); ++i)
        {
            const float top = candidates[i].center.y + candidates[i].halfExtents.y;
            if (top < low || top > high)
                continue;
            if (top > best)
            {
                best = top;
                found = true;
                normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }
    }

    if (found)
    {
        outHeight = best;
        outNormal = normal;
    }
    return found;
}

} // namespace

CharacterMoveOutput moveCharacter(const CollisionWorld& world,
                                  const CharacterShape& shape,
                                  const CharacterMoveSettings& settings,
                                  const glm::vec3& start,
                                  const glm::vec3& delta,
                                  bool wasGrounded)
{
    CharacterMoveOutput out;
    out.position = start;

    const glm::vec3 half(shape.radius, std::max(shape.height, 0.2f) * 0.5f, shape.radius);
    const float cosSlopeLimit = std::cos(glm::radians(settings.maxSlopeAngle));
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    float groundHeight = 0.0f;
    glm::vec3 groundNormal = up;
    const bool hasGround = world.sampleGround(start.x, start.z, groundHeight, groundNormal);

    bool grounded = wasGrounded && hasGround && isWalkable(groundNormal, cosSlopeLimit);
    out.groundHeight = hasGround ? groundHeight : start.y;
    out.groundNormal = hasGround ? groundNormal : up;

    glm::vec3 remaining = delta;
    // 站在可行走坡面上时，把水平位移投影到坡面，否则每一步都会"悬空"再被
    // 地面吸附，产生上下抖动。投影后上坡会自然变慢（符合直觉）。
    if (grounded && std::abs(groundNormal.y) < 0.999f)
        remaining -= groundNormal * glm::dot(remaining, groundNormal);

    glm::vec3 position = start;

    for (uint32_t iteration = 0; iteration < settings.maxIterations; ++iteration)
    {
        if (glm::dot(remaining, remaining) < 1.0e-10f)
            break;

        out.iterations = iteration + 1;

        const glm::vec3 center = position + glm::vec3(0.0f, half.y, 0.0f);
        CollisionBox candidates[kMaxCandidates];
        const std::size_t found = world.collectBoxes(
            center, half + glm::vec3(glm::length(remaining)), candidates, kMaxCandidates);
        const std::size_t usable = std::min(found, kMaxCandidates);

        float bestTime = 1.0f;
        glm::vec3 bestNormal(0.0f);
        bool hit = false;

        for (std::size_t i = 0; i < usable; ++i)
        {
            float time = 0.0f;
            glm::vec3 normal(0.0f);
            if (sweepBoxAabb(center, half, remaining, candidates[i], time, normal) &&
                time < bestTime)
            {
                bestTime = time;
                bestNormal = normal;
                hit = true;
            }
        }

        // 地面当成平面扫掠：只在下落、且终点真的会陷进地面时才生效。
        // 判据用"终点脚底 vs 终点地面"，否则站在坡上时每一轮都在 t=0 命中同一
        // 个平面，永远走不动。
        if (remaining.y < 0.0f)
        {
            float destinationGround = 0.0f;
            glm::vec3 destinationNormal = up;
            const float destinationX = position.x + remaining.x;
            const float destinationZ = position.z + remaining.z;
            if (world.sampleGround(destinationX, destinationZ, destinationGround,
                                   destinationNormal))
            {
                const float bottom = center.y - half.y;
                if (bottom + remaining.y < destinationGround - settings.skin)
                {
                    const float time = std::clamp(
                        (destinationGround - settings.skin - bottom) / remaining.y,
                        0.0f, 1.0f);
                    if (time < bestTime)
                    {
                        bestTime = time;
                        bestNormal = destinationNormal;
                        hit = true;
                    }
                }
            }
        }

        if (!hit)
        {
            position += remaining;
            remaining = glm::vec3(0.0f);
            break;
        }

        out.blocked = true;
        const float advance = std::max(0.0f, bestTime - settings.skin);
        position += remaining * advance;
        glm::vec3 left = remaining * (1.0f - advance);

        // 上台阶：水平被挡、人还站在地上时，抬高 stepHeight 再试着往前。
        const bool horizontalBlocked = std::abs(bestNormal.y) < 0.5f;
        if (grounded && horizontalBlocked &&
            (std::abs(left.x) + std::abs(left.z)) > 1.0e-4f)
        {
            const glm::vec3 raised = position + up * settings.stepHeight;
            const glm::vec3 raisedCenter = raised + glm::vec3(0.0f, half.y, 0.0f);
            CollisionBox stepCandidates[kMaxCandidates];
            const std::size_t stepFound = world.collectBoxes(
                raisedCenter, half + glm::vec3(glm::length(left)),
                stepCandidates, kMaxCandidates);
            const std::size_t stepUsable = std::min(stepFound, kMaxCandidates);

            bool clear = true;
            for (std::size_t i = 0; i < stepUsable; ++i)
            {
                if (overlapsBox(raisedCenter, half, stepCandidates[i]))
                {
                    clear = false;
                    break;
                }
            }

            if (clear)
            {
                const glm::vec3 horizontal(left.x, 0.0f, left.z);
                float stepTime = 1.0f;
                glm::vec3 stepNormal(0.0f);
                bool stepHit = false;
                for (std::size_t i = 0; i < stepUsable; ++i)
                {
                    float time = 0.0f;
                    glm::vec3 normal(0.0f);
                    if (sweepBoxAabb(raisedCenter, half, horizontal, stepCandidates[i],
                                     time, normal) && time < stepTime)
                    {
                        stepTime = time;
                        stepNormal = normal;
                        stepHit = true;
                    }
                }

                const float stepAdvance = stepHit ? std::max(0.0f, stepTime - settings.skin)
                                                  : 1.0f;
                position = raised + horizontal * stepAdvance;
                if (stepHit)
                {
                    left = horizontal * (1.0f - stepAdvance);
                    bestNormal = stepNormal;
                }
                else
                {
                    left = glm::vec3(0.0f);
                }
            }
        }

        // 沿接触面滑动：去掉法线方向的分量，剩下的下一轮继续走。
        remaining = left - bestNormal * glm::dot(left, bestNormal);
        if (grounded && bestNormal.y > 0.0f &&
            !isWalkable(bestNormal, cosSlopeLimit))
        {
            grounded = false;
        }
    }

    if (glm::dot(remaining, remaining) > 1.0e-8f)
        out.exhausted = true;

    // 落地/贴地判定：支撑面 = 地面与脚下盒子顶面里最高的那个。
    // 只认可行走的支撑面；坡度太陡就保持离地，交给重力沿坡滑下去。
    float support = 0.0f;
    glm::vec3 supportNormal = up;
    if (findSupport(world, position, shape.radius, settings, support, supportNormal))
    {
        out.groundHeight = support;
        out.groundNormal = supportNormal;
        const float gap = support - position.y;
        const bool canSnap = gap <= settings.stepHeight + settings.skin &&
                             gap >= -settings.groundSnapDistance &&
                             (wasGrounded || gap >= -settings.skin);
        if (canSnap && isWalkable(supportNormal, cosSlopeLimit))
        {
            position.y = support;
            grounded = true;
        }
        else
        {
            grounded = false;
        }
    }
    else
    {
        grounded = false;
    }

    out.position = position;
    out.grounded = grounded;
    return out;
}

// ---------------------------------------------------------------------------
// 自检：解析几何世界（平面 / 斜坡 / 台阶 / 墙 / 天花板）
// ---------------------------------------------------------------------------

namespace
{

struct TestWorld : CollisionWorld
{
    std::vector<CollisionBox> boxes;
    float slopeTangent = 0.0f;   // 0 = 平地；>0 表示 h = x * tangent
    bool hasGround = true;

    std::size_t collectBoxes(const glm::vec3& center, const glm::vec3& halfExtents,
                             CollisionBox* out, std::size_t capacity) const override
    {
        std::size_t count = 0;
        for (const CollisionBox& box : boxes)
        {
            const glm::vec3 lo = box.center - box.halfExtents;
            const glm::vec3 hi = box.center + box.halfExtents;
            const bool overlap = center.x + halfExtents.x > lo.x &&
                                 center.x - halfExtents.x < hi.x &&
                                 center.y + halfExtents.y > lo.y &&
                                 center.y - halfExtents.y < hi.y &&
                                 center.z + halfExtents.z > lo.z &&
                                 center.z - halfExtents.z < hi.z;
            if (!overlap)
                continue;
            if (out != nullptr && count < capacity)
                out[count] = box;
            ++count;
        }
        return count;
    }

    bool sampleGround(float x, float z, float& outHeight,
                      glm::vec3& outNormal) const override
    {
        (void)z;
        if (!hasGround)
            return false;
        outHeight = slopeTangent * x;
        outNormal = glm::normalize(glm::vec3(-slopeTangent, 1.0f, 0.0f));
        return true;
    }
};

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "[Collision] selfTest FAIL: %s\n", message);
    return condition;
}

bool near(float a, float b, float tolerance = 1.0e-3f)
{
    return std::abs(a - b) <= tolerance;
}

} // namespace

bool characterCollisionSelfTest()
{
    bool ok = true;
    const CharacterShape shape{ 0.35f, 1.8f };
    CharacterMoveSettings settings;
    settings.maxIterations = 4;

    // 1) 平地直走：一步走完，落地，不耗尽迭代。
    {
        TestWorld world;
        const CharacterMoveOutput out = moveCharacter(world, shape, settings,
                                                      glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), true);
        ok &= expect(near(out.position.x, 1.0f), "flat walk should advance 1m");
        ok &= expect(out.grounded, "flat walk should stay grounded");
        ok &= expect(!out.exhausted, "flat walk should not exhaust iterations");
        ok &= expect(out.iterations == 1, "flat walk should need one iteration");
    }

    // 2) 45 度坡：贴着坡面走，连续 20 步高度单调上升且不抖动。
    {
        TestWorld world;
        world.slopeTangent = 1.0f;   // 45 度
        glm::vec3 position(0.0f);
        bool monotonic = true;
        bool followed = true;
        for (int step = 0; step < 20; ++step)
        {
            const CharacterMoveOutput out = moveCharacter(world, shape, settings, position,
                                                          glm::vec3(1.0f, 0.0f, 0.0f), true);
            if (out.position.y < position.y - 1.0e-4f)
                monotonic = false;
            position = out.position;
            if (!near(position.y, world.slopeTangent * position.x, 2.0e-3f))
                followed = false;
            if (!out.grounded || out.exhausted)
                followed = false;
        }
        ok &= expect(position.x > 1.0f, "45 degree slope should still make progress");
        ok &= expect(monotonic, "45 degree slope should rise monotonically");
        ok &= expect(followed, "45 degree slope should stay on the surface");
    }

    // 3) 60 度坡站不住：只给重力，必须出现沿坡向下的水平位移，且不算落地。
    {
        TestWorld world;
        world.slopeTangent = std::tan(glm::radians(60.0f));
        const glm::vec3 start(1.0f, world.slopeTangent * 1.0f, 0.0f);
        const CharacterMoveOutput out = moveCharacter(world, shape, settings, start,
                                                      glm::vec3(0.0f, -0.2f, 0.0f), true);
        ok &= expect(!out.grounded, "60 degree slope should not be walkable");
        ok &= expect(out.position.x < start.x - 1.0e-3f, "60 degree slope should slide downhill");
    }

    // 4) 上台阶 0.4m（小于 stepHeight）：能迈上去。
    {
        TestWorld world;
        world.boxes.push_back(CollisionBox{ glm::vec3(1.25f, 0.2f, 0.0f), glm::vec3(0.75f, 0.2f, 1.0f) });
        const CharacterMoveOutput out = moveCharacter(world, shape, settings,
                                                      glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), true);
        ok &= expect(near(out.position.y, 0.4f, 2.0e-3f), "0.4m step should be climbed");
        ok &= expect(out.position.x > 0.5f, "0.4m step should not block movement");
        ok &= expect(!out.exhausted, "step climb should not exhaust iterations");
    }

    // 5) 台阶 0.8m（大于 stepHeight）：过不去。
    {
        TestWorld world;
        world.boxes.push_back(CollisionBox{ glm::vec3(1.25f, 0.4f, 0.0f), glm::vec3(0.75f, 0.4f, 1.0f) });
        const CharacterMoveOutput out = moveCharacter(world, shape, settings,
                                                      glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), true);
        ok &= expect(out.blocked, "0.8m step should block");
        ok &= expect(out.position.x < 0.5f, "0.8m step should not be climbed");
        ok &= expect(!out.exhausted, "blocked step should settle within iterations");
    }

    // 6) 墙：停在墙前 radius 处。
    {
        TestWorld world;
        world.boxes.push_back(CollisionBox{ glm::vec3(1.1f, 1.0f, 0.0f), glm::vec3(0.1f, 1.0f, 2.0f) });
        const CharacterMoveOutput out = moveCharacter(world, shape, settings,
                                                      glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), true);
        ok &= expect(out.blocked, "wall should block");
        ok &= expect(near(out.position.x, 1.0f - shape.radius, 2.0e-2f), "wall should stop at radius");
        ok &= expect(out.iterations <= settings.maxIterations, "wall stop within iteration budget");
    }

    // 7) 天花板：向上顶住。
    {
        TestWorld world;
        world.boxes.push_back(CollisionBox{ glm::vec3(0.0f, 2.2f, 0.0f), glm::vec3(2.0f, 0.2f, 2.0f) });
        const CharacterMoveOutput out = moveCharacter(world, shape, settings,
                                                      glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), false);
        ok &= expect(out.blocked, "ceiling should block upward motion");
        ok &= expect(out.position.y < 1.2f, "ceiling should keep the head below it");
    }

    // 8) 迭代预算：任何用例都不应该耗尽 4 轮。
    {
        TestWorld world;
        world.boxes.push_back(CollisionBox{ glm::vec3(1.1f, 1.0f, 0.0f), glm::vec3(0.1f, 1.0f, 2.0f) });
        const CharacterMoveOutput out = moveCharacter(world, shape, settings,
                                                      glm::vec3(0.0f), glm::vec3(3.0f, -0.5f, 0.0f), true);
        ok &= expect(!out.exhausted, "slide along a wall should fit in four iterations");
        ok &= expect(out.iterations <= settings.maxIterations, "iteration counter within budget");
    }

    if (ok)
        std::printf("[Collision] characterSelfTest PASS | cases=8 slide=ok step=ok slope=ok\n");
    return ok;
}

} // namespace scene
