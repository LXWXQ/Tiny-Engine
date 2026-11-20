#pragma once

#include "Render/IRenderPass.h"
#include "RHI/Device.h"

#include "Core/FrameInFlight.h"

#include <array>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

class DescriptorManager;
class RenderGraph;
class ComputePipeline;
struct RenderResources;

// F6-02：自动曝光。
//
// 每帧一次 compute dispatch，把 HDR 的平均对数亮度量化后 atomicAdd 进一个小 SSBO；
// CPU 在下一帧开头（同一帧槽再次轮到时，那一份已经提交完毕）读回，
// 再按 1 - exp(-dt * speed) 在对数域推进曝光值。
// 曝光值只走 FrameInfo::exposureOverride，**不写 SceneSettings::exposure** ——
// 后者会被 EngineConfig 存进 config/engine.json，等于替用户改配置。
//
// 默认关闭：默认画面必须与没有这个功能时逐像素一致。
class AutoExposurePass : public IRenderPass
{
public:
    AutoExposurePass(Device& device, RenderGraph& graph, RenderResources& resources,
                     DescriptorManager* descriptorManager);
    ~AutoExposurePass() override;

    void init() override {}
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "AutoExposure.Reduce"; }

    // 每帧开头调用：读回两帧前的统计并推进曝光，返回本帧使用的曝光值。
    float update(uint32_t frameInFlightIndex, float deltaTime);

    void setEnabled(bool value) { enabled = value; }
    // 从场景里当前的曝光值起步，避免打开开关的那一帧画面跳一下。
    void seedExposure(float value) { if (value > 0.0f) exposure = value; }
    bool isEnabled() const { return enabled; }
    float currentExposure() const { return exposure; }
    float lastAverageLuminance() const { return averageLuminance; }
    uint32_t lastSampleCount() const { return sampleCount; }

    // 纯函数，供自检：在对数域按 1 - exp(-dt * speed) 靠近目标值。
    static float adaptExposure(float previous, float target, float deltaSeconds, float speed);
    static bool selfTest();

private:
    // 与 exposure_reduce.comp 的 ExposureStats 一致。
    struct Stats
    {
        uint32_t sumQ = 0;
        uint32_t count = 0;
        float pad0 = 0.0f;
        float pad1 = 0.0f;
    };

    struct PushConstants
    {
        glm::vec4 params{ 0.0f };
        uint32_t stride = 0;
        uint32_t pad[3]{ 0, 0, 0 };
    };

    void createBuffers();
    void createDescriptors();
    void createPipeline();

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager = nullptr;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> pipeline;

    std::array<VkBuffer, kFramesInFlight> statsBuffers{};
    std::array<VmaAllocation, kFramesInFlight> statsAllocations{};
    std::array<Stats*, kFramesInFlight> statsMapped{};
    std::array<VkDescriptorSet, kFramesInFlight> sets{};

    bool enabled = false;
    float exposure = 1.0f;
    float averageLuminance = 0.18f;
    uint32_t sampleCount = 0;
};
