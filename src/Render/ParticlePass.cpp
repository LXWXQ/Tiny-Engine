#include "ParticlePass.h"

#include "AssetPaths.h"
#include "Scene/FeatureComponents.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>

static_assert(sizeof(ParticlePushConstantData) == 32,
              "particle push constant must stay 32 bytes");
static_assert(sizeof(ParticleInstance) == 32,
              "particle instance must stay 32 bytes");

namespace
{
constexpr VkDeviceSize kInstanceBytes =
    static_cast<VkDeviceSize>(sizeof(ParticleInstance)) * 16384ull;
}

ParticlePass::ParticlePass(Device& device, RenderGraph& graph, RenderResources& resources,
                           VkDescriptorSetLayout globalSetLayout,
                           DescriptorManager* descriptorManager, VkFormat depthFormat)
    : device(device), renderGraph(graph), resources(resources),
      descriptorManager(descriptorManager), globalSetLayout(globalSetLayout),
      depthFormat(depthFormat)
{
    instances.reserve(kMaxParticles);
}

ParticlePass::~ParticlePass()
{
    particlePipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    destroyBuffers();
}

void ParticlePass::init()
{
    createBuffers();
    createPipeline();
}

void ParticlePass::createBuffers()
{
    static const char* const names[kFramesInFlight] = {
        "ParticleInstances.Frame0", "ParticleInstances.Frame1"
    };
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kInstanceBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>(names[frame]);

        VmaAllocationInfo allocationInfo{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &instanceBuffers[frame], &instanceAllocations[frame],
                            &allocationInfo) != VK_SUCCESS)
            throw std::runtime_error("ParticlePass: instance buffer creation failed");
        instanceMapped[frame] = allocationInfo.pMappedData;
        if (instanceMapped[frame] == nullptr)
            throw std::runtime_error("ParticlePass: instance buffer is not mapped");
    }
}

void ParticlePass::destroyBuffers()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (instanceBuffers[frame] != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device.getVmaAllocator(), instanceBuffers[frame],
                             instanceAllocations[frame]);
        }
        instanceBuffers[frame] = VK_NULL_HANDLE;
        instanceAllocations[frame] = VK_NULL_HANDLE;
        instanceMapped[frame] = nullptr;
    }
}

void ParticlePass::createPipeline()
{
    VkDescriptorSetLayout setLayouts[] = { globalSetLayout };

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ParticlePushConstantData);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr,
                               &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("ParticlePass: pipeline layout creation failed");

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ParticleInstance);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[0].offset = offsetof(ParticleInstance, positionSize);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[1].offset = offsetof(ParticleInstance, color);

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.depthAttachmentFormat = depthFormat;

    VkPipelineColorBlendAttachmentState& blend = config.colorBlendAttachments[0];
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    config.colorBlendInfo.attachmentCount = 1;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    // 与 Glass 相同：粒子被不透明物体遮挡，但不把自己的深度写进 G-Buffer。
    config.depthStencilInfo.depthTestEnable = VK_TRUE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    config.bindingDescriptions = { binding };
    config.attributeDescriptions = { attributes[0], attributes[1] };

    particlePipeline = std::make_unique<Pipeline>(
        device, TINY_SHADER_DIR "particle.vert.spv",
        TINY_SHADER_DIR "particle.frag.spv", config);
}

ParticlePass::BuildResult ParticlePass::buildInstances(FrameInfo& frameInfo,
                                                       const glm::vec3& cameraPosition)
{
    instances.clear();
    BuildResult result{};

    for (auto [entity, emitter] :
         frameInfo.registry.view<scene::ParticleEmitterComponent>().each())
    {
        (void)entity;
        ++result.emitters;
        for (const scene::Particle& particle : emitter.particles)
        {
            if (particle.lifetime <= 0.0f)
                continue;

            const float t = std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f);
            glm::vec4 color = glm::mix(emitter.startColor, emitter.endColor, t);
            // 出生后的极短淡入，避免粒子在相机前突然弹出来。
            color.a *= std::min(t / 0.08f, 1.0f);
            if (color.a <= 0.001f)
                continue;

            const float size = glm::mix(emitter.startSize, emitter.endSize, t) *
                               particle.sizeScale;
            if (size <= 0.0001f)
                continue;

            instances.push_back({ glm::vec4(particle.position, size), color });
        }
    }

    if (instances.size() > kMaxParticles)
        instances.resize(kMaxParticles);

    std::sort(instances.begin(), instances.end(),
              [&cameraPosition](const ParticleInstance& a, const ParticleInstance& b)
              {
                  const glm::vec3 da = glm::vec3(a.positionSize) - cameraPosition;
                  const glm::vec3 db = glm::vec3(b.positionSize) - cameraPosition;
                  return glm::dot(da, da) > glm::dot(db, db);
              });

    result.alive = static_cast<uint32_t>(instances.size());
    return result;
}

void ParticlePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;
    if (frame >= kFramesInFlight || frame >= instanceBuffers.size() ||
        instanceBuffers[frame] == VK_NULL_HANDLE)
        return;

    auto& pass = graph.addGraphicsPass("Particle");
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeDepth(resources.gbDepth[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    pass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        glm::vec3 cameraPosition(0.0f);
        if (frameInfo.cameraTransform != nullptr)
            cameraPosition = frameInfo.cameraTransform->getTranslation();
        else
            cameraPosition = glm::vec3(glm::inverse(frameInfo.viewMatrix)[3]);

        const BuildResult result = buildInstances(frameInfo, cameraPosition);
        if (result.alive == 0u || particlePipeline == nullptr)
            return;

        const glm::mat4 inverseView = glm::inverse(frameInfo.viewMatrix);
        glm::vec3 cameraRight = glm::vec3(inverseView[0]);
        glm::vec3 cameraUp = glm::vec3(inverseView[1]);
        if (glm::length(cameraRight) < 1e-5f)
            cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
        if (glm::length(cameraUp) < 1e-5f)
            cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
        cameraRight = glm::normalize(cameraRight);
        cameraUp = glm::normalize(cameraUp);

        std::memcpy(instanceMapped[frame], instances.data(),
                    static_cast<std::size_t>(result.alive) * sizeof(ParticleInstance));
        vmaFlushAllocation(device.getVmaAllocator(), instanceAllocations[frame], 0,
                           static_cast<VkDeviceSize>(result.alive) * sizeof(ParticleInstance));

        particlePipeline->bind(cmd);

        VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                1, &globalSet, 0, nullptr);

        const ParticlePushConstantData push{
            glm::vec4(cameraRight, 0.0f),
            glm::vec4(cameraUp, 0.0f)
        };
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(ParticlePushConstantData), &push);

        VkBuffer vertexBuffers[] = { instanceBuffers[frame] };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdDraw(cmd, 6, result.alive, 0, 0);

        frameInfo.telemetry.drawCalls += 1;
        frameInfo.telemetry.triangles += result.alive * 2u;
        frameInfo.telemetry.vertices += result.alive * 6u;
        frameInfo.telemetry.descriptorBinds += 1;

        const auto now = std::chrono::steady_clock::now();
        const bool changed = result.alive != lastLoggedAlive;
        const bool interval = lastLogTime.time_since_epoch().count() == 0 ||
                              now - lastLogTime >= std::chrono::seconds(1);
        if (changed && (interval || result.alive == 0u))
        {
            std::cout << "[Particle] emitters=" << result.emitters
                      << " alive=" << result.alive << " drawcalls=1\n";
            lastLoggedAlive = result.alive;
            lastLogTime = now;
        }
    });
}
