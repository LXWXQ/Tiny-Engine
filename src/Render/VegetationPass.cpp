#include "VegetationPass.h"

#include "AssetPaths.h"
#include "RHI/GBuffer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Vegetation/VegetationGenerator.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
constexpr VkDeviceSize kGrassInstanceBytes =
    static_cast<VkDeviceSize>(vegetation::kMaxGrassInstances) *
    sizeof(vegetation::Instance);
constexpr VkDeviceSize kTreeInstanceBytes =
    static_cast<VkDeviceSize>(vegetation::kMaxTreeInstances) *
    sizeof(vegetation::Instance);

bool sameParams(const vegetation::Params& a, const vegetation::Params& b)
{
    return a.grassDensity == b.grassDensity &&
           a.grassDistance == b.grassDistance &&
           a.grassHeight == b.grassHeight &&
           a.treeDensity == b.treeDensity &&
           a.treeDistance == b.treeDistance &&
           a.treeHeight == b.treeHeight &&
           a.windStrength == b.windStrength &&
           a.seed == b.seed &&
           a.grassColor == b.grassColor &&
           a.treeColor == b.treeColor &&
           a.trunkColor == b.trunkColor &&
           a.waterEnabled == b.waterEnabled &&
           a.waterLevel == b.waterLevel;
}

uint64_t terrainSignature(const entt::registry& registry)
{
    uint64_t signature = 1469598103934665603ull;
    auto view = registry.view<TerrainComponent>();
    for (const entt::entity entity : view)
    {
        const TerrainComponent& terrain = view.get<TerrainComponent>(entity);
        signature ^= terrain.revision + 0x9e3779b97f4a7c15ull +
                     (signature << 6) + (signature >> 2);
        signature ^= static_cast<uint64_t>(terrain.generated ? 1u : 0u);
    }
    return signature;
}
} // namespace

VegetationPass::VegetationPass(Device& device, RenderGraph& graph,
                               RenderResources& resources,
                               VkDescriptorSetLayout globalSetLayout,
                               VkFormat depthFormat, Scene* scene)
    : device(device), renderGraph(graph), resources(resources), scene(scene),
      globalSetLayout(globalSetLayout), depthFormat(depthFormat)
{
    placement.grass.reserve(vegetation::kMaxGrassInstances);
    placement.trees.reserve(vegetation::kMaxTreeInstances);
}

VegetationPass::~VegetationPass()
{
    vegetationPipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
    destroyBuffers();
}

void VegetationPass::init()
{
    createBuffers();
    createPipeline();
}

void VegetationPass::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      const char* name, GpuBuffer& out)
{
    if (size == 0)
        throw std::runtime_error("VegetationPass: refusing to create an empty buffer");

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
    allocInfo.pUserData = const_cast<char*>(name);

    VmaAllocationInfo allocationInfo{};
    if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                        &out.buffer, &out.allocation, &allocationInfo) != VK_SUCCESS)
        throw std::runtime_error(std::string("VegetationPass: buffer creation failed: ") + name);
    out.mapped = allocationInfo.pMappedData;
    out.size = size;
    if (out.mapped == nullptr)
        throw std::runtime_error(std::string("VegetationPass: buffer not mapped: ") + name);
}

