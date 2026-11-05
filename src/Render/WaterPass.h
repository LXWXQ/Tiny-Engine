#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"
#include "Scene/Model.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

// 水面：Lighting 之后、TAA 之前的前向混合通道。
//
// 为什么不在 Geometry 里画：水面是透明表面，写进 G-Buffer 会让它后面的
// 不透明几何被深度挡住，而且延迟光照阶段没有地方做 Fresnel / 反射混合。
// 这里直接读 G-Buffer 的深度位置（inPosition）做厚度吸收，再采样环境贴图
// 作为反射，最后按 alpha 混合到已经算好光照的 HDR 图上。
//
// 几何：以相机 XZ 为中心、半径约 4km 的径向网格。顶点是局部坐标，shader
// 里再加 cameraPos.xz —— 网格始终跟着相机，远处一直铺到地平线，不需要
// 每帧重建顶点缓冲。顶点只做 4 个大波位移，像素级波纹在 fragment 里解析求法线。
struct WaterPushConstantData
{
    glm::vec4 params{ 0.0f };   // x=time(s), y=waterLevel, z=waveStrength, w=waveSpeed
    glm::vec4 params2{ 0.0f };  // x=farRadius, y=foamStrength, z=fogStart, w=fogDensity
    glm::vec4 params3{ 0.0f };  // x=planar reflection enabled
};

class ReflectionPass;

class WaterPass : public IRenderPass
{
public:
    WaterPass(Device& device, RenderGraph& graph, RenderResources& resources,
              VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
              VkFormat depthFormat);
    ~WaterPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Water"; }
    void setReflectionPass(ReflectionPass* pass) { reflectionPass = pass; }

private:
    void createBuffers();
    void destroyBuffers();
    void createPipeline();

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager = nullptr;
    ReflectionPass* reflectionPass = nullptr;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> waterPipeline;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    void* vertexMapped = nullptr;

    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    void* indexMapped = nullptr;

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;

    // 环形网格：中心 1 个顶点 + ringCount 圈、每圈 segmentCount 个顶点。
    // 半径 r_i = 1.2*i + 0.15*i^2：近处约 1.35m 一格，160 圈后约 4km。
    static constexpr uint32_t kRingCount = 160;
    static constexpr uint32_t kSegmentCount = 192;
    static constexpr float kFirstRingRadius = 1.35f;
    static constexpr float kFarRadius = 4032.0f;

    // 没有累计时间字段，自己按 frameTime 累加（截断到 0.1s，避免暂停/断点后跳变）。
    float timeSeconds = 0.0f;
    bool loggedDisabled = false;
    bool loggedFirstDraw = false;
};
