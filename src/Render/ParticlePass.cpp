#include "ParticlePass.h"

#include "AssetPaths.h"
#include "Core/Material.h"
#include "Core/MaterialSystem.h"
#include "Scene/FeatureComponents.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

static_assert(sizeof(ParticlePushConstantData) == 32,
              "particle push constant must stay 32 bytes");
static_assert(sizeof(ParticleInstance) == 36,
              "particle instance must stay 36 bytes (vec4 + vec4 + uint32)");

namespace
{
constexpr VkDeviceSize kInstanceBytes =
    static_cast<VkDeviceSize>(sizeof(ParticleInstance)) * 16384ull;
}

ParticlePass::ParticlePass(Device& device, RenderGraph& graph, RenderResources& resources,
                           VkDescriptorSetLayout globalSetLayout,
                           DescriptorManager* descriptorManager, VkFormat depthFormat,
                           Scene* scene)
    : device(device), renderGraph(graph), resources(resources), scene(scene),
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
    // 启动时预加载：帧中上传贴图会强制等待 GPU，且会让描述符集在录制命令时
    // 发生变化。新增/替换文件后需要重启（Inspector 会提示）。
    loadParticleTextures();
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

void ParticlePass::loadParticleTextures()
{
    if (scene == nullptr || descriptorManager == nullptr ||
        descriptorManager->getMaterialSet() == VK_NULL_HANDLE)
    {
        std::cout << "[Particle] textures=0 (no scene/descriptor manager)\n";
        return;
    }

    const std::filesystem::path textureRoot =
        std::filesystem::path(TINY_RESOURCE_DIR) / "Texture";
    const std::filesystem::path particleRoot = textureRoot / "Particles";

    std::error_code ec;
    if (!std::filesystem::is_directory(particleRoot, ec))
    {
        std::cout << "[Particle] textures=0 root=" << particleRoot.generic_string()
                  << " (missing)\n";
        return;
    }

    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(particleRoot, ec), end;
         it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc)
            continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" &&
            ext != ".tga" && ext != ".bmp")
            continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    size_t failed = 0;
    for (const std::filesystem::path& file : files)
    {
        if (textureSlots.size() >= kMaxParticleTextures)
            break;

        std::error_code relEc;
        std::filesystem::path rel = std::filesystem::relative(file, textureRoot, relEc);
        if (relEc)
            rel = file.lexically_relative(textureRoot);
        const std::string key = rel.generic_string();

        std::shared_ptr<Texture> texture = scene->loadMaterialTexture(file.string(), true);
        if (!texture)
        {
            ++failed;
            std::cout << "[Particle] texture load failed: " << key << "\n";
            continue;
        }

        // 粒子只采 albedo；其它贴图槽由 Material 的默认替身补齐。
        auto material = std::make_shared<Material>();
        material->name = "Particle/" + key;
        material->albedoMap = texture;
        scene->getMaterialSystem().buildMaterialDescriptor(material, *descriptorManager);
        textureSlots.emplace(key, material->descriptorSetIndex);
    }

    const char* status = (!textureSlots.empty() && failed == 0) ? "ready" : "fallback";
    std::cout << "[Particle] textures=" << textureSlots.size()
              << " failed=" << failed
              << " status=" << status
              << " root=" << particleRoot.generic_string() << "\n";
}

void ParticlePass::createPipeline()
{
    if (descriptorManager == nullptr ||
        descriptorManager->getMaterialSet() == VK_NULL_HANDLE)
        throw std::runtime_error("ParticlePass: material descriptor set is required");

    // set 0 = 帧全局；set 1 = bindless 材质数组（albedo/normal/metal/rough）。
    VkDescriptorSetLayout setLayouts[] = {
        globalSetLayout, descriptorManager->getMaterialSetLayout()
    };

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ParticlePushConstantData);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
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

    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[0].offset = offsetof(ParticleInstance, positionSize);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[1].offset = offsetof(ParticleInstance, color);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32_UINT;
    attributes[2].offset = offsetof(ParticleInstance, materialId);

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
    config.attributeDescriptions = { attributes[0], attributes[1], attributes[2] };

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

        // 路径统一成 generic 形式；空 = 程序化圆形。找不到（例如序列化后
        // 文件被删）也回退圆形，避免整批粒子消失。
        std::string textureKey = emitter.texture;
        for (char& c : textureKey)
        {
            if (c == '\\')
                c = '/';
        }
        uint32_t materialId = 0xFFFFFFFFu;
        if (!textureKey.empty())
        {
            const auto slot = textureSlots.find(textureKey);
            if (slot != textureSlots.end())
                materialId = slot->second;
        }

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

            instances.push_back({ glm::vec4(particle.position, size), color, materialId });
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
        VkDescriptorSet materialSet = descriptorManager->getMaterialSet();
        VkDescriptorSet sets[] = { globalSet, materialSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                2, sets, 0, nullptr);

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