void VegetationPass::createBuffers()
{
    vegetation::MeshData grassMesh;
    vegetation::MeshData treeMesh;
    std::string error;
    if (!vegetation::buildMeshes(grassMesh, treeMesh, &error))
        throw std::runtime_error("VegetationPass: " + error);

    createHostBuffer(static_cast<VkDeviceSize>(grassMesh.vertices.size()) *
                         sizeof(Vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     "Vegetation.GrassVertices", grassVertexBuffer);
    std::memcpy(grassVertexBuffer.mapped, grassMesh.vertices.data(),
                grassMesh.vertices.size() * sizeof(Vertex));

    createHostBuffer(static_cast<VkDeviceSize>(grassMesh.indices.size()) *
                         sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     "Vegetation.GrassIndices", grassIndexBuffer);
    std::memcpy(grassIndexBuffer.mapped, grassMesh.indices.data(),
                grassMesh.indices.size() * sizeof(uint32_t));
    grassIndexCount = static_cast<uint32_t>(grassMesh.indices.size());

    createHostBuffer(static_cast<VkDeviceSize>(treeMesh.vertices.size()) *
                         sizeof(Vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     "Vegetation.TreeVertices", treeVertexBuffer);
    std::memcpy(treeVertexBuffer.mapped, treeMesh.vertices.data(),
                treeMesh.vertices.size() * sizeof(Vertex));

    createHostBuffer(static_cast<VkDeviceSize>(treeMesh.indices.size()) *
                         sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     "Vegetation.TreeIndices", treeIndexBuffer);
    std::memcpy(treeIndexBuffer.mapped, treeMesh.indices.data(),
                treeMesh.indices.size() * sizeof(uint32_t));
    treeIndexCount = static_cast<uint32_t>(treeMesh.indices.size());

    static const char* const grassNames[kFramesInFlight] = {
        "Vegetation.GrassInstances.Frame0", "Vegetation.GrassInstances.Frame1"
    };
    static const char* const treeNames[kFramesInFlight] = {
        "Vegetation.TreeInstances.Frame0", "Vegetation.TreeInstances.Frame1"
    };
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        createHostBuffer(kGrassInstanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         grassNames[frame], grassInstanceBuffers[frame]);
        createHostBuffer(kTreeInstanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         treeNames[frame], treeInstanceBuffers[frame]);
    }

    std::cout << "[Vegetation] meshes | grassVerts=" << grassMesh.vertices.size()
              << " grassIndices=" << grassMesh.indices.size()
              << " treeVerts=" << treeMesh.vertices.size()
              << " treeIndices=" << treeMesh.indices.size()
              << " maxGrass=" << vegetation::kMaxGrassInstances
              << " maxTrees=" << vegetation::kMaxTreeInstances << "\n";
}

void VegetationPass::destroyBuffers()
{
    auto destroy = [this](GpuBuffer& buffer)
    {
        if (buffer.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), buffer.buffer, buffer.allocation);
        buffer = {};
    };
    destroy(grassVertexBuffer);
    destroy(grassIndexBuffer);
    destroy(treeVertexBuffer);
    destroy(treeIndexBuffer);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        destroy(grassInstanceBuffers[frame]);
        destroy(treeInstanceBuffers[frame]);
    }
}

void VegetationPass::createPipeline()
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(VegetationPushConstantData);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &globalSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr,
                               &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("VegetationPass: pipeline layout creation failed");

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(Vertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(vegetation::Instance);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[7]{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                      static_cast<uint32_t>(offsetof(Vertex, position)) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                      static_cast<uint32_t>(offsetof(Vertex, color)) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT,
                      static_cast<uint32_t>(offsetof(Vertex, normal)) };
    attributes[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,
                      static_cast<uint32_t>(offsetof(Vertex, uv)) };
    attributes[4] = { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                      static_cast<uint32_t>(offsetof(vegetation::Instance, positionScale)) };
    attributes[5] = { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                      static_cast<uint32_t>(offsetof(vegetation::Instance, rotationTint)) };
    attributes[6] = { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                      static_cast<uint32_t>(offsetof(vegetation::Instance, color)) };

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = {
        GBufferFormat::Position, GBufferFormat::Normal,
        GBufferFormat::Albedo, GBufferFormat::Pbr
    };
    config.depthAttachmentFormat = depthFormat;
    config.bindingDescriptions.assign(std::begin(bindings), std::end(bindings));
    config.attributeDescriptions.assign(std::begin(attributes), std::end(attributes));

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    attachment.blendEnable = VK_FALSE;
    config.colorBlendAttachments.assign(4, attachment);
    config.colorBlendInfo.attachmentCount = 4;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    // 植被必须遮挡后面的几何，也参与深度测试；alpha test 在片元里 discard，
    // 所以不需要按透明物体排序，也不会污染 G-Buffer 的深度。
    config.depthStencilInfo.depthTestEnable = VK_TRUE;
    config.depthStencilInfo.depthWriteEnable = VK_TRUE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;

    vegetationPipeline = std::make_unique<Pipeline>(
        device, TINY_SHADER_DIR "vegetation.vert.spv",
        TINY_SHADER_DIR "vegetation.frag.spv", config);
}

