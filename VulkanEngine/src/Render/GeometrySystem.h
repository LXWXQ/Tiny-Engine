#pragma once
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Core/FrameInfo.h"
#include <memory>
#include <unordered_set>

// 几何渲染逻辑：持有 3 条管线（标准物体 / 天空盒 / 网格）。
// D-1(b) 起管线按「附件格式」创建（动态渲染），不再需要 VkRenderPass。
class GeometrySystem
{
public:
    GeometrySystem(Device& device, VkFormat depthFormat,
                   VkDescriptorSetLayout globalSetLayout,
                   VkDescriptorSetLayout materialSetLayout);
    ~GeometrySystem();

    GeometrySystem(const GeometrySystem&) = delete;
    GeometrySystem& operator=(const GeometrySystem&) = delete;

    void render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }

private:
    void createPipelineLayout(VkDescriptorSetLayout globalSetLayout,
                              VkDescriptorSetLayout materialSetLayout);
    void createPipelines();

    Device& vulkanDevice;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> geometryPipeline;
    std::unique_ptr<Pipeline> skyboxPipeline;
    std::unique_ptr<Pipeline> gridPipeline;

    // 没有材质槽位的子网格会每帧走一次 fallback 分支。提示只发一次：
    // 每帧写 stderr 既刷屏，又会破坏"stderr 0 字节"的回归判据。
    std::unordered_set<uint32_t> warnedFallbackEntities;
};