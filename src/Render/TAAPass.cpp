#include "TAAPass.h"
#include "AssetPaths.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
constexpr uint32_t kBindingCurrentColor = 0;
constexpr uint32_t kBindingWorldPos = 1;
constexpr uint32_t kBindingHistory = 2;
}

TAAPass::TAAPass(Device& device, RenderGraph& graph, RenderResources& resources,
                 VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager)
    : device(device), renderGraph(graph), resources(resources),
      descriptorManager(descriptorManager), globalSetLayout(globalSetLayout)
{
}

TAAPass::~TAAPass()
{
    pipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), sampler, nullptr);
}

void TAAPass::init()
{
    createDescriptorLayout();
    createSampler();

    const VkExtent2D extent = renderGraph.getExtent(resources.hdrScene[0]);
    const VkFormat format = renderGraph.getFormat(resources.hdrScene[0]);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "TAA.Output.Frame" + std::to_string(frame);
        desc.format = format;
        desc.extent = extent;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.persistent = false;
        resources.taaOutput[frame] = renderGraph.createTexture(desc);
    }

    for (uint32_t i = 0; i < 2; ++i)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "TAA.History" + std::to_string(i);
        desc.format = format;
        desc.extent = extent;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.persistent = true;
        resources.taaHistory[i] = renderGraph.createTexture(desc);
    }

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
            sets[frame * 2 + readIndex] = descriptorManager->allocateSet(setLayout);
    }

    updateDescriptors();
    createPipeline();

    std::cout << "[TAA] initialized | " << extent.width << "x" << extent.height
              << " | history=2 | jitter=Halton(2,3)\n";
}

void TAAPass::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    info.bindingCount = 3;
    info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.getDevice(), &info, nullptr, &setLayout) != VK_SUCCESS)
        throw std::runtime_error("TAAPass: descriptor set layout creation failed");
}

void TAAPass::createSampler()
{
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &info, nullptr, &sampler) != VK_SUCCESS)
        throw std::runtime_error("TAAPass: sampler creation failed");
}

void TAAPass::updateDescriptors()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            VkDescriptorImageInfo infos[3] = {
                { sampler, renderGraph.getView(resources.hdrScene[frame]),
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sampler, renderGraph.getView(resources.gbPosition[frame]),
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sampler, renderGraph.getView(resources.taaHistory[readIndex]),
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            };

            VkWriteDescriptorSet writes[3]{};
            for (uint32_t i = 0; i < 3; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = sets[frame * 2 + readIndex];
                writes[i].dstBinding = i;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].descriptorCount = 1;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(device.getDevice(), 3, writes, 0, nullptr);
        }
    }
}

void TAAPass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(Push);

    VkDescriptorSetLayout layouts[2] = { globalSetLayout, setLayout };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("TAAPass: pipeline layout creation failed");

    const VkFormat format = renderGraph.getFormat(resources.taaOutput[0]);
    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.colorAttachmentFormats = { format, format };
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.pipelineLayout = pipelineLayout;

    // 默认配置只有一个颜色附件，这里要显式扩成两个，并且新元素的
    // colorWriteMask 必须设置，否则第二个输出会被静默丢掉。
    config.colorBlendAttachments.resize(2);
    for (VkPipelineColorBlendAttachmentState& attachment : config.colorBlendAttachments)
    {
        attachment.blendEnable = VK_FALSE;
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    config.colorBlendInfo.attachmentCount = 2;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    pipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "taa.frag.spv", config);
}

float TAAPass::halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0)
    {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

glm::vec2 TAAPass::prepareFrame(uint64_t frameNumber, VkExtent2D extent,
                                bool enabled, float jitterScale)
{
    previousJitterPixels = currentJitterPixels;

    const bool enabledChanged = enabled != lastEnabled;
    const bool scaleChanged = std::abs(jitterScale - lastJitterScale) > 1e-6f;
    if (enabledChanged || scaleChanged)
        resetHistory = true;

    if (enabledChanged)
        std::cout << "[TAA] " << (enabled ? "enabled" : "disabled") << "\n";
    lastEnabled = enabled;
    lastJitterScale = jitterScale;

    if (!enabled || extent.width == 0 || extent.height == 0)
    {
        currentJitterPixels = glm::vec2(0.0f);
        return currentJitterPixels;
    }

    const float scale = std::clamp(jitterScale, 0.0f, 1.5f);
    const uint32_t index = static_cast<uint32_t>(frameNumber % 64ull) + 1u;
    currentJitterPixels = glm::vec2(
        (halton(index, 2u) - 0.5f) * scale,
        (halton(index, 3u) - 0.5f) * scale);
    return currentJitterPixels;
}

void TAAPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;
    const uint32_t writeIndex = static_cast<uint32_t>(frameInfo.frameNumber & 1ull);
    const uint32_t readIndex = writeIndex ^ 1u;
    const VkExtent2D extent = graph.getExtent(resources.taaOutput[frame]);

    const bool valid = historyValid && !resetHistory;
    float weight = std::clamp(frameInfo.settings.taaBlend, 0.0f, 0.98f);
    if (!valid || !frameInfo.settings.taaEnabled)
        weight = 0.0f;

    auto& pass = graph.addGraphicsPass("TAA");
    pass.read(resources.hdrScene[frame]);
    pass.read(resources.gbPosition[frame]);
    pass.read(resources.taaHistory[readIndex]);
    pass.writeColor(resources.taaOutput[frame], VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    pass.writeColor(resources.taaHistory[writeIndex], VK_ATTACHMENT_LOAD_OP_DONT_CARE);

    Push push{};
    push.previousJitterUv = glm::vec2(
        previousJitterPixels.x / static_cast<float>(extent.width),
        previousJitterPixels.y / static_cast<float>(extent.height));
    push.texelSize = glm::vec2(
        1.0f / static_cast<float>(extent.width),
        1.0f / static_cast<float>(extent.height));
    push.historyWeight = weight;
    push.historyValid = valid ? 1.0f : 0.0f;

    const VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
    const VkDescriptorSet taaSet = sets[frame * 2 + readIndex];
    pass.execute([this, push, globalSet, taaSet](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        VkDescriptorSet descriptorSets[] = { globalSet, taaSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            0, 2, descriptorSets, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });

    historyValid = true;
    resetHistory = false;
}

void TAAPass::resize(VkExtent2D extent)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        renderGraph.resizeOwnedTexture(resources.taaOutput[frame], extent);
    for (uint32_t i = 0; i < 2; ++i)
        renderGraph.resizeOwnedTexture(resources.taaHistory[i], extent);

    updateDescriptors();

    historyValid = false;
    resetHistory = true;
    currentJitterPixels = glm::vec2(0.0f);
    previousJitterPixels = glm::vec2(0.0f);

    std::cout << "[TAA] resize | " << extent.width << "x" << extent.height
              << " | history reset\n";
}