void VegetationPass::updatePlacement(FrameInfo& frameInfo,
                                     const glm::vec3& cameraPosition)
{
    if (scene == nullptr)
        return;

    vegetation::Params params;
    params.grassDensity = frameInfo.settings.vegetationGrassDensity;
    params.grassDistance = frameInfo.settings.vegetationGrassDistance;
    params.grassHeight = frameInfo.settings.vegetationGrassHeight;
    params.treeDensity = frameInfo.settings.vegetationTreeDensity;
    params.treeDistance = frameInfo.settings.vegetationTreeDistance;
    params.treeHeight = frameInfo.settings.vegetationTreeHeight;
    params.windStrength = frameInfo.settings.vegetationWindStrength;
    params.seed = frameInfo.settings.vegetationSeed;
    params.grassColor = frameInfo.settings.vegetationGrassColor;
    params.treeColor = frameInfo.settings.vegetationTreeColor;
    params.trunkColor = frameInfo.settings.vegetationTrunkColor;
    params.waterEnabled = frameInfo.settings.waterEnabled;
    params.waterLevel = frameInfo.settings.waterLevel;

    const uint64_t signature = terrainSignature(frameInfo.registry);
    if (!hasTerrainSignature || signature != lastTerrainSignature)
    {
        // 流送期间 chunk 一直在增删：等签名稳定半秒再重排，避免每来一个
        // chunk 就把上万株草重新采样一遍（会把加载期帧率拖到个位数）。
        lastTerrainSignature = signature;
        hasTerrainSignature = true;
        terrainStableSeconds = 0.0f;
    }
    else
    {
        terrainStableSeconds += frameInfo.frameTime;
    }

    const bool moved = !hasLastCamera ||
        glm::distance(cameraPosition, lastCameraPosition) > 6.0f;
    const bool paramsChanged = !sameParams(params, lastParams);
    const bool placementStale = !placementValid || moved || paramsChanged ||
        signature != placedTerrainSignature;
    if (!placementStale || terrainStableSeconds < 0.5f)
        return;

    placement = vegetation::generate(
        cameraPosition, params,
        [this](float x, float z, float& height)
        {
            return scene->sampleTerrainOnlyGroundHeightWorld(x, z, height);
        });

    lastParams = params;
    lastCameraPosition = cameraPosition;
    hasLastCamera = true;
    placedTerrainSignature = signature;
    placementValid = true;

    const uint32_t grassCount = static_cast<uint32_t>(placement.grass.size());
    const uint32_t treeCount = static_cast<uint32_t>(placement.trees.size());
    if (grassCount != lastLoggedGrass || treeCount != lastLoggedTrees)
    {
        std::cout << "[Vegetation] placed | grass=" << grassCount
                  << " trees=" << treeCount
                  << " grassRadius=" << params.grassDistance
                  << " treeRadius=" << params.treeDistance
                  << " seed=" << params.seed << "\n";
        lastLoggedGrass = grassCount;
        lastLoggedTrees = treeCount;
    }
    if (grassCount >= vegetation::kMaxGrassInstances)
        std::cout << "[Vegetation] warning: grass instance cap reached ("
                  << vegetation::kMaxGrassInstances << ")\n";
    if (treeCount >= vegetation::kMaxTreeInstances)
        std::cout << "[Vegetation] warning: tree instance cap reached ("
                  << vegetation::kMaxTreeInstances << ")\n";
}

