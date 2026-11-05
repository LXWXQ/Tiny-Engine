#include "PresentPass.h"
#include "AssetPaths.h"
#include <stdexcept>

PresentPass::PresentPass(Device& device, Swapchain& swapchain, RenderGraph& graph,
                         RenderResources& resources, DescriptorManager* descMgr,
                         VkDescriptorSetLayout globalSetLayout)
    : device(device), swapchain(swapchain), renderGraph(graph),
      resources(resources), descriptorManager(descMgr),
      globalSetLayout(globalSetLayout)
{
}

PresentPass::~PresentPass()
{
    pipeline.reset();
    noBloomPipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (noBloomPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), noBloomPipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (noBloomSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), noBloomSetLayout, nullptr);
    if (sampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), sampler, nullptr);
    if (positionSampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), positionSampler, nullptr);
}

void PresentPass::init()
{
    createDescriptorLayout();

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &si, nullptr, &sampler) != VK_SUCCESS)
        throw std::runtime_error("PresentPass: color sampler creation failed");

    // World-position data must not be interpolated across silhouettes.
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device.getDevice(), &si, nullptr, &positionSampler) != VK_SUCCESS)
        throw std::runtime_error("PresentPass: position sampler creation failed");

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        sets[frame] = descriptorManager->allocateSet(setLayout);
        noBloomSets[frame] = descriptorManager->allocateSet(noBloomSetLayout);
        updateBloomDescriptor(frame);
        updateNoBloomDescriptor(frame);
    }

    resources.hdrSampler = sampler;
    createPipeline();
}

