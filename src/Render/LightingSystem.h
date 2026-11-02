#pragma once
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Core/FrameInfo.h"
#include <memory>

// 光照合成逻辑。D-1(b) 起管线按附件格式创建（动态渲染）。
class LightingSystem
{
public:
    LightingSystem(Device& device, VkDescriptorSetLayout globalSetLayout);
    ~LightingSystem();

    LightingSystem(const LightingSystem&) = delete;
    LightingSystem& operator=(const LightingSystem&) = delete;

    void render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }

private:
    void createPipelineLayout(VkDescriptorSetLayout globalSetLayout);
    void createPipeline();

    Device& vulkanDevice;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> lightingPipeline;
};