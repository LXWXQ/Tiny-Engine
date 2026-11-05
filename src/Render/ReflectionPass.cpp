#include "ReflectionPass.h"

#include "Core/DescriptorManager.h"
#include "Render/GeometrySystem.h"
#include "Render/LightingSystem.h"
#include "Scene/Components.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
VkExtent2D halfExtent(VkExtent2D extent)
{
    return VkExtent2D{
        std::max(1u, extent.width / 2u),
        std::max(1u, extent.height / 2u)
    };
}
}

ReflectionPass::ReflectionPass(Device& device, RenderGraph& graph, RenderResources& resources,
                               DescriptorManager* descriptorManager,
                               VkDescriptorSetLayout globalSetLayout, VkFormat depthFormat)
    : device(device), renderGraph(graph), resources(resources),
      descriptorManager(descriptorManager), globalSetLayout(globalSetLayout),
      depthFormat(depthFormat)
{
    uboBuffers.fill(VK_NULL_HANDLE);
    uboAllocations.fill(VK_NULL_HANDLE);
    uboMapped.fill(nullptr);
    reflectionSets.fill(VK_NULL_HANDLE);
    shadowViews.fill(VK_NULL_HANDLE);
}

ReflectionPass::~ReflectionPass()
{
    lightingSystem.reset();
    destroyUniformBuffers();

    if (colorSampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), colorSampler, nullptr);
    if (gbufferSampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), gbufferSampler, nullptr);
}

void ReflectionPass::init()
{
    VkExtent2D baseExtent{ 1, 1 };
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (frame < resources.hdrScene.size() && resources.hdrScene[frame].valid())
        {
            baseExtent = renderGraph.getExtent(resources.hdrScene[frame]);
            break;
        }
    }
    createTextures(halfExtent(baseExtent));
    createUniformBuffers();

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &samplerInfo, nullptr, &colorSampler) != VK_SUCCESS)
        throw std::runtime_error("ReflectionPass: color sampler creation failed");

    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device.getDevice(), &samplerInfo, nullptr, &gbufferSampler) != VK_SUCCESS)
        throw std::runtime_error("ReflectionPass: gbuffer sampler creation failed");

    lightingSystem = std::make_unique<LightingSystem>(device, globalSetLayout);

    std::cout << "[Reflection] planar reflection initialized | extent="
              << currentExtent.width << "x" << currentExtent.height << "\n";
    loggedInit = true;
}

void ReflectionPass::createTextures(VkExtent2D extent)
{
    static const char* const gbufferNames[kFramesInFlight][4] = {
        { "Reflection.Position.0", "Reflection.Normal.0", "Reflection.Albedo.0", "Reflection.Pbr.0" },
        { "Reflection.Position.1", "Reflection.Normal.1", "Reflection.Albedo.1", "Reflection.Pbr.1" }
    };
    static const char* const depthNames[kFramesInFlight] = {
        "Reflection.Depth.0", "Reflection.Depth.1"
    };
    static const char* const colorNames[kFramesInFlight] = {
        "Reflection.Color.0", "Reflection.Color.1"
    };
    const VkFormat gbufferFormats[4] = {
        GBufferFormat::Position, GBufferFormat::Normal,
        GBufferFormat::Albedo, GBufferFormat::Pbr
    };

    currentExtent = extent;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            RenderGraph::TextureDesc desc{};
            desc.name = gbufferNames[frame][i];
            desc.format = gbufferFormats[i];
            desc.extent = extent;
            desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            gbufferTextures[frame][i] = renderGraph.createTexture(desc);
        }

        RenderGraph::TextureDesc depthDesc{};
        depthDesc.name = depthNames[frame];
        depthDesc.format = depthFormat;
        depthDesc.extent = extent;
        depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        depthTextures[frame] = renderGraph.createTexture(depthDesc);

        RenderGraph::TextureDesc colorDesc{};
        colorDesc.name = colorNames[frame];
        colorDesc.format = kHdrSceneFormat;
        colorDesc.extent = extent;
        colorDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        colorTextures[frame] = renderGraph.createTexture(colorDesc);
    }
}

void ReflectionPass::createUniformBuffers()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = sizeof(GlobalUbo);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &uboBuffers[frame], &uboAllocations[frame], nullptr) != VK_SUCCESS)
            throw std::runtime_error("ReflectionPass: reflection UBO creation failed");

        VmaAllocationInfo info{};
        vmaGetAllocationInfo(device.getVmaAllocator(), uboAllocations[frame], &info);
        uboMapped[frame] = info.pMappedData;
    }
}

