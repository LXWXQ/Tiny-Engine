#include "SSRPass.h"
#include "AssetPaths.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

static_assert(sizeof(SSRPass::PushConstants) == 96,
              "SSRPass::PushConstants 必须与 ssr.frag 的 push_constant 等大");

namespace
{
    VkExtent2D halfExtent(VkExtent2D e)
    {
        return VkExtent2D{ std::max(1u, e.width / 2u), std::max(1u, e.height / 2u) };
    }
}

SSRPass::SSRPass(Device& device, RenderGraph& graph, RenderResources& resources,
                 VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager)
    : device(device), renderGraph(graph), resources(resources),
      globalSetLayout(globalSetLayout), descriptorManager(descriptorManager)
{
}

SSRPass::~SSRPass()
{
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)      vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE)        vkDestroySampler(device.getDevice(), sampler, nullptr);
}

void SSRPass::init()
{
    createTextures();
    createDescriptorLayout();
    createSampler();

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        sets[frame] = descriptorManager->allocateSet(setLayout);
        updateDescriptor(frame);
    }

    createPipeline();

    const VkExtent2D extent = renderGraph.getExtent(resources.ssrColor[0]);
    std::cout << "[SSR] initialized | extent=" << extent.width << "x" << extent.height
              << " steps=" << kStepCount << "\n";
}

void SSRPass::createTextures()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "SSR.Frame" + std::to_string(frame);
        desc.format = kHdrSceneFormat;
        desc.extent = halfExtent(renderGraph.getExtent(resources.hdrScene[frame]));
        // 只被 SSR 写、被 Water 读；半分辨率已经足够，反射本来就要软一点。
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resources.ssrColor[frame] = renderGraph.createTexture(desc);
    }
}

void SSRPass::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    info.bindingCount = 1;
    info.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device.getDevice(), &info, nullptr, &setLayout) != VK_SUCCESS)
        throw std::runtime_error("SSRPass: 描述符集布局创建失败");
}

void SSRPass::createSampler()
{
    // 命中点会落在半分辨率像素之间，线性过滤顺便做一次 2x2 软化。
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;

    if (vkCreateSampler(device.getDevice(), &info, nullptr, &sampler) != VK_SUCCESS)
        throw std::runtime_error("SSRPass: 采样器创建失败");
}

void SSRPass::updateDescriptor(uint32_t frame)
{
    VkDescriptorImageInfo imageInfo{ sampler, renderGraph.getView(resources.hdrScene[frame]),
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = sets[frame];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);
}

void SSRPass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);

    const VkDescriptorSetLayout layouts[2] = { globalSetLayout, setLayout };

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;

    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("SSRPass: 管线布局创建失败");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    pipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "ssr.frag.spv", config);
}

VkImageView SSRPass::getColorView(uint32_t frame) const
{
    return renderGraph.getView(resources.ssrColor[frame]);
}

void SSRPass::resize(VkExtent2D extent)
{
    const VkExtent2D half = halfExtent(extent);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        renderGraph.resizeOwnedTexture(resources.ssrColor[frame], half);
        updateDescriptor(frame);
    }
}

void SSRPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const EngineSettings& s = frameInfo.settings;
    const uint32_t frame = frameInfo.frameInFlightIndex;

    const bool hasTerrainOrVoxel =
        frameInfo.registry.view<TerrainComponent>().size() > 0 ||
        frameInfo.registry.view<VoxelChunkComponent>().size() > 0;

    if (!s.ssrEnabled || !s.waterEnabled || !hasTerrainOrVoxel)
    {
        active = false;
        if (!loggedDisabled && !s.ssrEnabled)
        {
            loggedDisabled = true;
            std::cout << "[SSR] disabled | settings.ssrEnabled=0\n";
        }
        return;
    }

    active = true;

    auto& pass = graph.addGraphicsPass("SSR");
    pass.read(resources.hdrScene[frame]);
    pass.read(resources.gbPosition[frame]);
    pass.writeColor(resources.ssrColor[frame], VK_ATTACHMENT_LOAD_OP_DONT_CARE);

    PushConstants push{};
    // 与 HDR 的一致性：用未抖动的 projection，TAA jitter 只影响最终采样位置，
    // 不应该让反射射线跟着抖 —— 否则反射本身也会变成噪声源。
    push.inverseProjectionView = glm::inverse(frameInfo.projectionMatrix * frameInfo.viewMatrix);
    push.params = glm::vec4(s.ssrMaxDistance, 0.5f, float(kStepCount), 1.0f);

    const VkExtent2D extent = graph.getExtent(resources.ssrColor[frame]);
    push.water = glm::vec4(s.waterLevel, 1.0f / float(extent.width),
                           1.0f / float(extent.height), 0.0f);

    const VkDescriptorSet globalSet = descriptorManager->getGlobalSet(frame);
    const VkDescriptorSet localSet = sets[frame];
    const bool first = !loggedFirstDraw;
    if (first)
    {
        loggedFirstDraw = true;
        std::cout << "[SSR] first draw | extent=" << extent.width << "x" << extent.height
                  << " waterLevel=" << s.waterLevel
                  << " maxDistance=" << s.ssrMaxDistance
                  << " planarReflection=" << (s.waterPlanarReflection ? 1 : 0) << "\n";
    }

    pass.execute([this, push, globalSet, localSet](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            0, 1, &globalSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            1, 1, &localSet, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });
}