void VegetationPass::record(VkCommandBuffer cmd, FrameInfo& frameInfo, uint32_t frame)
{
    if (vegetationPipeline == nullptr || frame >= kFramesInFlight)
        return;

    const glm::vec3 cameraPosition = frameInfo.cameraTransform != nullptr
        ? frameInfo.cameraTransform->getTranslation()
        : glm::vec3(glm::inverse(frameInfo.viewMatrix)[3]);

    updatePlacement(frameInfo, cameraPosition);

    const uint32_t grassCount = static_cast<uint32_t>(std::min<size_t>(
        placement.grass.size(), vegetation::kMaxGrassInstances));
    const uint32_t treeCount = static_cast<uint32_t>(std::min<size_t>(
        placement.trees.size(), vegetation::kMaxTreeInstances));
    if (grassCount == 0 && treeCount == 0)
        return;

    if (grassCount > 0)
    {
        std::memcpy(grassInstanceBuffers[frame].mapped, placement.grass.data(),
                    static_cast<size_t>(grassCount) * sizeof(vegetation::Instance));
        vmaFlushAllocation(device.getVmaAllocator(),
                           grassInstanceBuffers[frame].allocation, 0,
                           static_cast<VkDeviceSize>(grassCount) *
                               sizeof(vegetation::Instance));
    }
    if (treeCount > 0)
    {
        std::memcpy(treeInstanceBuffers[frame].mapped, placement.trees.data(),
                    static_cast<size_t>(treeCount) * sizeof(vegetation::Instance));
        vmaFlushAllocation(device.getVmaAllocator(),
                           treeInstanceBuffers[frame].allocation, 0,
                           static_cast<VkDeviceSize>(treeCount) *
                               sizeof(vegetation::Instance));
    }

    vegetationPipeline->bind(cmd);

    const VkDescriptorSet globalSet =
        frameInfo.descriptorManager->getGlobalSet(frame);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                            1, &globalSet, 0, nullptr);

    VegetationPushConstantData push{};
    push.timeWind = glm::vec4(elapsedTime, frameInfo.settings.vegetationWindStrength,
                              0.0f, 0.0f);
    push.grassColor = frameInfo.settings.vegetationGrassColor;
    push.treeColor = frameInfo.settings.vegetationTreeColor;
    push.trunkColor = frameInfo.settings.vegetationTrunkColor;

    if (grassCount > 0)
    {
        push.timeWind.z = 0.0f;
        vkCmdPushConstants(cmd, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        VkBuffer vertexBuffers[] = { grassVertexBuffer.buffer,
                                     grassInstanceBuffers[frame].buffer };
        VkDeviceSize offsets[] = { 0, 0 };
        vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, grassIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, grassIndexCount, grassCount, 0, 0, 0);
    }

    if (treeCount > 0)
    {
        push.timeWind.z = 1.0f;
        vkCmdPushConstants(cmd, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        VkBuffer vertexBuffers[] = { treeVertexBuffer.buffer,
                                     treeInstanceBuffers[frame].buffer };
        VkDeviceSize offsets[] = { 0, 0 };
        vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, treeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, treeIndexCount, treeCount, 0, 0, 0);
    }

    frameInfo.telemetry.drawCalls += (grassCount > 0 ? 1u : 0u) +
                                     (treeCount > 0 ? 1u : 0u);
    frameInfo.telemetry.triangles +=
        grassCount * (grassIndexCount / 3u) + treeCount * (treeIndexCount / 3u);
    frameInfo.telemetry.vertices +=
        grassCount * (grassVertexBuffer.size / sizeof(Vertex)) +
        treeCount * (treeVertexBuffer.size / sizeof(Vertex));
    frameInfo.telemetry.descriptorBinds += 1;

    if (!loggedFirstDraw)
    {
        std::cout << "[Vegetation] first draw | grass=" << grassCount
                  << " trees=" << treeCount << " draws="
                  << ((grassCount > 0 ? 1 : 0) + (treeCount > 0 ? 1 : 0))
                  << "\n";
        loggedFirstDraw = true;
    }
}

void VegetationPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;
    if (frame >= kFramesInFlight)
        return;

    auto& pass = graph.addGraphicsPass("Vegetation");
    pass.writeColor(resources.gbPosition[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeColor(resources.gbNormal[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeColor(resources.gbAlbedo[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeColor(resources.gbPbr[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeDepth(resources.gbDepth[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    pass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        if (!frameInfo.settings.vegetationEnabled)
            return;

        elapsedTime += frameInfo.frameTime;
        record(cmd, frameInfo, frame);
    });
}