void ReflectionPass::destroyUniformBuffers()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (uboBuffers[frame] != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), uboBuffers[frame], uboAllocations[frame]);
        uboBuffers[frame] = VK_NULL_HANDLE;
        uboAllocations[frame] = VK_NULL_HANDLE;
        uboMapped[frame] = nullptr;
    }
}

void ReflectionPass::prepareFrame(const GlobalUbo& mainUbo, const glm::mat4& view,
                                  const glm::mat4& unjitteredProjection, float level)
{
    frameUbo = mainUbo;
    mainView = view;
    projection = unjitteredProjection;
    waterLevel = level;
}

void ReflectionPass::resize(VkExtent2D extent)
{
    const VkExtent2D half = halfExtent(extent);
    if (half.width == currentExtent.width && half.height == currentExtent.height)
        return;

    currentExtent = half;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (gbufferTextures[frame][i].valid())
                renderGraph.resizeOwnedTexture(gbufferTextures[frame][i], half);
        }
        if (depthTextures[frame].valid())
            renderGraph.resizeOwnedTexture(depthTextures[frame], half);
        if (colorTextures[frame].valid())
            renderGraph.resizeOwnedTexture(colorTextures[frame], half);
    }

    loggedFirstDraw = false;
    std::cout << "[Reflection] resize | extent=" << half.width << "x" << half.height << "\n";
}

void ReflectionPass::setSharedInputs(const VkDescriptorImageInfo& env,
                                     const VkDescriptorImageInfo& irradiance,
                                     const VkDescriptorImageInfo& prefiltered,
                                     const VkDescriptorImageInfo& brdfLut,
                                     const VkDescriptorImageInfo& ssao,
                                     VkSampler shadowSamplerIn,
                                     const std::array<VkImageView, kFramesInFlight>& shadowViewsIn)
{
    envInfo = env;
    irradianceInfo = irradiance;
    prefilteredInfo = prefiltered;
    brdfLutInfo = brdfLut;
    ssaoInfo = ssao;
    shadowSampler = shadowSamplerIn;
    shadowViews = shadowViewsIn;
}

void ReflectionPass::rebuildDescriptors()
{
    if (descriptorManager == nullptr || lightingSystem == nullptr || colorSampler == VK_NULL_HANDLE)
        return;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (!colorTextures[frame].valid())
            continue;

        if (reflectionSets[frame] == VK_NULL_HANDLE)
            reflectionSets[frame] = descriptorManager->allocateSet(globalSetLayout);
        if (reflectionSets[frame] == VK_NULL_HANDLE)
            throw std::runtime_error("ReflectionPass: reflection descriptor set allocation failed");

        VkDescriptorImageInfo gbufferInfos[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            gbufferInfos[i].sampler = gbufferSampler;
            gbufferInfos[i].imageView = renderGraph.getView(gbufferTextures[frame][i]);
            gbufferInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = uboBuffers[frame];
        uboInfo.offset = 0;
        uboInfo.range = sizeof(GlobalUbo);

        VkDescriptorBufferInfo materialInfo{};
        materialInfo.buffer = descriptorManager->getMaterialFactorBuffer();
        materialInfo.offset = 0;
        materialInfo.range = VK_WHOLE_SIZE;

        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler = shadowSampler;
        shadowInfo.imageView = shadowViews[frame];
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo colorInfo{};
        colorInfo.sampler = colorSampler;
        colorInfo.imageView = getColorView(frame);
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::vector<VkWriteDescriptorSet> writes(13);
        auto addImage = [&](uint32_t binding, const VkDescriptorImageInfo* info)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = reflectionSets[frame];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].descriptorCount = 1;
            writes[binding].pImageInfo = info;
        };

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = reflectionSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &uboInfo;

        addImage(1, &envInfo);
        addImage(2, &gbufferInfos[0]);
        addImage(3, &gbufferInfos[1]);
        addImage(4, &gbufferInfos[2]);
        addImage(5, &gbufferInfos[3]);
        addImage(6, &ssaoInfo);
        addImage(7, &irradianceInfo);
        addImage(8, &prefilteredInfo);
        addImage(9, &brdfLutInfo);

        writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[10].dstSet = reflectionSets[frame];
        writes[10].dstBinding = 10;
        writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[10].descriptorCount = 1;
        writes[10].pBufferInfo = &materialInfo;

        writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[11].dstSet = reflectionSets[frame];
        writes[11].dstBinding = 11;
        writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[11].descriptorCount = 1;
        writes[11].pImageInfo = &shadowInfo;

        addImage(12, &colorInfo);

        descriptorManager->writeSet(reflectionSets[frame], writes);
    }
}

void ReflectionPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    active = false;

    if (!frameInfo.settings.waterEnabled || !frameInfo.settings.waterPlanarReflection)
        return;
    if (lightingSystem == nullptr)
        return;
    if (frameInfo.registry.view<TerrainComponent>().size() == 0 &&
        frameInfo.registry.view<VoxelChunkComponent>().size() == 0)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    if (frame >= kFramesInFlight || !colorTextures[frame].valid() ||
        reflectionSets[frame] == VK_NULL_HANDLE)
        return;

    active = true;

    // 镜像反射矩阵：R = T(0, 2*level, 0) * S(1, -1, 1)。
    glm::mat4 mirror(1.0f);
    mirror[1][1] = -1.0f;
    mirror[3][1] = 2.0f * waterLevel;
    const glm::mat4 reflectionView = mainView * mirror;
    const glm::mat4 reflectionProjectionView = projection * reflectionView;
    const glm::vec3 mainCamera(frameUbo.cameraPos);
    const glm::vec3 reflectionCamera(mainCamera.x, 2.0f * waterLevel - mainCamera.y, mainCamera.z);

    auto& gbufferPass = graph.addGraphicsPass("PlanarReflection.GBuffer");
    for (uint32_t i = 0; i < 4; ++i)
        gbufferPass.writeColor(gbufferTextures[frame][i], VK_ATTACHMENT_LOAD_OP_CLEAR);
    VkClearValue depthClear{};
    depthClear.depthStencil = { 1.0f, 0 };
    gbufferPass.writeDepth(depthTextures[frame], VK_ATTACHMENT_LOAD_OP_CLEAR,
                           depthClear, VK_ATTACHMENT_STORE_OP_STORE);
    gbufferPass.setExtent(currentExtent);
    gbufferPass.execute([this, &frameInfo, frame, reflectionProjectionView,
                         reflectionCamera](VkCommandBuffer cmd)
    {
        GlobalUbo reflectionUbo = frameUbo;
        reflectionUbo.projectionView = reflectionProjectionView;
        reflectionUbo.cameraPos = glm::vec4(reflectionCamera, 1.0f);
        std::memcpy(uboMapped[frame], &reflectionUbo, sizeof(reflectionUbo));
        vmaFlushAllocation(device.getVmaAllocator(), uboAllocations[frame], 0, sizeof(reflectionUbo));

        if (geometrySystem == nullptr)
            return;

        GeometrySystem::GeometryViewOverride viewOverride{};
        viewOverride.globalSet = reflectionSets[frame];
        viewOverride.viewProjection = reflectionProjectionView;
        viewOverride.cameraPosition = reflectionCamera;
        viewOverride.clipY = waterLevel;
        viewOverride.clipMode = 1;
        viewOverride.reflection = true;

        // 反射几何的统计不应污染主视角 telemetry（Validate 的 draw call 判据）。
        RenderTelemetry saved = frameInfo.telemetry;
        geometrySystem->render(cmd, frameInfo, &viewOverride);
        frameInfo.telemetry = saved;

        if (!loggedFirstDraw)
        {
            std::cout << "[Reflection] first draw | level=" << waterLevel
                      << " mirrorY=" << reflectionCamera.y
                      << " extent=" << currentExtent.width << "x" << currentExtent.height << "\n";
            loggedFirstDraw = true;
        }
    });

    auto& lightingPass = graph.addGraphicsPass("PlanarReflection.Lighting");
    for (uint32_t i = 0; i < 4; ++i)
        lightingPass.read(gbufferTextures[frame][i]);
    lightingPass.read(resources.environment);
    lightingPass.read(resources.irradiance);
    lightingPass.read(resources.prefiltered);
    lightingPass.read(resources.brdfLut);
    VkClearValue colorClear{};
    colorClear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    lightingPass.writeColor(colorTextures[frame], VK_ATTACHMENT_LOAD_OP_CLEAR, colorClear);
    lightingPass.setExtent(currentExtent);
    lightingPass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        VkDescriptorSet set = reflectionSets[frame];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                lightingSystem->getPipelineLayout(), 0, 1, &set, 0, nullptr);
        lightingSystem->render(cmd, frameInfo);
    });
}

VkImageView ReflectionPass::getColorView(uint32_t frame) const
{
    if (frame >= kFramesInFlight || !colorTextures[frame].valid())
        return VK_NULL_HANDLE;
    return renderGraph.getView(colorTextures[frame]);
}

RenderGraph::TextureHandle ReflectionPass::getColorHandle(uint32_t frame) const
{
    if (frame >= kFramesInFlight)
        return {};
    return colorTextures[frame];
}