void PresentPass::updateBloomDescriptor(uint32_t frame)
{
    VkDescriptorImageInfo infos[3] = {
        { sampler, renderGraph.getView(resources.taaOutput[frame]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { sampler, renderGraph.getView(resources.bloomResult[frame]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { positionSampler, renderGraph.getView(resources.gbPosition[frame]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };

    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = sets[frame];
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device.getDevice(), 3, writes, 0, nullptr);
}

void PresentPass::updateNoBloomDescriptor(uint32_t frame)
{
    VkDescriptorImageInfo infos[2] = {
        { sampler, renderGraph.getView(resources.taaOutput[frame]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { positionSampler, renderGraph.getView(resources.gbPosition[frame]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };

    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = noBloomSets[frame];
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device.getDevice(), 2, writes, 0, nullptr);
}

void PresentPass::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding bloomBindings[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bloomBindings[i].binding = i;
        bloomBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bloomBindings[i].descriptorCount = 1;
        bloomBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo bloomInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    bloomInfo.bindingCount = 3;
    bloomInfo.pBindings = bloomBindings;
    if (vkCreateDescriptorSetLayout(device.getDevice(), &bloomInfo, nullptr, &setLayout) != VK_SUCCESS)
        throw std::runtime_error("PresentPass: bloom descriptor layout creation failed");

    VkDescriptorSetLayoutBinding noBloomBindings[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        noBloomBindings[i].binding = i;
        noBloomBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        noBloomBindings[i].descriptorCount = 1;
        noBloomBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo noBloomInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    noBloomInfo.bindingCount = 2;
    noBloomInfo.pBindings = noBloomBindings;
    if (vkCreateDescriptorSetLayout(device.getDevice(), &noBloomInfo, nullptr, &noBloomSetLayout) != VK_SUCCESS)
        throw std::runtime_error("PresentPass: no-bloom descriptor layout creation failed");
}

void PresentPass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(PresentPush);

    VkDescriptorSetLayout bloomSetLayouts[] = { globalSetLayout, setLayout };
    VkPipelineLayoutCreateInfo bloomLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    bloomLayoutInfo.setLayoutCount = 2;
    bloomLayoutInfo.pSetLayouts = bloomSetLayouts;
    bloomLayoutInfo.pushConstantRangeCount = 1;
    bloomLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &bloomLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("PresentPass: bloom pipeline layout creation failed");

    VkDescriptorSetLayout noBloomSetLayouts[] = { globalSetLayout, noBloomSetLayout };
    VkPipelineLayoutCreateInfo noBloomLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    noBloomLayoutInfo.setLayoutCount = 2;
    noBloomLayoutInfo.pSetLayouts = noBloomSetLayouts;
    noBloomLayoutInfo.pushConstantRangeCount = 1;
    noBloomLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &noBloomLayoutInfo, nullptr, &noBloomPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("PresentPass: no-bloom pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.colorAttachmentFormats = { swapchain.getSwapChainImageFormat() };
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    config.pipelineLayout = pipelineLayout;
    pipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "present.frag.spv", config);

    config.pipelineLayout = noBloomPipelineLayout;
    noBloomPipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "present_no_bloom.frag.spv", config);

    pipelineSwapchainFormat = swapchain.getSwapChainImageFormat();
}

void PresentPass::resize(VkExtent2D /*extent*/)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        updateBloomDescriptor(frame);
        updateNoBloomDescriptor(frame);
    }

    if (pipelineSwapchainFormat != swapchain.getSwapChainImageFormat())
    {
        pipeline.reset();
        noBloomPipeline.reset();
        if (pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (noBloomPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device.getDevice(), noBloomPipelineLayout, nullptr);
            noBloomPipelineLayout = VK_NULL_HANDLE;
        }
        createPipeline();
    }
}

void PresentPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;
    const bool bloomEnabled = frameInfo.settings.bloomEnabled;
    auto& pass = graph.addGraphicsPass("Present");

    pass.read(resources.taaOutput[frame]);
    if (bloomEnabled)
        pass.read(resources.bloomResult[frame]);
    pass.read(resources.gbPosition[frame]);

    // 输出目标二选一：编辑器离屏视口 or 交换链。
    // 视口纹理按 frame slot 各一张 —— 用 frameInfo.frameInFlightIndex 索引，
    // 与 UiCompositePass 读的那张严格对齐。
    // 两者格式相同（都用交换链格式），所以 present.frag 里手写的 gamma 编码
    // 在两条路径上的语义完全一致，不会出现"编辑器里亮一点"这种差异。
    const RenderGraph::TextureHandle target = resources.editorViewportEnabled
        ? resources.editorViewport[frame]
        : resources.swapchainImages[frameInfo.swapchainImageIndex];
    pass.writeColor(target, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

    PresentPush push{};
    push.exposure = frameInfo.settings.exposure;
    push.bloomIntensity = bloomEnabled ? frameInfo.settings.bloomIntensity : 0.0f;
    push.bloomEnabled = bloomEnabled ? 1.0f : 0.0f;
    push.motionBlurEnabled = frameInfo.settings.motionBlurEnabled ? 1.0f : 0.0f;
    push.motionBlurStrength = frameInfo.settings.motionBlurStrength;
    push.motionBlurMaxRadius = frameInfo.settings.motionBlurMaxRadius;
    push.motionBlurFrame = static_cast<float>(frameInfo.frameNumber % 4096u);

    const VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
    pass.execute([this, push, frame, bloomEnabled, globalSet](VkCommandBuffer cmd)
    {
        if (bloomEnabled)
        {
            pipeline->bind(cmd);
            VkDescriptorSet descriptorSets[] = { globalSet, sets[frame] };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                0, 2, descriptorSets, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
        }
        else
        {
            noBloomPipeline->bind(cmd);
            VkDescriptorSet descriptorSets[] = { globalSet, noBloomSets[frame] };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, noBloomPipelineLayout,
                0, 2, descriptorSets, 0, nullptr);
            vkCmdPushConstants(cmd, noBloomPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
        }

        vkCmdDraw(cmd, 3, 1, 0, 0);

        // 离屏模式（编辑器）下 ImGui 由 UiCompositePass 绘制 —— 它需要先采样
        // 本 Pass 刚写好的视口纹理，两件事不能在同一个 Pass 里做。
        if (renderImGui && imguiSystem != nullptr)
            imguiSystem->renderDrawData(cmd);
    });
}
