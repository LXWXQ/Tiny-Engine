#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

// F5：粒子前向混合通道。
//
// 延迟管线只处理不透明 G-Buffer，粒子必须按生命周期做 alpha 混合，所以单独
// 在 Lighting/Glass/Water 之后、TAA/Bloom 之前把实例化 billboard 画进 hdrScene。
// 深度只测试不写入；粒子之间按到相机距离降序绘制。
struct ParticlePushConstantData
{
    alignas(16) glm::vec4 cameraRight{ 1.0f, 0.0f, 0.0f, 0.0f };
    alignas(16) glm::vec4 cameraUp{ 0.0f, 1.0f, 0.0f, 0.0f };
};

struct ParticleInstance
{
    glm::vec4 positionSize{ 0.0f };   // xyz = 世界位置, w = 直径
    glm::vec4 color{ 0.0f };          // rgba
};

class ParticlePass : public IRenderPass
{
public:
    ParticlePass(Device& device, RenderGraph& graph, RenderResources& resources,
                 VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
                 VkFormat depthFormat);
    ~ParticlePass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Particle"; }

private:
    struct BuildResult
    {
        uint32_t emitters = 0;
        uint32_t alive = 0;
    };

    void createBuffers();
    void destroyBuffers();
    void createPipeline();
    BuildResult buildInstances(FrameInfo& frameInfo, const glm::vec3& cameraPosition);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager = nullptr;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> particlePipeline;

    // 每帧一份，避免两帧在飞时上一帧还在读的实例被覆写。
    std::array<VkBuffer, kFramesInFlight> instanceBuffers{};
    std::array<VmaAllocation, kFramesInFlight> instanceAllocations{};
    std::array<void*, kFramesInFlight> instanceMapped{};

    // 与 Scene::kMaxParticlesTotal 保持一致；模拟侧已先截断，这里只负责绘制。
    static constexpr uint32_t kMaxParticles = 16384;

    std::vector<ParticleInstance> instances;
    std::chrono::steady_clock::time_point lastLogTime{};
    uint32_t lastLoggedAlive = 0xFFFFFFFFu;
};
