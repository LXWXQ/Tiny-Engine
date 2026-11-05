#include "GlassPass.h"

#include "Scene/Components.h"
#include "AssetPaths.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>

static_assert(sizeof(GlassPushConstantData) == 32, "glass push constant must stay 32 bytes");

namespace
{
constexpr VkDeviceSize kVertexBytes =
    static_cast<VkDeviceSize>(sizeof(Vertex)) * 4ull * 65536ull;
constexpr VkDeviceSize kIndexBytes =
    static_cast<VkDeviceSize>(sizeof(uint32_t)) * 6ull * 65536ull;
}

GlassPass::GlassPass(Device& device, RenderGraph& graph, RenderResources& resources,
                     VkDescriptorSetLayout globalSetLayout, VkDescriptorSetLayout materialSetLayout,
                     DescriptorManager* descriptorManager, VkFormat depthFormat)
    : device(device), renderGraph(graph), resources(resources),
      descriptorManager(descriptorManager), globalSetLayout(globalSetLayout),
      materialSetLayout(materialSetLayout), depthFormat(depthFormat)
{
}

GlassPass::~GlassPass()
{
    // 先销毁管线，再销毁它引用的布局，最后才是缓冲。
    glassPipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    destroyBuffers();
}

void GlassPass::init()
{
    createBuffers();
    createPipeline();
}

void GlassPass::createBuffers()
{
    // 顶点池：所有玻璃面共用一份，主机可见、常驻映射。
    // 重建只发生在体素编辑时，量级很小，不值得配 staging buffer。
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kVertexBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>("GlassVertexPool");

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &vertexBuffer, &vertexAllocation, &info) != VK_SUCCESS)
            throw std::runtime_error("GlassPass: vertex pool creation failed");
        vertexMapped = info.pMappedData;
        if (vertexMapped == nullptr)
            throw std::runtime_error("GlassPass: vertex pool is not mapped");
    }

    // 索引每帧重排，所以每帧一份，避免覆盖仍在执行的上一帧。
    static const char* const names[kFramesInFlight] = { "GlassIndices.Frame0", "GlassIndices.Frame1" };
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kIndexBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>(names[frame]);

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &indexBuffers[frame], &indexAllocations[frame], &info) != VK_SUCCESS)
            throw std::runtime_error("GlassPass: index buffer creation failed");
        indexMapped[frame] = info.pMappedData;
        if (indexMapped[frame] == nullptr)
            throw std::runtime_error("GlassPass: index buffer is not mapped");
    }
}

void GlassPass::destroyBuffers()
{
    if (vertexBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(device.getVmaAllocator(), vertexBuffer, vertexAllocation);
    vertexBuffer = VK_NULL_HANDLE;
    vertexAllocation = VK_NULL_HANDLE;
    vertexMapped = nullptr;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (indexBuffers[frame] != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), indexBuffers[frame], indexAllocations[frame]);
        indexBuffers[frame] = VK_NULL_HANDLE;
        indexAllocations[frame] = VK_NULL_HANDLE;
        indexMapped[frame] = nullptr;
    }
}

void GlassPass::createPipeline()
{
    std::array<VkDescriptorSetLayout, 2> setLayouts{ globalSetLayout, materialSetLayout };

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(GlassPushConstantData);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("GlassPass: pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.depthAttachmentFormat = depthFormat;

    // 标准 over 混合：玻璃颜色按自身 alpha 压到已有 HDR 颜色上。
    // 写入 alpha 通道保持 1（dstAlpha 不变），避免后续 Bloom/Present 误用玻璃 alpha。
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

    // 透明物体不写深度，但必须按 G-Buffer 深度做遮挡测试（LESS_OR_EQUAL：
    // 玻璃面和它贴身的不透明面可能共面）。
    config.depthStencilInfo.depthTestEnable = VK_TRUE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    config.bindingDescriptions = { Vertex::getBindingDescription() };
    config.attributeDescriptions = Vertex::getAttributeDescriptions();

    glassPipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "glass.vert.spv", TINY_SHADER_DIR "glass.frag.spv", config);
}

