#pragma once

#include "Device.h"
#include <string>
#include <vector>

struct PipelineConfigInfo 
{
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo;
    VkPipelineRasterizationStateCreateInfo rasterizationInfo;
    VkPipelineMultisampleStateCreateInfo multisampleInfo;
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
    VkPipelineColorBlendStateCreateInfo colorBlendInfo;
    VkPipelineDepthStencilStateCreateInfo depthStencilInfo;
    VkPipelineLayout pipelineLayout = nullptr;

    // Vulkan 1.3 动态渲染：不再需要 VkRenderPass，改为声明附件格式。
    // 管线会通过 VkPipelineRenderingCreateInfo 链进 pNext。
    std::vector<VkFormat> colorAttachmentFormats{};
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    std::vector<VkVertexInputBindingDescription> bindingDescriptions{};
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};

    // 动态状态。非空时这些状态不会被烘焙进管线，必须在录制命令时显式设置，
    // 否则行为未定义（验证层的 VUID-vkCmdSetViewport-None-08971 等）。
    // 本项目所有图形管线统一启用 VIEWPORT + SCISSOR，见 defaultPipelineConfigInfo()。
    std::vector<VkDynamicState> dynamicStates{};
};

class Pipeline 
{
public:
    Pipeline(Device& device, const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void bind(VkCommandBuffer commandBuffer);

    static void defaultPipelineConfigInfo(PipelineConfigInfo& configInfo, uint32_t width, uint32_t height);

private:
    static std::vector<char> readFile(const std::string& filepath);
    void createGraphicsPipeline(const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo);
    void createShaderModule(const std::vector<char>& code, VkShaderModule* shaderModule);
    Device& device;
    VkPipeline graphicsPipeline;
    VkShaderModule vertShaderModule;
    VkShaderModule fragShaderModule;
};
// D-5：Compute 管线只包含一个 compute shader，不需要动态渲染附件 / 顶点输入 /
// 光栅状态。单独成类是为了让图形管线的配置结构保持干净，也避免在
// PipelineConfigInfo 里塞一堆 compute 用不到但必须置空的字段。
class ComputePipeline
{
public:
    ComputePipeline(Device& device, const std::string& compFilepath, VkPipelineLayout pipelineLayout);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    void bind(VkCommandBuffer commandBuffer);

private:
    static std::vector<char> readFile(const std::string& filepath);
    static void createShaderModule(Device& device, const std::vector<char>& code, VkShaderModule* module);

    Device& device;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
};
