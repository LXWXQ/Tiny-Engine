#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 萤火虫：同一小块区域里 N 个自发光点光源。
//
// 和点光源压力场（LightSources）的区别在"看得见"这件事上：
// 压力场只往光照里加数，屏幕上不出现任何光源本体；萤火虫每个都是一个有尺寸、
// 有颜色的发光点，能直接看出"这里有 16 盏/32 盏灯在飘"，同时它们也真的照亮附近地面。
//
// 位置与颜色全部由确定性伪随机 + 时间函数决定：同一份参数在两次运行里摆位一致，
// 只有动画相位不同 —— 截图对比时不用再怀疑"是不是随机摆的"。
struct FireflySample
{
    glm::vec4 positionSize{ 0.0f };   // xyz = 世界位置, w = 发光点直径（米）
    glm::vec4 color{ 0.0f };          // rgb = 颜色, a = 当前亮度（0..1 之间呼吸）
    glm::vec4 light{ 0.0f };          // x = 照亮半径（米）, y = 当前强度
};

class Fireflies
{
public:
    // count 盏灯铺在 center 周围 regionRadius 的圆盘里（纵向只有 1/4 抖动，像贴地飞）。
    void configure(uint32_t count, const glm::vec3& center, float regionRadius,
        uint64_t seed = 20251120ull);

    // timeSeconds 只驱动动画（游走 + 呼吸），不改变布点。
    void update(float timeSeconds);

    const std::vector<FireflySample>& samples() const { return samples_; }
    size_t size() const { return samples_.size(); }
    bool empty() const { return samples_.empty(); }
    void clear() { samples_.clear(); seeds_.clear(); }
    uint32_t count() const { return static_cast<uint32_t>(seeds_.size()); }

    // 纯逻辑校验：数量、确定性、范围、颜色与亮度合法、呼吸不越界。
    static bool selfTest();

private:
    struct Seed
    {
        glm::vec3 base{ 0.0f };       // 布点中心
        glm::vec3 drift{ 0.0f };      // 游走幅度（每轴不同，避免同步摆）
        glm::vec3 phase{ 0.0f };      // 三轴相位
        glm::vec3 color{ 1.0f };      // 颜色（已归一）
        float radius = 5.0f;          // 照亮半径（米）
        float intensity = 1.5f;       // 亮度峰值
        float size = 0.5f;            // 发光点直径（米）
        float breath = 0.0f;          // 呼吸相位
    };

    std::vector<Seed> seeds_;
    std::vector<FireflySample> samples_;
};