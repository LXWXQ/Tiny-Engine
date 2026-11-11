#include "FogVolumePass.h"
#include "AssetPaths.h"

#include "Scene/Components.h"
#include "Scene/FeatureComponents.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr uint32_t kNoiseSize = 32;

    uint32_t wrapIndex(int value, int period)
    {
        const int wrapped = value % period;
        return static_cast<uint32_t>(wrapped < 0 ? wrapped + period : wrapped);
    }

    uint32_t hash3(int x, int y, int z)
    {
        uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u;
        h ^= static_cast<uint32_t>(y) * 0xd8163841u;
        h ^= static_cast<uint32_t>(z) * 0xcb1ab31fu;
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        h *= 0x297a2d39u;
        h ^= h >> 15;
        return h;
    }

    float random01(uint32_t h)
    {
        return static_cast<float>(h & 0x00ffffffu) / 16777216.0f;
    }

    float fade(float t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    // 可平铺 value noise：格点按 period 取模，u=0 与 u=1 边界连续。
    float valueNoise(glm::vec3 p, int period)
    {
        const glm::vec3 base = glm::floor(p);
        const glm::vec3 f = p - base;
        const int ix = static_cast<int>(base.x);
        const int iy = static_cast<int>(base.y);
        const int iz = static_cast<int>(base.z);

        auto lattice = [&](int dx, int dy, int dz)
        {
            return random01(hash3(static_cast<int>(wrapIndex(ix + dx, period)),
                                  static_cast<int>(wrapIndex(iy + dy, period)),
                                  static_cast<int>(wrapIndex(iz + dz, period))));
        };

        const float u = fade(f.x);
        const float v = fade(f.y);
        const float w = fade(f.z);
        const float x00 = glm::mix(lattice(0, 0, 0), lattice(1, 0, 0), u);
        const float x10 = glm::mix(lattice(0, 1, 0), lattice(1, 1, 0), u);
        const float x01 = glm::mix(lattice(0, 0, 1), lattice(1, 0, 1), u);
        const float x11 = glm::mix(lattice(0, 1, 1), lattice(1, 1, 1), u);
        return glm::mix(glm::mix(x00, x10, v), glm::mix(x01, x11, v), w);
    }

    float fbm(glm::vec3 u, int basePeriod, int octaves)
    {
        float sum = 0.0f;
        float amplitude = 0.5f;
        float norm = 0.0f;
        int period = basePeriod;
        for (int i = 0; i < octaves; ++i)
        {
            sum += valueNoise(u * static_cast<float>(period), period) * amplitude;
            norm += amplitude;
            amplitude *= 0.5f;
            period *= 2;
        }
        return norm > 0.0f ? sum / norm : 0.0f;
    }

    std::vector<uint8_t> buildNoiseVolume()
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(kNoiseSize) * kNoiseSize * kNoiseSize * 4u);
        size_t offset = 0;
        for (uint32_t z = 0; z < kNoiseSize; ++z)
        {
            for (uint32_t y = 0; y < kNoiseSize; ++y)
            {
                for (uint32_t x = 0; x < kNoiseSize; ++x)
                {
                    const glm::vec3 u{
                        (static_cast<float>(x) + 0.5f) / static_cast<float>(kNoiseSize),
                        (static_cast<float>(y) + 0.5f) / static_cast<float>(kNoiseSize),
                        (static_cast<float>(z) + 0.5f) / static_cast<float>(kNoiseSize)
                    };

                    // R=低频雾团，G=中频细节，B=高频丝缕。雾不像云那样需要
                    // Worley 侵蚀，value noise 的低频团已经足够，而且生成更快。
                    const float low = fbm(u, 4, 4);
                    const float mid = fbm(u, 8, 2);
                    const float detail = fbm(u, 16, 2);

                    pixels[offset + 0] = static_cast<uint8_t>(std::clamp(low, 0.0f, 1.0f) * 255.0f + 0.5f);
                    pixels[offset + 1] = static_cast<uint8_t>(std::clamp(mid, 0.0f, 1.0f) * 255.0f + 0.5f);
                    pixels[offset + 2] = static_cast<uint8_t>(std::clamp(detail, 0.0f, 1.0f) * 255.0f + 0.5f);
                    pixels[offset + 3] = 255u;
                    offset += 4;
                }
            }
        }
        return pixels;
    }
}

FogVolumePass::FogVolumePass(Device& device, RenderGraph& graph, RenderResources& resources,
                             VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
                             VkCommandPool commandPool)
    : device(device), renderGraph(graph), resources(resources),
      globalSetLayout(globalSetLayout), descriptorManager(descriptorManager), commandPool(commandPool)
{
}

FogVolumePass::~FogVolumePass()
{
    destroyUniformBuffers();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (positionSampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), positionSampler, nullptr);
}

void FogVolumePass::init()
{
    createNoiseTexture();
    createUniformBuffers();
    createDescriptorLayout();
    createSampler();

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        sets[frame] = descriptorManager->allocateSet(setLayout);
        updateDescriptor(frame);
    }

    createPipeline();
    std::cout << "[FogVolume] initialized | noise=" << kNoiseSize << "^3"
              << " maxVolumes=" << kMaxFogVolumes << std::endl;
}

