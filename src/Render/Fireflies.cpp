#include "Render/Fireflies.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
// splitmix64：确定性伪随机，不引 <random>（它的分布实现跨编译器不保证一致）。
struct Rng
{
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next()
    {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // [0, 1)
    float unit()
    {
        return static_cast<float>(next() >> 40) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};
}

void Fireflies::configure(uint32_t count, const glm::vec3& center, float regionRadius, uint64_t seed)
{
    seeds_.clear();
    samples_.clear();
    seeds_.reserve(count);
    samples_.resize(count);

    if (regionRadius < 0.5f)
        regionRadius = 0.5f;

    Rng rng(seed);
    for (uint32_t i = 0; i < count; ++i)
    {
        // 圆盘内均匀撒点（sqrt 保证面积均匀，否则会往中心挤）。
        const float r = regionRadius * std::sqrt(rng.unit());
        const float a = rng.unit() * 6.2831853f;

        Seed s;
        s.base = center + glm::vec3(r * std::cos(a), rng.range(-2.0f, 2.0f), r * std::sin(a));
        s.drift = glm::vec3(rng.range(0.35f, 1.1f), rng.range(0.15f, 0.5f), rng.range(0.35f, 1.1f));
        s.phase = glm::vec3(rng.range(0.0f, 6.2831853f), rng.range(0.0f, 6.2831853f), rng.range(0.0f, 6.2831853f));

        // 冷暖两种：多数偏黄绿（真实萤火虫），少数偏青，避免整片一个颜色。
        // 注意：粒子着色器会把颜色乘 1.4 再混合，所以这里必须给"深一点"的饱和色，
        // 否则屏幕上看到的是白点 —— 白点没有"萤火虫"的样子，也分不出冷暖两群。
        const bool cool = rng.unit() < 0.25f;
        s.color = cool ? glm::vec3(0.25f, 0.95f, 0.80f) : glm::vec3(1.0f, 0.62f, 0.12f);

        s.radius = rng.range(6.0f, 9.0f);
        s.intensity = rng.range(1.6f, 2.8f);
        s.size = rng.range(0.42f, 0.85f);
        s.breath = rng.range(0.0f, 6.2831853f);

        seeds_.push_back(s);
    }

    update(0.0f);
}

void Fireflies::update(float timeSeconds)
{
    if (seeds_.empty())
        return;

    if (samples_.size() != seeds_.size())
        samples_.resize(seeds_.size());

    for (size_t i = 0; i < seeds_.size(); ++i)
    {
        const Seed& s = seeds_[i];
        // 三个轴用不同频率与相位：单摆看起来像在"抖"，多频叠加才像飘。
        const glm::vec3 offset(
            s.drift.x * std::sin(timeSeconds * 0.7f + s.phase.x),
            s.drift.y * std::sin(timeSeconds * 1.1f + s.phase.y),
            s.drift.z * std::cos(timeSeconds * 0.6f + s.phase.z));

        // 呼吸：0.55~1.0 之间，永不为 0 —— 全灭会让"到底有没有灯"又变成猜谜。
        const float breath = 0.55f + 0.45f * std::sin(timeSeconds * 1.7f + s.breath);

        FireflySample& out = samples_[i];
        out.positionSize = glm::vec4(s.base + offset, s.size * (0.85f + 0.3f * breath));
        out.color = glm::vec4(s.color, breath);
        // 照亮半径与亮度和发光点的直径是两件事：点只有几十厘米，光要铺几米。
        out.light = glm::vec4(s.radius, s.intensity * breath, 0.0f, 0.0f);
    }
}

bool Fireflies::selfTest()
{
    bool ok = true;
    const auto check = [&ok](bool cond, const char* what)
    {
        if (!cond)
        {
            std::printf("[Fireflies] selfTest FAIL | %s\n", what);
            ok = false;
        }
    };

    Fireflies a;
    a.configure(32, glm::vec3(1.0f, 2.0f, 3.0f), 12.0f, 20251120ull);
    check(a.size() == 32, "32 盏灯");

    Fireflies b;
    b.configure(32, glm::vec3(1.0f, 2.0f, 3.0f), 12.0f, 20251120ull);
    bool same = true;
    for (size_t i = 0; i < a.size(); ++i)
        same = same && a.samples()[i].positionSize == b.samples()[i].positionSize;
    check(same, "两次布点一致");

    bool inRange = true;
    bool colorOk = true;
    for (const FireflySample& s : a.samples())
    {
        const glm::vec3 d = glm::vec3(s.positionSize) - glm::vec3(1.0f, 2.0f, 3.0f);
        // 布点半径是 12，但动画会给每盏灯最多 1.1 m 的游走（纵向 0.5 m），
        // 纵向初始抖动 2 m —— 所以边界按 12 + 1.2 / 2 + 0.6 判。
        inRange = inRange && std::sqrt(d.x * d.x + d.z * d.z) <= 13.21f;
        inRange = inRange && std::fabs(d.y) <= 2.61f;
        colorOk = colorOk && s.color.r >= 0.0f && s.color.r <= 1.0f && s.color.g >= 0.0f &&
            s.color.g <= 1.0f && s.color.b >= 0.0f && s.color.b <= 1.0f;
        colorOk = colorOk && s.color.a > 0.0f && s.positionSize.w > 0.0f;
        colorOk = colorOk && s.light.x > 0.0f && s.light.y > 0.0f;
    }
    check(inRange, "位置在区域内");
    check(colorOk, "颜色与亮度合法");

    // 呼吸必须有起伏，否则动画等于没做；同时亮度不能归零。
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (int step = 0; step < 64; ++step)
    {
        a.update(static_cast<float>(step) * 0.13f);
        for (const FireflySample& s : a.samples())
        {
            lo = std::min(lo, s.color.a);
            hi = std::max(hi, s.color.a);
        }
    }
    check(hi > lo * 1.2f, "亮度有起伏");
    check(lo > 0.0f, "亮度不归零");

    if (ok)
        std::printf("[Fireflies] selfTest PASS | 布点确定 + 范围合法 + 呼吸不灭\n");
    return ok;
}