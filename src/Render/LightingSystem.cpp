#include "LightingSystem.h"
#include "RHI/CommandUtils.h"
#include "Render/RenderResources.h"   // kHdrSceneFormat
#include "AssetPaths.h"
#include <stdexcept>

LightingSystem::LightingSystem(Device& device, VkDescriptorSetLayout globalSetLayout)
    : vulkanDevice(device)
{
    createPipelineLayout(globalSetLayout);
    createPipeline();
}

LightingSystem::~LightingSystem()
{
    vkDestroyPipelineLayout(vulkanDevice.getDevice(), pipelineLayout, nullptr);
}

void LightingSystem::createPipelineLayout(VkDescriptorSetLayout globalSetLayout)
{
    std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ globalSetLayout };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(descriptorSetLayouts.size());
    pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();

    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("LightingSystem: pipeline layout failed!");
}

void LightingSystem::createPipeline()
{
    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    // D-2：光照的输出不再是交换链，而是线性 HDR 中间目标。
    // 格式必须和 LightingPass 里 createTexture 给的完全一致，
    // 否则管线与附件的格式不匹配（验证层会直接报 VUID 错误）。
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.pipelineLayout = pipelineLayout;
    config.colorBlendInfo.attachmentCount = 1;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();

    lightingPipeline = std::make_unique<Pipeline>(vulkanDevice,
        SENTINEL_SHADER_DIR "lighting.vert.spv", SENTINEL_SHADER_DIR "lighting.frag.spv", config);
}

void LightingSystem::render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo)
{
    (void)commandBuffer;
    lightingPipeline->bind(frameInfo.commandBuffer);
    vkCmdDraw(frameInfo.commandBuffer, 3, 1, 0, 0);
}