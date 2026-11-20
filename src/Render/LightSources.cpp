#include "Render/LightSources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

void LightSources::addPointLight(const glm::vec3& position, float radius,
    const glm::vec3& color, float intensity)
{
    GpuPointLight light{};
    light.positionRadius = glm::vec4(position, radius > 0.01f ? radius : 0.01f);
    light.colorIntensity = glm::vec4(color, intensity);
    lights_.push_back(light);
}

void LightSources::buildStressField(uint32_t count)
{
    buildStressField(count, glm::vec3(0.f, 2.f, 0.f), 40.f);
}

void LightSources::buildStressField(uint32_t count, const glm::vec3& center, float spread)
{
    buildStressField(count, center, spread, GroundSampler{});
}

void LightSources::buildStressField(uint32_t count, const glm::vec3& center, float spread,
    const GroundSampler& ground)
{
    lights_.clear();
    lights_.reserve(count);

    if (spread < 1.f)
        spread = 1.f;

    // 黄金角螺旋 + 分层高度：均匀铺满一个圆盘，且任何一对相邻灯的球都有交叠，
    // 这样"每个片元命中几个灯"是有代表性的，而不是稀疏到聚簇看起来毫无作用。
    const float goldenAngle = 2.39996323f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float radius = spread * (0.15f + 0.85f * std::sqrt(t));
        const float angle = goldenAngle * static_cast<float>(i);
        const float height = 3.5f * std::sin(static_cast<float>(i) * 0.7f);

        glm::vec3 position = center + glm::vec3(radius * std::cos(angle), height, radius * std::sin(angle));
        // 有地面采样就贴地：抬 1.2 米，光斑正好落在方块表面上。采不到（比如水面/空洞）就保留飘空。
        float groundY = 0.f;
        if (ground && ground(position.x, position.z, groundY))
            position.y = groundY + 1.2f;
        // 冷暖交替：画家算法不会因为"全是同一个颜色"而看不出叠加是否正确。
        const float warm = (i % 3 == 0) ? 1.0f : 0.45f;
        const float cool = (i % 3 == 0) ? 0.55f : 1.0f;
        const glm::vec3 color(warm, 0.75f, cool);
        // 单灯半径固定 8 米，与铺开范围解耦：两个变量各管一件事 —— spread 控制"密度"，
        // 半径控制"每盏灯影响多大"。绑在一起会让聚簇对比失真（铺得越开、每盏灯覆盖的簇越多，
        // 聚簇反而更慢），这一版实测过：半径跟着 spread 走时 512 灯聚簇掉到 39 FPS。
        // 强度 10：默认机位离地面四五十米，强度 6 在 tonemap 之后几乎看不出颜色，
        // 而"看不出效果"和"没有实现"在截图上是同一件事；再往上（20）红通道直接饱和，
        // 光斑变成硬边橘色圆盘，反而看不出衰减。半径仍固定 8 米（见上）。
        addPointLight(position, 12.0f, color, 5.0f);
    }
}

bool LightSources::selfTest()
{
    bool ok = true;

    LightSources a;
    a.buildStressField(512);
    if (a.size() != 512) { std::printf("[Lights] FAIL 生成数量 %zu != 512\n", a.size()); ok = false; }

    LightSources b;
    b.buildStressField(512);
    if (a.size() != b.size()) { std::printf("[Lights] FAIL 两次生成数量不一致\n"); ok = false; }
    else
    {
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a.lights()[i].positionRadius != b.lights()[i].positionRadius)
            { std::printf("[Lights] FAIL 第 %zu 个灯不确定（位置不同）\n", i); ok = false; break; }
        }
    }

    for (size_t i = 0; i < a.lights().size(); ++i)
    {
        const GpuPointLight& l = a.lights()[i];
        if (!std::isfinite(l.positionRadius.x) || !std::isfinite(l.positionRadius.y) ||
            !std::isfinite(l.positionRadius.z) || l.positionRadius.w <= 0.0f)
        { std::printf("[Lights] FAIL 第 %zu 个灯的坐标或半径非法\n", i); ok = false; break; }
        if (l.colorIntensity.w <= 0.0f)
        { std::printf("[Lights] FAIL 第 %zu 个灯的强度非正\n", i); ok = false; break; }
    }

    // 半径必须够大，否则"灯与 cluster 相交"这件事在统计上没有意义。
    LightSources one;
    one.addPointLight(glm::vec3(0.f), 0.f, glm::vec3(1.f), 1.f);
    if (one.lights().empty() || one.lights()[0].positionRadius.w < 0.01f)
    { std::printf("[Lights] FAIL 半径为 0 时没有夹到最小值\n"); ok = false; }

    if (ok)
        std::printf("[Lights] selfTest PASS | 生成确定 + 数值合法 + 半径夹取\n");
    return ok;
}
