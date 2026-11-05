#pragma once

#include "IRenderPass.h"
#include "RHI/RenderGraph.h"
#include "RHI/GBuffer.h"
#include "Render/RenderResources.h"
#include "Core/FrameInfo.h"

#include <array>
#include <glm/glm.hpp>

class DescriptorManager;
class GeometrySystem;
class LightingSystem;

// C1：半分辨率平面反射（第二遍延迟）。
//
// 为什么不是 SSR：远水（100m~4km）只有天空可反射，屏幕空间里根本没有对岸/山体
// 的像素。平面反射用镜像相机把水面以上的真实几何重新画一遍，远处也能得到
// 对岸轮廓，代价是额外一遍半分辨率 G-Buffer + 光照。
//
// 只在水面启用且场景里有地形/体素时注册；关闭开关时水面回退到环境贴图反射。
class ReflectionPass : public IRenderPass
{
public:
    ReflectionPass(Device& device, RenderGraph& graph, RenderResources& resources,
                   DescriptorManager* descriptorManager,
                   VkDescriptorSetLayout globalSetLayout, VkFormat depthFormat);
    ~ReflectionPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "PlanarReflection"; }

    // GeometrySystem 由 GeometryPass 在 initAll 里创建，因此用注入而不是构造参数。
    void setGeometrySystem(GeometrySystem* system) { geometrySystem = system; }

    // 每帧主 UBO 的副本 + 未抖动的 view/projection；反射矩阵在这里推导。
    void prepareFrame(const GlobalUbo& mainUbo, const glm::mat4& mainView,
                      const glm::mat4& unjitteredProjection, float waterLevel);

    // env/IBL/shadow/SSAO 是跨通道共享输入，由 Application 在重建全局描述符时注入。
    void setSharedInputs(const VkDescriptorImageInfo& envInfo,
                         const VkDescriptorImageInfo& irradianceInfo,
                         const VkDescriptorImageInfo& prefilteredInfo,
                         const VkDescriptorImageInfo& brdfLutInfo,
                         const VkDescriptorImageInfo& ssaoInfo,
                         VkSampler shadowSampler,
                         const std::array<VkImageView, kFramesInFlight>& shadowViews);
    void rebuildDescriptors();

    bool isActive() const { return active; }
    VkImageView getColorView(uint32_t frame) const;
    RenderGraph::TextureHandle getColorHandle(uint32_t frame) const;
    VkSampler getColorSampler() const { return colorSampler; }

private:
    void createTextures(VkExtent2D extent);
    void createUniformBuffers();
    void destroyUniformBuffers();

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager = nullptr;
    GeometrySystem* geometrySystem = nullptr;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    std::unique_ptr<LightingSystem> lightingSystem;

    std::array<std::array<RenderGraph::TextureHandle, 4>, kFramesInFlight> gbufferTextures{};
    std::array<RenderGraph::TextureHandle, kFramesInFlight> depthTextures{};
    std::array<RenderGraph::TextureHandle, kFramesInFlight> colorTextures{};

    std::array<VkBuffer, kFramesInFlight> uboBuffers{};
    std::array<VmaAllocation, kFramesInFlight> uboAllocations{};
    std::array<void*, kFramesInFlight> uboMapped{};

    std::array<VkDescriptorSet, kFramesInFlight> reflectionSets{};
    VkSampler colorSampler = VK_NULL_HANDLE;
    VkSampler gbufferSampler = VK_NULL_HANDLE;

    GlobalUbo frameUbo{};
    glm::mat4 mainView{ 1.0f };
    glm::mat4 projection{ 1.0f };
    float waterLevel = 2.0f;

    VkDescriptorImageInfo envInfo{};
    VkDescriptorImageInfo irradianceInfo{};
    VkDescriptorImageInfo prefilteredInfo{};
    VkDescriptorImageInfo brdfLutInfo{};
    VkDescriptorImageInfo ssaoInfo{};
    VkSampler shadowSampler = VK_NULL_HANDLE;
    std::array<VkImageView, kFramesInFlight> shadowViews{};

    VkExtent2D currentExtent{ 0, 0 };
    bool active = false;
    bool loggedInit = false;
    bool loggedFirstDraw = false;
};
