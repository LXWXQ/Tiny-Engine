#include "Pipeline.h"
#include "Scene/Model.h"
#include <fstream>
#include <stdexcept>
#include <iostream>

Pipeline::Pipeline(Device& device, const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo)
    : device(device) 
{
    createGraphicsPipeline(vertFilepath, fragFilepath, configInfo);
}

Pipeline::~Pipeline() 
{
    vkDestroyShaderModule(device.getDevice(), vertShaderModule, nullptr);
    vkDestroyShaderModule(device.getDevice(), fragShaderModule, nullptr);
    vkDestroyPipeline(device.getDevice(), graphicsPipeline, nullptr);
}

std::vector<char> Pipeline::readFile(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) 
    {
        throw std::runtime_error("failed to open file: " + filepath);
    }
    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
    return buffer;
}

void Pipeline::createGraphicsPipeline(const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo) 
{
    auto vertCode = readFile(vertFilepath);
    auto fragCode = readFile(fragFilepath);

    createShaderModule(vertCode, &vertShaderModule);
    createShaderModule(fragCode, &fragShaderModule);

    VkPipelineShaderStageCreateInfo shaderStages[2];
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShaderModule;
    shaderStages[0].pName = "main";
    shaderStages[0].flags = 0;
    shaderStages[0].pNext = nullptr;
    shaderStages[0].pSpecializationInfo = nullptr;

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragShaderModule;
    shaderStages[1].pName = "main";
    shaderStages[1].flags = 0;
    shaderStages[1].pNext = nullptr;
    shaderStages[1].pSpecializationInfo = nullptr;

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(configInfo.bindingDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = configInfo.bindingDescriptions.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(configInfo.attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = configInfo.attributeDescriptions.data();

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.pViewports = &configInfo.viewport;
    viewportInfo.scissorCount = 1;
    viewportInfo.pScissors = &configInfo.scissor;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &configInfo.inputAssemblyInfo;
    pipelineInfo.pViewportState = &viewportInfo;
    pipelineInfo.pRasterizationState = &configInfo.rasterizationInfo;
    pipelineInfo.pMultisampleState = &configInfo.multisampleInfo;
    pipelineInfo.pColorBlendState = &configInfo.colorBlendInfo;
    pipelineInfo.pDepthStencilState = &configInfo.depthStencilInfo;

    // 动态状态：viewport / scissor 改为在录制时用 vkCmdSetViewport / vkCmdSetScissor 设置。
    // pDynamicStates 只在本函数栈帧内有效（vkCreateGraphicsPipelines 会拷贝内容），
    // 因此用局部结构体，避免 PipelineConfigInfo 被拷贝后指针悬空。
    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(configInfo.dynamicStates.size());
    dynamicStateInfo.pDynamicStates = configInfo.dynamicStates.data();
    pipelineInfo.pDynamicState = configInfo.dynamicStates.empty() ? nullptr : &dynamicStateInfo;

    // 动态渲染：不设 renderPass，改为通过 pNext 挂 VkPipelineRenderingCreateInfo
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(configInfo.colorAttachmentFormats.size());
    renderingInfo.pColorAttachmentFormats = configInfo.colorAttachmentFormats.empty()
        ? nullptr : configInfo.colorAttachmentFormats.data();
    renderingInfo.depthAttachmentFormat = configInfo.depthAttachmentFormat;
    renderingInfo.stencilAttachmentFormat = configInfo.stencilAttachmentFormat;

    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.layout = configInfo.pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;   // 动态渲染，无 RenderPass
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) 
    {
        throw std::runtime_error("failed to create graphics pipeline!");
    }
}

void Pipeline::createShaderModule(const std::vector<char>& code, VkShaderModule* shaderModule) 
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    if (vkCreateShaderModule(device.getDevice(), &createInfo, nullptr, shaderModule) != VK_SUCCESS) 
    {
        throw std::runtime_error("failed to create shader module!");
    }
}

void Pipeline::bind(VkCommandBuffer commandBuffer) 
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
}

void Pipeline::defaultPipelineConfigInfo(PipelineConfigInfo& configInfo, uint32_t width, uint32_t height) 
{
    configInfo.inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    configInfo.inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    configInfo.inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    configInfo.viewport.x = 0.0f;
    configInfo.viewport.y = 0.0f;
    configInfo.viewport.width = static_cast<float>(width);
    configInfo.viewport.height = static_cast<float>(height);
    configInfo.viewport.minDepth = 0.0f;
    configInfo.viewport.maxDepth = 1.0f;

    configInfo.scissor.offset = {0, 0};
    configInfo.scissor.extent = {width, height};

    configInfo.rasterizationInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    configInfo.rasterizationInfo.depthClampEnable = VK_FALSE;
    configInfo.rasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
    configInfo.rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
    configInfo.rasterizationInfo.lineWidth = 1.0f;
    configInfo.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    configInfo.rasterizationInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
    configInfo.rasterizationInfo.depthBiasEnable = VK_FALSE;

    configInfo.multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    configInfo.multisampleInfo.sampleShadingEnable = VK_FALSE;
    configInfo.multisampleInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    configInfo.colorBlendAttachments = {colorBlendAttachment};
    configInfo.bindingDescriptions = {Vertex::getBindingDescription()};
    configInfo.attributeDescriptions = Vertex::getAttributeDescriptions();
    configInfo.colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    configInfo.colorBlendInfo.logicOpEnable = VK_FALSE;
    configInfo.colorBlendInfo.attachmentCount = static_cast<uint32_t>(configInfo.colorBlendAttachments.size());
    configInfo.colorBlendInfo.pAttachments = configInfo.colorBlendAttachments.data();

    configInfo.depthStencilInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    configInfo.depthStencilInfo.depthTestEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthWriteEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS;
    configInfo.depthStencilInfo.depthBoundsTestEnable = VK_FALSE;
    configInfo.depthStencilInfo.stencilTestEnable = VK_FALSE;

    // 统一启用动态 viewport / scissor：
    //   1. 消除原先 GeometrySystem 里「调用 vkCmdSetViewport 但管线未启用动态状态」的验证层错误
    //   2. 将来支持窗口缩放时无需重建全部管线
    // 注意：启用后，每个 Pass 在绑定管线后、绘制前都必须调用 cmdSetFullViewportAndScissor()
    configInfo.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
}
// ============================================================================
// ComputePipeline
// ============================================================================

ComputePipeline::ComputePipeline(Device& device, const std::string& compFilepath,
                                 VkPipelineLayout pipelineLayout)
    : device(device)
{
    const std::vector<char> code = readFile(compFilepath);
    createShaderModule(device, code, &shaderModule);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName = "main";

    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = pipelineLayout;

    if (vkCreateComputePipelines(device.getDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline!");
}

ComputePipeline::~ComputePipeline()
{
    if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device.getDevice(), pipeline, nullptr);
    if (shaderModule != VK_NULL_HANDLE)
        vkDestroyShaderModule(device.getDevice(), shaderModule, nullptr);
}

void ComputePipeline::bind(VkCommandBuffer commandBuffer)
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
}

std::vector<char> ComputePipeline::readFile(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("failed to open file: " + filepath);

    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

void ComputePipeline::createShaderModule(Device& device, const std::vector<char>& code,
                                         VkShaderModule* module)
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());

    if (vkCreateShaderModule(device.getDevice(), &info, nullptr, module) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute shader module!");
}