void FogVolumePass::createNoiseTexture()
{
    const std::vector<uint8_t> pixels = buildNoiseVolume();
    noiseTexture = std::make_unique<Texture3D>(
        device, pixels.data(), kNoiseSize, kNoiseSize, kNoiseSize,
        VK_FORMAT_R8G8B8A8_UNORM, commandPool);
}

void FogVolumePass::createUniformBuffers()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = sizeof(VolumeUbo);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &uboBuffers[frame], &uboAllocations[frame], nullptr) != VK_SUCCESS)
            throw std::runtime_error("FogVolumePass: per-frame UBO 创建失败");

        VmaAllocationInfo info{};
        vmaGetAllocationInfo(device.getVmaAllocator(), uboAllocations[frame], &info);
        uboMapped[frame] = info.pMappedData;
        std::memset(uboMapped[frame], 0, sizeof(VolumeUbo));
    }
}

void FogVolumePass::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    setLayout = descriptorManager->createSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings, bindings + 3));
}

void FogVolumePass::createSampler()
{
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &info, nullptr, &positionSampler) != VK_SUCCESS)
        throw std::runtime_error("FogVolumePass: 位置采样器创建失败");
}

void FogVolumePass::updateDescriptor(uint32_t frame)
{
    VkDescriptorBufferInfo bufferInfo{ uboBuffers[frame], 0, sizeof(VolumeUbo) };
    VkDescriptorImageInfo noiseInfo{ noiseTexture->getSampler(), noiseTexture->getView(),
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo positionInfo{ positionSampler, renderGraph.getView(resources.gbPosition[frame]),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = sets[frame];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = sets[frame];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &noiseInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = sets[frame];
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &positionInfo;
    vkUpdateDescriptorSets(device.getDevice(), 3, writes, 0, nullptr);
}

void FogVolumePass::createPipeline()
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
        throw std::runtime_error("FogVolumePass: 管线布局创建失败");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState& blend = config.colorBlendAttachments[0];
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    pipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "fog_volume.frag.spv", config);
}

void FogVolumePass::resize(VkExtent2D /*extent*/)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        updateDescriptor(frame);
}

void FogVolumePass::destroyUniformBuffers()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (uboBuffers[frame] != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device.getVmaAllocator(), uboBuffers[frame], uboAllocations[frame]);
            uboBuffers[frame] = VK_NULL_HANDLE;
            uboAllocations[frame] = VK_NULL_HANDLE;
            uboMapped[frame] = nullptr;
        }
    }
}

void FogVolumePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;

    VolumeUbo ubo{};
    uint32_t count = 0;
    auto view = frameInfo.registry.view<scene::FogVolumeComponent, WorldTransformComponent>();
    for (const entt::entity entity : view)
    {
        if (count >= kMaxFogVolumes)
            break;

        const auto& component = view.get<scene::FogVolumeComponent>(entity);
        const auto& world = view.get<WorldTransformComponent>(entity);
        if (!component.enabled || !world.valid || component.density <= 0.0f || component.steps <= 0)
            continue;

        // 盒体 = 实体世界矩阵 × 组件局部 placement。用户可在 Inspector 里直接
        // 调 center/size/rotation；实体 Transform 仍然可以整体移动/旋转。
        const glm::mat4 volumeWorld = world.matrix * component.localMatrix();
        const float determinant = glm::determinant(volumeWorld);
        if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f)
            continue;

        GpuVolume& volume = ubo.volumes[count];
        volume.world = volumeWorld;
        volume.inverseWorld = glm::inverse(volumeWorld);
        volume.params0 = glm::vec4(component.density, component.heightFalloff,
                                   component.noiseStrength, component.noiseScale);
        volume.params1 = glm::vec4(volumeWorld[3][1], component.edgeSoftness,
                                   component.anisotropy, component.ambient);
        volume.color = glm::vec4(component.color.r, component.color.g, component.color.b,
                                 static_cast<float>(component.steps));
        volume.wind = glm::vec4(component.wind, 0.0f);
        ++count;
    }

    active = count > 0;
    if (!active)
    {
        if (!loggedEmpty)
        {
            loggedEmpty = true;
            std::cout << "[FogVolume] idle | volumes=0" << std::endl;
        }
        return;
    }

    timeSeconds += std::max(0.0f, frameInfo.frameTime);
    ubo.header = glm::vec4(static_cast<float>(count), 0.0f, 0.0f, 0.0f);
    std::memcpy(uboMapped[frame], &ubo, sizeof(VolumeUbo));

    auto& pass = graph.addGraphicsPass("FogVolume");
    pass.read(resources.gbPosition[frame]);
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    PushConstants push{};
    push.inverseProjectionView = glm::inverse(frameInfo.projectionMatrix * frameInfo.viewMatrix);
    push.params = glm::vec4(timeSeconds, 500.0f, 0.0f, 0.0f);

    const VkDescriptorSet globalSet = descriptorManager->getGlobalSet(frame);
    const VkDescriptorSet localSet = sets[frame];
    if (!loggedFirstDraw)
    {
        loggedFirstDraw = true;
        std::cout << "[FogVolume] first draw | volumes=" << count
                  << " density=" << ubo.volumes[0].params0.x
                  << " steps=" << static_cast<int>(ubo.volumes[0].color.a + 0.5f) << "\n";
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
