#include "SunFlarePass.h"
#include "AssetPaths.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

static_assert(sizeof(SunFlarePass::PushConstants) == 128,
              "SunFlarePass::PushConstants 必须与 sunflare.frag 的 push_constant 等大");

SunFlarePass::SunFlarePass(Device& device, RenderGraph& graph, RenderResources& resources,
                           VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager)
    : device(device), renderGraph(graph), resources(resources),
      globalSetLayout(globalSetLayout), descriptorManager(descriptorManager)
{
}

SunFlarePass::~SunFlarePass()
{
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)      vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (depthSampler != VK_NULL_HANDLE)   vkDestroySampler(device.getDevice(), depthSampler, nullptr);
}

void SunFlarePass::init()
{
    createDescriptorLayout();
    createSampler();
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        sets[frame] = descriptorManager->allocateSet(setLayout);
        updateDescriptor(frame);
    }
    createPipeline();
    std::cout << "[Sun] initialized | ghostLimit=8" << std::endl;
}

void SunFlarePass::createDescriptorLayout()
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
        throw std::runtime_error("SunFlarePass: 描述符集布局创建失败");
}

void SunFlarePass::createSampler()
{
    // 深度只做“这一点是不是天空”的判定，用最近邻 + clamp 就够；
    // 线性过滤会把山脊和天空平均出一个假的 0.5，反而让日盘边缘抖。
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &info, nullptr, &depthSampler) != VK_SUCCESS)
        throw std::runtime_error("SunFlarePass: 深度采样器创建失败");
}

void SunFlarePass::updateDescriptor(uint32_t frame)
{
    VkDescriptorImageInfo depthInfo{ depthSampler, renderGraph.getView(resources.gbDepth[frame]),
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets[frame];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &depthInfo;
    vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);
}

void SunFlarePass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    const VkDescriptorSetLayout layouts[2] = { globalSetLayout, setLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("SunFlarePass: 管线布局创建失败");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    // 纯加色：把太阳能量叠到已经完成 TAA 的 HDR 上。alpha 用 ZERO/ONE
    // 保持不变 —— taaOutput 的 alpha 存世界半径，后面的 Bloom 不读它，
    // 但保留原语义，避免以后有人把光晕插到 TAA 之前时踩坑。
    VkPipelineColorBlendAttachmentState& blend = config.colorBlendAttachments[0];
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    pipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "sunflare.frag.spv", config);
}

void SunFlarePass::resize(VkExtent2D /*extent*/)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        updateDescriptor(frame);
}

void SunFlarePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const EngineSettings& s = frameInfo.settings;
    const uint32_t frame = frameInfo.frameInFlightIndex;

    if (!s.sunEnabled)
    {
        active = false;
        if (!loggedDisabled && !s.sunEnabled)
        {
            loggedDisabled = true;
            std::cout << "[Sun] disabled | settings.sunEnabled=0" << std::endl;
        }
        return;
    }

    active = true;

    glm::vec3 lightDirection = glm::vec3(s.directionalLightDir);
    if (glm::length(lightDirection) < 1e-4f)
        lightDirection = glm::vec3(0.0f, -1.0f, 0.0f);
    lightDirection = glm::normalize(lightDirection);
    const glm::vec3 toSun = -lightDirection;

    const glm::vec3 camera = frameInfo.cameraTransform != nullptr
        ? frameInfo.cameraTransform->getTranslation()
        : glm::vec3(0.0f);
    const glm::mat4 viewProjection = frameInfo.projectionMatrix * frameInfo.viewMatrix;
    const glm::vec4 sunClip = viewProjection * glm::vec4(camera + toSun * 10000.0f, 1.0f);

    float sunU = 0.5f;
    float sunV = 0.5f;
    float inFront = 0.0f;
    if (std::abs(sunClip.w) > 1e-5f && sunClip.w > 0.0f)
    {
        const glm::vec2 ndc = glm::vec2(sunClip.x, sunClip.y) / sunClip.w;
        sunU = ndc.x * 0.5f + 0.5f;
        sunV = ndc.y * 0.5f + 0.5f;
        inFront = 1.0f;
    }

    const float aspect = std::abs(frameInfo.projectionMatrix[0][0]) > 1e-6f
        ? std::abs(frameInfo.projectionMatrix[1][1] / frameInfo.projectionMatrix[0][0])
        : 1.7777778f;

    auto& pass = graph.addGraphicsPass("SunFlare");
    pass.read(resources.gbDepth[frame]);
    pass.writeColor(resources.taaOutput[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    PushConstants push{};
    push.inverseProjectionView = glm::inverse(viewProjection);
    push.sunDirectionRadius = glm::vec4(toSun, glm::radians(std::max(s.sunAngularRadius, 0.05f)));
    push.params0 = glm::vec4(s.sunIntensity, s.sunGlow, s.sunFlareIntensity,
                             s.sunOcclusion ? 1.0f : 0.0f);
    push.params1 = glm::vec4(float(s.sunFlareGhosts), aspect, sunU, sunV);
    push.color = glm::vec4(glm::vec3(s.sunColor), inFront);

    const VkDescriptorSet globalSet = descriptorManager->getGlobalSet(frame);
    const VkDescriptorSet localSet = sets[frame];
    if (!loggedFirstDraw)
    {
        loggedFirstDraw = true;
        std::cout << "[Sun] first draw | intensity=" << s.sunIntensity
                  << " angular=" << s.sunAngularRadius
                  << " flare=" << s.sunFlareIntensity
                  << " ghosts=" << s.sunFlareGhosts
                  << " inFront=" << inFront << "\n";
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