void GlassPass::refreshChunkStatesAndRebuild(FrameInfo& frameInfo)
{
    std::unordered_map<uint32_t, ChunkState> current;
    auto view = frameInfo.registry.view<VoxelChunkComponent>();
    for (auto entity : view)
    {
        const VoxelChunkComponent& chunk = view.get<VoxelChunkComponent>(entity);
        if (chunk.glassFaceCount == 0u || !chunk.glassMesh)
            continue;

        const WorldTransformComponent* world =
            frameInfo.registry.try_get<WorldTransformComponent>(entity);
        ChunkState state;
        state.meshRevision = chunk.revision;
        state.transformRevision = world != nullptr ? world->sourceRevision : 0ull;
        current.emplace(static_cast<uint32_t>(entity), state);
    }

    bool changed = current.size() != chunkStates.size();
    if (!changed)
    {
        for (const auto& [key, state] : current)
        {
            const auto it = chunkStates.find(key);
            if (it == chunkStates.end() ||
                it->second.meshRevision != state.meshRevision ||
                it->second.transformRevision != state.transformRevision)
            {
                changed = true;
                break;
            }
        }
    }
    if (!changed)
        return;

    chunkStates = std::move(current);
    rebuildVertexPool(frameInfo);
}

void GlassPass::rebuildVertexPool(FrameInfo& frameInfo)
{
    faces.clear();
    sortedFaces.clear();
    vertexCount = 0;
    faceCount = 0;
    materialId = 0xFFFFFFFFu;

    Vertex* dst = static_cast<Vertex*>(vertexMapped);
    uint32_t chosenMaterial = 0xFFFFFFFFu;
    bool poolFull = false;

    auto view = frameInfo.registry.view<VoxelChunkComponent>();
    for (auto entity : view)
    {
        const VoxelChunkComponent& chunk = view.get<VoxelChunkComponent>(entity);
        if (chunk.glassFaceCount == 0u || !chunk.glassMesh)
            continue;

        const WorldTransformComponent* world =
            frameInfo.registry.try_get<WorldTransformComponent>(entity);
        if (world == nullptr)
            continue;

        // 所有体素 chunk 共用同一份 "voxel" 材质；取第一个有效槽位即可。
        if (chosenMaterial == 0xFFFFFFFFu)
        {
            if (const MaterialComponent* material = frameInfo.registry.try_get<MaterialComponent>(entity))
            {
                if (material->materialSetIndex != kInvalidMaterialSlot)
                    chosenMaterial = material->materialSetIndex;
            }
        }

        const std::vector<terrain::TerrainVertex>& source = chunk.glassMesh->vertices;
        const uint32_t sourceFaces = std::min<uint32_t>(
            chunk.glassFaceCount, static_cast<uint32_t>(source.size() / 4u));
        for (uint32_t face = 0; face < sourceFaces; ++face)
        {
            if (faceCount >= kMaxGlassFaces)
            {
                poolFull = true;
                break;
            }

            const uint32_t baseVertex = vertexCount;
            glm::vec3 centroid(0.0f);
            for (uint32_t corner = 0; corner < 4u; ++corner)
            {
                const terrain::TerrainVertex& src = source[face * 4u + corner];
                Vertex& out = dst[vertexCount++];
                const glm::vec4 worldPos = world->matrix * glm::vec4(src.position, 1.0f);
                out.position = glm::vec3(worldPos);
                out.color = src.color;
                out.normal = glm::normalize(glm::mat3(world->normalMatrix) * src.normal);
                out.uv = src.uv;
                centroid += out.position;
            }
            centroid *= 0.25f;
            faces.push_back(FaceRecord{ baseVertex, centroid });
            ++faceCount;
        }
        if (poolFull)
            break;
    }

    if (vertexCount > 0u)
    {
        vmaFlushAllocation(device.getVmaAllocator(), vertexAllocation, 0,
                           static_cast<VkDeviceSize>(vertexCount) * sizeof(Vertex));
    }

    if (chosenMaterial == 0xFFFFFFFFu)
    {
        // 没有材质槽时 shader 无法采样图集；退回 0 号槽，至少不会读到非法下标。
        if (faceCount > 0u && !loggedEmpty)
        {
            std::cout << "[Glass] no material slot on glass chunks; falling back to slot 0\n";
            loggedEmpty = true;
        }
        chosenMaterial = 0u;
    }
    materialId = chosenMaterial;
    lastLoggedFaceCount = 0xFFFFFFFFu;

    if (poolFull && !poolOverflowLogged)
    {
        std::cout << "[Glass] face pool full (" << kMaxGlassFaces
                  << "), extra glass faces skipped\n";
        poolOverflowLogged = true;
    }
}

