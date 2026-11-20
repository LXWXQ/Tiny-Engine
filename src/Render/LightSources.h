#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

// TM-13：场景里的点光源列表。
//
// 为什么先不做成 ECS 组件：这一步要验证的是"上千个灯能不能只算它附近的那几个"，
// 而组件会牵动序列化、编辑器面板与 30 个组件描述符。光源先作为渲染侧的数据
// 结构存在，等真的需要"在编辑器里摆灯"时再升级成组件 —— 那时结构体已经稳定了。
//
// 布局直接对着 GPU：positionRadius.xyz = 世界坐标、w = 影响半径；
// colorIntensity.rgb = 颜色、a = 强度。和 lighting.frag 里的 PointLight 一一对应。
struct GpuPointLight
{
    glm::vec4 positionRadius{ 0.f, 0.f, 0.f, 1.f };
    glm::vec4 colorIntensity{ 1.f, 1.f, 1.f, 1.f };
};

class LightSources
{
public:
    void clear() { lights_.clear(); }

    void addPointLight(const glm::vec3& position, float radius,
        const glm::vec3& color, float intensity);

    // 压力场景：把 count 个灯铺成一片。用于量"无聚簇 vs 聚簇"的差别，
    // 也用于截图回归 —— 参数固定时结果完全确定（不读时钟、不用随机数）。
    void buildStressField(uint32_t count);

    // 同上，但以 center 为中心、spread 为半径铺开。相机未必看着原点，
    // 压力场必须铺在相机前方，否则量到的是"一盏灯都没进视锥"。
    void buildStressField(uint32_t count, const glm::vec3& center, float spread);

    size_t size() const { return lights_.size(); }
    bool empty() const { return lights_.empty(); }
    const std::vector<GpuPointLight>& lights() const { return lights_; }

    // 纯逻辑校验：确定性、数值范围、网格排布不重叠到不合理。
    static bool selfTest();

private:
    std::vector<GpuPointLight> lights_;
};
