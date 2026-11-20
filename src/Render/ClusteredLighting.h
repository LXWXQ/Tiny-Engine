#pragma once
#include "Render/LightSources.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

// TM-13：CPU 侧聚簇（clustered）光源筛选。
//
// 做法是教科书的那一套：把视锥按屏幕切成 tile（x/y）、按深度切成 slice（z），
// 每个 cluster 是一个视空间 AABB；球与 AABB 相交就把灯记进这一簇。
// 片元着色器只需要「我在哪一簇」，然后遍历那一簇里的灯。
//
// 为什么先 CPU 建：一帧只有几千簇 × 几千灯的测试量级，build 一次在百微秒量级，
// 而先做 compute 版本会把"聚簇是否真的减少计算"这个问题掩盖在管线问题里。
// 结构与 GPU 侧一一对应，将来搬到 compute 时只换生产者，不改消费者。

struct ClusterParamsGpu
{
    glm::mat4 inverseProjection{ 1.f };   // 64 字节
    glm::vec4 screenParams{ 0.f };        // xy = 渲染分辨率, z = near, w = far
    glm::uvec4 grid{ 0, 0, 0, 0 };        // xyz = 簇分辨率, w = 每簇上限
};

struct ClusterRange
{
    uint32_t offset = 0;
    uint32_t count = 0;
};

class ClusteredLighting
{
public:
    static constexpr uint32_t kGridX = 16;
    static constexpr uint32_t kGridY = 9;
    static constexpr uint32_t kGridZ = 24;
    static constexpr uint32_t kClusterCount = kGridX * kGridY * kGridZ;   // 3456
    static constexpr uint32_t kMaxLightsPerCluster = 256;

    void build(const std::vector<GpuPointLight>& lights, const glm::mat4& view,
        const glm::mat4& projection, float nearZ, float farZ,
        uint32_t width, uint32_t height);

    const ClusterParamsGpu& params() const { return params_; }
    const std::vector<ClusterRange>& ranges() const { return ranges_; }
    const std::vector<uint32_t>& indices() const { return indices_; }

    uint32_t dropped() const { return dropped_; }        // 因超过每簇上限被丢掉的条目
    uint32_t usedClusters() const { return usedClusters_; }
    uint32_t maxClusterSize() const { return maxClusterSize_; }
    uint32_t buildCount() const { return builds_; }

    // 供着色器与自检共用的两条数学：屏幕射线 -> 视空间点、球与 AABB 相交。
    static glm::vec3 viewRayPoint(const glm::mat4& inverseProjection,
        float ndcX, float ndcY, float viewDepth);
    static bool sphereIntersectsAabb(const glm::vec3& center, float radius,
        const glm::vec3& lo, const glm::vec3& hi);

    static uint32_t clusterIndex(uint32_t x, uint32_t y, uint32_t z)
    {
        return (z * kGridY + y) * kGridX + x;
    }

    static bool selfTest();


private:
    ClusterParamsGpu params_{};
    std::vector<ClusterRange> ranges_;
    std::vector<uint32_t> indices_;
    // 每帧复用：每簇的视空间 AABB 与每簇的命中共计数（避免每帧重新分配）。
    std::vector<glm::vec3> boundsLo_;
    std::vector<glm::vec3> boundsHi_;
    std::vector<uint32_t> counts_;
    uint32_t dropped_ = 0;
    uint32_t usedClusters_ = 0;
    uint32_t maxClusterSize_ = 0;
    uint32_t builds_ = 0;
};
