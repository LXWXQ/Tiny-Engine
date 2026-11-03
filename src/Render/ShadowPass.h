#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"

#include <array>
#include <memory>

#include <glm/glm.hpp>

class RenderGraph;
class TerrainMeshArena;
class Pipeline;

// F6：方向光阴影（3 级联 CSM）。
//
// 设计取舍说明：
// 1) 阴影几何走 CPU 逐实体提交，而不是复用 GPU 驱动的间接提交。
//    GPU 批量只为一个视图（主相机）生成命令，阴影要每个级联一份；把剔除拆成
//    「每级联一次 dispatch」的工作量远大于收益 —— 阴影图只有 2048x2048、
//    而场景静态网格只有 103 个子网格。等体素/粒子真接进来（F3/F6 后续）再考虑。
// 2) 深度图与采样器由本 Pass 自己管理（手工屏障），不进渲染图：
//    图上一条纹理对应一个图层，3 层级联会让布局/屏障声明变复杂，
//    而这块资源只有「本 Pass 写、Lighting 读」一条依赖。
// 3) 级联矩阵写进全局 UBO（set 0 binding 0 的尾部），采样器是 set 0 binding 11，
//    这样 Lighting 不需要新描述符集，编辑器/后处理也不用改。
class ShadowPass : public IRenderPass
{
public:
    static constexpr uint32_t kCascadeCount = 3;
    static constexpr uint32_t kResolution = 2048;

    struct Cascade
    {
        glm::mat4 viewProjection{ 1.0f };
        float splitDistance = 0.0f;   // 该级的最远「视深度」，Lighting 用它选级
    };

    ShadowPass(Device& device, RenderGraph& graph, TerrainMeshArena* terrainMeshArena);
    ~ShadowPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Shadow"; }

    // 每帧在主相机矩阵确定后调用一次：算级联、记下本帧用哪套矩阵。
    void updateCascades(const glm::mat4& cameraView, const glm::mat4& cameraProjection,
                        const glm::vec3& lightDirection);

    const std::array<Cascade, kCascadeCount>& getCascades() const { return cascades; }

    VkImageView getShadowView(uint32_t frameInFlightIndex) const;
    VkSampler getShadowSampler() const { return shadowSampler; }

    void setEnabled(bool value) { enabled = value; }
    bool isEnabled() const { return enabled; }

    struct Stats
    {
        uint32_t drewEntities = 0;
        uint32_t drewTerrainChunks = 0;
        uint32_t drawCalls = 0;
        uint32_t triangles = 0;
    };
    const Stats& getStats() const { return stats; }

    // 级联数学的自检：不建 Device，纯 CPU。
    static bool selfTest();

private:
    struct Frame
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView arrayView = VK_NULL_HANDLE;
        std::array<VkImageView, kCascadeCount> layerViews{};
    };

    void createResources();
    void destroyResources();
    void createPipeline();

    void recordCascade(VkCommandBuffer cmd, uint32_t cascadeIndex, uint32_t frameInFlightIndex,
                       FrameInfo& frameInfo);
    static bool cascadeVisible(const glm::mat4& lightViewProjection, const glm::vec3& minimum,
                               const glm::vec3& maximum);

    Device& vulkanDevice;
    RenderGraph& renderGraph;
    TerrainMeshArena* terrainMeshArena = nullptr;

    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;
    std::array<Frame, kFramesInFlight> frames{};

    std::array<Cascade, kCascadeCount> cascades{};
    glm::vec3 cachedLightDirection{ 0.0f, -1.0f, 0.0f };
    bool cascadesDirty = true;
    bool enabled = true;
    Stats stats{};
    bool reported = false;
    // 首帧先把所有槽位转成 SHADER_READ_ONLY：即使阴影关掉，法线光照也会绑定这张
    // 描述符，validation 会拿描述符声明的布局和图像实际布局比较，UNDEFINED 会报错。
    bool layoutReady = false;
};