uint32_t GlassPass::writeSortedIndices(uint32_t frame, const glm::vec3& cameraPosition)
{
    const uint32_t count = static_cast<uint32_t>(faces.size());
    sortedFaces.resize(count);
    std::iota(sortedFaces.begin(), sortedFaces.end(), 0u);

    std::sort(sortedFaces.begin(), sortedFaces.end(), [this, &cameraPosition](uint32_t a, uint32_t b)
    {
        const glm::vec3 da = faces[a].centroid - cameraPosition;
        const glm::vec3 db = faces[b].centroid - cameraPosition;
        return glm::dot(da, da) > glm::dot(db, db);
    });

    uint32_t* dst = static_cast<uint32_t*>(indexMapped[frame]);
    uint32_t written = 0;
    for (uint32_t index : sortedFaces)
    {
        const uint32_t base = faces[index].baseVertex;
        dst[written++] = base + 0u;
        dst[written++] = base + 1u;
        dst[written++] = base + 2u;
        dst[written++] = base + 0u;
        dst[written++] = base + 2u;
        dst[written++] = base + 3u;
    }

    if (written > 0u)
    {
        vmaFlushAllocation(device.getVmaAllocator(), indexAllocations[frame], 0,
                           static_cast<VkDeviceSize>(written) * sizeof(uint32_t));
    }
    return written;
}

void GlassPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;
    if (frame >= kFramesInFlight || frame >= indexBuffers.size() || indexBuffers[frame] == VK_NULL_HANDLE)
        return;

    auto& pass = graph.addGraphicsPass("Glass");
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeDepth(resources.gbDepth[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    pass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        refreshChunkStatesAndRebuild(frameInfo);
        if (faceCount == 0u || glassPipeline == nullptr)
            return;

        glm::vec3 cameraPosition(0.0f);
        if (frameInfo.cameraTransform != nullptr)
            cameraPosition = frameInfo.cameraTransform->getTranslation();
        else
            cameraPosition = glm::vec3(glm::inverse(frameInfo.viewMatrix)[3]);

        const uint32_t indexCount = writeSortedIndices(frame, cameraPosition);
        if (indexCount == 0u)
            return;

        glassPipeline->bind(cmd);

        VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
        VkDescriptorSet sets[] = { globalSet, descriptorManager->getMaterialSet() };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);

        const EngineSettings& settings = frameInfo.settings;
        GlassPushConstantData push{};
        push.materialId = materialId;
        push.alpha = 0.45f;
        push.fogParams = glm::vec4(std::max(settings.fogStart, 0.0f),
            std::max(settings.fogDensity, 0.0f), 0.0f,
            settings.fogEnabled ? 1.0f : 0.0f);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(GlassPushConstantData), &push);

        VkBuffer vertexBuffers[] = { vertexBuffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, indexBuffers[frame], 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);

        frameInfo.telemetry.drawCalls += 1;
        frameInfo.telemetry.triangles += faceCount * 2u;
        frameInfo.telemetry.vertices += faceCount * 4u;
        frameInfo.telemetry.descriptorBinds += 1;

        if (lastLoggedFaceCount != faceCount)
        {
            std::cout << "[Glass] sorted=" << faceCount << " drawcalls=1\n";
            lastLoggedFaceCount = faceCount;
        }
    });
}
