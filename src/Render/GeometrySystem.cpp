#include "GeometrySystem.h"
#include "Core/DescriptorManager.h"
#include "RHI/CommandUtils.h"
#include "RHI/GBuffer.h"
#include "AssetPaths.h"
#include "Render/SubMeshCulling.h"
#include "Render/SceneGpuBatch.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

GeometrySystem::GeometrySystem(Device& device, VkFormat depthFormat,
                               VkDescriptorSetLayout globalSetLayout,
                               VkDescriptorSetLayout materialSetLayout,
                               VkDescriptorSetLayout terrainSetLayout,
                               VkDescriptorSetLayout sceneInstanceSetLayout,
                               SceneGpuBatch* sceneGpuBatch)
    : vulkanDevice(device), depthFormat(depthFormat),
      sceneGpuBatch(sceneGpuBatch), sceneInstanceSetLayout(sceneInstanceSetLayout)
{
    createPipelineLayout(globalSetLayout, materialSetLayout, terrainSetLayout, sceneInstanceSetLayout);
    createPipelines();
    createTerrainIndirectBuffers();
    createTerrainGpuSelection();
}

GeometrySystem::~GeometrySystem()
{
    destroyTerrainGpuSelection();
    destroyTerrainIndirectBuffers();
    vkDestroyPipelineLayout(vulkanDevice.getDevice(), pipelineLayout, nullptr);
}

namespace
{
struct TerrainGpuPushConstants
{
    glm::mat4 viewProjection{ 1.0f };
    glm::vec4 cameraPosition{ 0.0f }; // xyz, w=deltaTime
    glm::uvec4 counts{ 0u };          // x=recordCount
    glm::vec4 lodTiming{ 0.24f, 0.15f, 0.0f, 0.0f };
};
static_assert(sizeof(TerrainGpuPushConstants) == 112,
              "terrain_lod.comp push constant layout mismatch");
} // namespace

void GeometrySystem::createTerrainGpuBuffer(TerrainGpuBuffer& buffer,
                                             VkDeviceSize size,
                                             VkBufferUsageFlags usage,
                                             bool hostVisible)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible)
    {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    else
    {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocationInfo allocationInfo{};
    if (vmaCreateBuffer(vulkanDevice.getVmaAllocator(), &bufferInfo, &allocInfo,
                        &buffer.buffer, &buffer.allocation,
                        &allocationInfo) != VK_SUCCESS)
    {
        throw std::runtime_error("GeometrySystem: GPU terrain selection buffer creation failed");
    }
    buffer.mapped = allocationInfo.pMappedData;
    buffer.size = size;
    if (hostVisible && !buffer.mapped)
        throw std::runtime_error("GeometrySystem: GPU terrain selection buffer is not mapped");
}

void GeometrySystem::destroyTerrainGpuBuffer(TerrainGpuBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(vulkanDevice.getVmaAllocator(), buffer.buffer, buffer.allocation);
    buffer = {};
}

void GeometrySystem::createTerrainGpuSelection()
{
    terrainGpuSelectionAvailable = vulkanDevice.supportsDrawIndirectCount();
    if (!terrainGpuSelectionAvailable)
    {
        std::cout << "[Terrain] GPU selection unavailable | drawIndirectCount=0; using CPU fallback\n";
        return;
    }

    // Descriptor set: records / persistent LOD state / commands / counts /
    // stats / debug mirror. All frames use the same layout, different buffers.
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (uint32_t i = 0; i < 6u; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &layoutInfo, nullptr,
                                    &terrainGpuSetLayout) != VK_SUCCESS)
        throw std::runtime_error("GeometrySystem: GPU terrain descriptor layout failed");

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 6u * kFramesInFlight;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &poolInfo, nullptr,
                               &terrainGpuDescriptorPool) != VK_SUCCESS)
        throw std::runtime_error("GeometrySystem: GPU terrain descriptor pool failed");

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(terrainGpuSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = terrainGpuDescriptorPool;
    allocateInfo.descriptorSetCount = kFramesInFlight;
    allocateInfo.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, kFramesInFlight> sets{};
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &allocateInfo, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("GeometrySystem: GPU terrain descriptor allocation failed");

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(TerrainGpuPushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &terrainGpuSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &pipelineLayoutInfo, nullptr,
                               &terrainGpuPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("GeometrySystem: GPU terrain pipeline layout failed");

    terrainGpuPipeline = std::make_unique<ComputePipeline>(
        vulkanDevice, TINY_SHADER_DIR "terrain_lod.comp.spv",
        terrainGpuPipelineLayout);

    const VkDeviceSize recordBytes =
        static_cast<VkDeviceSize>(kTerrainGpuMaxRecords) * sizeof(TerrainGpuChunkRecord);
    const VkDeviceSize stateBytes =
        static_cast<VkDeviceSize>(kTerrainGpuMaxEntities * kTerrainGpuChunksPerEntity) *
        4u * sizeof(int32_t);
    const VkDeviceSize countBytes =
        static_cast<VkDeviceSize>(kTerrainGpuMaxEntities) * sizeof(uint32_t);
    const VkDeviceSize statsBytes =
        static_cast<VkDeviceSize>(kTerrainGpuStatsCount) * sizeof(uint32_t);
    const VkDeviceSize debugBytes =
        static_cast<VkDeviceSize>(kTerrainGpuMaxRecords) *
        kTerrainGpuDebugWords * sizeof(uint32_t);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        TerrainGpuFrameResources& resources = terrainGpuFrames[frame];
        createTerrainGpuBuffer(resources.records, recordBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createTerrainGpuBuffer(resources.state, stateBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        // Persistent LOD state is read/written by a later frame slot before the
        // first explicit entity reset reaches that slot. Undefined contents can
        // look like a valid in-flight transition, so start every slot at zero.
        std::memset(resources.state.mapped, 0, static_cast<size_t>(stateBytes));
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(),
                           resources.state.allocation, 0, VK_WHOLE_SIZE);
        createTerrainGpuBuffer(resources.counts, countBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        createTerrainGpuBuffer(resources.stats, statsBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createTerrainGpuBuffer(resources.debug, debugBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

        VkDescriptorBufferInfo infos[6]{};
        infos[0] = { resources.records.buffer, 0, VK_WHOLE_SIZE };
        infos[1] = { resources.state.buffer, 0, VK_WHOLE_SIZE };
        infos[2] = { terrainIndirectBuffers[frame].buffer, 0, VK_WHOLE_SIZE };
        infos[3] = { resources.counts.buffer, 0, VK_WHOLE_SIZE };
        infos[4] = { resources.stats.buffer, 0, VK_WHOLE_SIZE };
        infos[5] = { resources.debug.buffer, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[6]{};
        for (uint32_t i = 0; i < 6u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = sets[frame];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 6, writes, 0, nullptr);
        resources.descriptorSet = sets[frame];
        resources.debugValid = false;
        resources.parityExpected.clear();
    }

    std::cout << "[Terrain] GPU selection ready | entities=" << kTerrainGpuMaxEntities
              << " records=" << kTerrainGpuMaxRecords
              << " commandsPerEntity=" << kTerrainGpuCommandsPerEntity
              << " drawIndirectCount=1\n";
}

void GeometrySystem::destroyTerrainGpuSelection()
{
    terrainGpuPipeline.reset();
    for (TerrainGpuFrameResources& resources : terrainGpuFrames)
    {
        destroyTerrainGpuBuffer(resources.records);
        destroyTerrainGpuBuffer(resources.state);
        destroyTerrainGpuBuffer(resources.counts);
        destroyTerrainGpuBuffer(resources.stats);
        destroyTerrainGpuBuffer(resources.debug);
        resources.descriptorSet = VK_NULL_HANDLE;
        resources.debugValid = false;
        resources.parityExpected.clear();
    }
    if (terrainGpuDescriptorPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), terrainGpuDescriptorPool, nullptr);
    terrainGpuDescriptorPool = VK_NULL_HANDLE;
    if (terrainGpuSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vulkanDevice.getDevice(), terrainGpuSetLayout, nullptr);
    terrainGpuSetLayout = VK_NULL_HANDLE;
    if (terrainGpuPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vulkanDevice.getDevice(), terrainGpuPipelineLayout, nullptr);
    terrainGpuPipelineLayout = VK_NULL_HANDLE;
}

void GeometrySystem::createTerrainIndirectBuffers()
{
    const VkDeviceSize size = static_cast<VkDeviceSize>(kTerrainIndirectCapacity) *
                              sizeof(VkDrawIndexedIndirectCommand);
    for (TerrainIndirectBuffer& indirect : terrainIndirectBuffers)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocationInfo{};
        if (vmaCreateBuffer(vulkanDevice.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &indirect.buffer, &indirect.allocation,
                            &allocationInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("GeometrySystem: terrain indirect buffer creation failed");
        }
        indirect.mapped = allocationInfo.pMappedData;
        indirect.capacity = kTerrainIndirectCapacity;
        if (!indirect.mapped)
            throw std::runtime_error("GeometrySystem: terrain indirect buffer is not mapped");
    }
}

void GeometrySystem::destroyTerrainIndirectBuffers()
{
    for (TerrainIndirectBuffer& indirect : terrainIndirectBuffers)
    {
        // VMA_ALLOCATION_CREATE_MAPPED_BIT keeps the mapping owned by the
        // allocation; vmaDestroyBuffer releases it. Calling vmaUnmapMemory()
        // here asserts because no explicit vmaMapMemory() was made.
        if (indirect.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(vulkanDevice.getVmaAllocator(), indirect.buffer,
                             indirect.allocation);
        indirect = {};
    }
}

void GeometrySystem::createPipelineLayout(VkDescriptorSetLayout globalSetLayout,
                                          VkDescriptorSetLayout materialSetLayout,
                                          VkDescriptorSetLayout terrainSetLayout,
                                          VkDescriptorSetLayout sceneInstanceSetLayout)
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(SimplePushConstantData);

    std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ globalSetLayout, materialSetLayout, terrainSetLayout };
    // F1：set 3 = 实例缓冲。没有 GPU 批时也照常建布局（复用 set 0 的布局占位），
    // 免得管线布局随运行期状态变化而需要重建。
    descriptorSetLayouts.push_back(sceneInstanceSetLayout != VK_NULL_HANDLE
                                       ? sceneInstanceSetLayout
                                       : globalSetLayout);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(descriptorSetLayouts.size());
    pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("GeometrySystem: pipeline layout failed!");
}

void GeometrySystem::createPipelines()
{
    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);

    // 动态渲染：附件格式必须与渲染图里 writeColor 的顺序完全一致
    config.colorAttachmentFormats = {
        GBufferFormat::Position, GBufferFormat::Normal, GBufferFormat::Albedo, GBufferFormat::Pbr
    };
    config.depthAttachmentFormat = depthFormat;
    config.pipelineLayout = pipelineLayout;

    // 4 个颜色附件，混合全部关闭
    VkPipelineColorBlendAttachmentState defaultAttachment = config.colorBlendAttachments[0];
    config.colorBlendAttachments.assign(4, defaultAttachment);
    config.colorBlendInfo.attachmentCount = 4;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    geometryPipeline = std::make_unique<Pipeline>(vulkanDevice,
        TINY_SHADER_DIR "mrt.vert.spv", TINY_SHADER_DIR "mrt.frag.spv", config);
    gridPipeline = std::make_unique<Pipeline>(vulkanDevice,
        TINY_SHADER_DIR "grid_mrt.vert.spv", TINY_SHADER_DIR "grid_mrt.frag.spv", config);

    // 天空盒：不写深度、深度比较改成 LESS_OR_EQUAL（配合 gl_Position = pos.xyww）
    config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    skyboxPipeline = std::make_unique<Pipeline>(vulkanDevice,
        TINY_SHADER_DIR "skybox_mrt.vert.spv", TINY_SHADER_DIR "skybox_mrt.frag.spv", config);
}

void GeometrySystem::render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo)
{
    DescriptorManager& descMgr = *frameInfo.descriptorManager;

    // -----------------------------------------------------------------------
    // D-3 bindless：**整帧只绑一次 set 1**。
    //
    // 以前是"每个子网格切材质就 vkCmdBindDescriptorSets 一次"—— Sponza 有
    // 103 个子网格，于是 104 次 draw 伴随多达 104 次描述符集绑定。
    // 现在材质纹理在一个数组里，下标走 push constant，绑定与绘制彻底解耦。
    // -----------------------------------------------------------------------
    VkDescriptorSet sets[] = { descMgr.getGlobalSet(frameInfo.frameInFlightIndex), descMgr.getMaterialSet(), descMgr.getTerrainSet() };
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout, 0, 3, sets, 0, nullptr);
    frameInfo.telemetry.descriptorBinds += 1;

    // F1：set 3（实例缓冲）在所有图形管线里静态使用，所以整帧也要绑一次，
    // 否则地形/天空盒这些仍走旧路径的绘制会因为 set 3 未绑而通不过验证层。
    if (sceneGpuBatch != nullptr && sceneGpuBatch->isAvailable())
    {
        VkDescriptorSet instanceSet = sceneGpuBatch->getInstanceSet(frameInfo.frameInFlightIndex);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout, 3, 1, &instanceSet, 0, nullptr);
        frameInfo.telemetry.descriptorBinds += 1;
    }

    // B-08: terrain material sets are a bounded array. Most frames need one
    // bind; a batch switch occurs only when adjacent terrain entities select a
    // different set. The texture array itself remains bindless.
    uint32_t boundTerrainMaterialSet = 0;
    uint32_t terrainMaterialBatchSet = 0;
    bool terrainMaterialBatchOpen = false;

    // 没材质时用哪个下标：数组尾部允许未绑定（descriptorBindingPartiallyBound），
    // 但"没绑定的内容"采出来是未定义值。所以退化路径不指向数组，
    // 而是让着色器走 materialValid = 0 的分支（直接输出顶点色 / 默认 PBR）。
    const uint32_t fallbackMatId = 0;
    const render::SubMeshCuller subMeshCuller(
        frameInfo.projectionMatrix * frameInfo.viewMatrix);
    const glm::vec3 cameraPosition = frameInfo.cameraTransform
        ? frameInfo.cameraTransform->getTranslation()
        : glm::vec3(0.0f);

    struct ChunkLodSelection
    {
        bool ready = false;
        bool visible = true;
        glm::vec3 worldMin{ 0.0f };
        glm::vec3 worldMax{ 0.0f };
        render::TerrainLodSelection transition{};
    };

    if (frameInfo.frameNumber % 120u == 0u)
    {
        for (auto it = terrainLodStates.begin(); it != terrainLodStates.end();)
        {
            if (it->second.lastFrame + 120u < frameInfo.frameNumber)
                it = terrainLodStates.erase(it);
            else
                ++it;
        }
    }

    TerrainIndirectBuffer& indirectBuffer =
        terrainIndirectBuffers[frameInfo.frameInFlightIndex];
    auto* indirectCommands =
        static_cast<VkDrawIndexedIndirectCommand*>(indirectBuffer.mapped);
    uint32_t indirectCursor = 0;

    // GPU compute selection was recorded in a pre-graphics graph pass.
    const TerrainGpuRunContext& gpuRun = currentTerrainGpuRun;

    for (entt::entity entity : frameInfo.visibleEntities)
    {
        const MeshComponent* mesh = frameInfo.registry.try_get<MeshComponent>(entity);
        if (!mesh || !mesh->model)
            continue;

        const WorldTransformComponent* worldTransform =
            frameInfo.registry.try_get<WorldTransformComponent>(entity);
        if (!worldTransform)
            continue;

        const RenderTagComponent* tag = frameInfo.registry.try_get<RenderTagComponent>(entity);
        const MaterialComponent* materialComp = frameInfo.registry.try_get<MaterialComponent>(entity);

        const bool isSkybox = tag && tag->isSkybox;
        const bool isGrid = tag && tag->isGrid;
        const bool isTerrain = tag && tag->isTerrain;
        const TerrainComponent* terrain = frameInfo.registry.try_get<TerrainComponent>(entity);
        const uint32_t entityId = static_cast<uint32_t>(entt::to_integral(entity));
        const auto gpuSlotIt = gpuRun.entitySlots.find(entityId);
        const bool terrainGpuEntity =
            isTerrain && gpuRun.enabled && gpuSlotIt != gpuRun.entitySlots.end();
        const uint32_t terrainGpuSlot = terrainGpuEntity ? gpuSlotIt->second : 0u;

        // When GPU selection is active, never mix in CPU-generated indirect
        // commands for a terrain entity that could not get a slot: both paths
        // would write the same indirect buffer. Skipping the overflow entity is
        // safer and explicit (capacity is 64 entities / 1024 chunk records).
        if (isTerrain && gpuRun.enabled && !terrainGpuEntity)
            continue;

        // F1：普通网格交给 SceneGpuBatch 的 GPU 驱动提交（实例缓冲 + compute 剔除 +
        // indirect draw）。天空盒、网格辅助线、地形仍然留在下面的 CPU 路径。
        if (sceneGpuBatch != nullptr && sceneGpuBatch->useGpuPath(frameInfo) &&
            !isSkybox && !isGrid && !isTerrain &&
            mesh->model->getIndexCount() > 0)
        {
            continue;
        }

        if (isTerrain)
        {
            uint32_t materialSet = terrain ? terrain->config.materialSetIndex : 0u;
            if (materialSet >= DescriptorManager::kTerrainSetCount)
            {
                materialSet = 0u;
                ++frameInfo.telemetry.terrainMaterialFallbacks;
            }
            if (!terrainMaterialBatchOpen || materialSet != terrainMaterialBatchSet)
            {
                ++frameInfo.telemetry.terrainMaterialBatches;
                if (terrainMaterialBatchOpen)
                    ++frameInfo.telemetry.terrainMaterialSwitches;
                terrainMaterialBatchOpen = true;
                terrainMaterialBatchSet = materialSet;
            }
            if (materialSet != boundTerrainMaterialSet)
            {
                VkDescriptorSet terrainSet = descMgr.getTerrainSet(materialSet);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 2, 1, &terrainSet, 0, nullptr);
                boundTerrainMaterialSet = materialSet;
                frameInfo.telemetry.descriptorBinds += 1;
            }
        }

        uint32_t terrainLodCount = 1;
        float terrainLodDistanceScale = 1.0f;
        if (isTerrain && terrain)
        {
            terrainLodCount = std::clamp(terrain->config.lodLevelCount, 1u, 3u);
            terrainLodDistanceScale = terrain->config.lodDistanceScale;
        }
        TerrainEntityLodState* terrainState = nullptr;
        if (isTerrain && terrain && mesh->model && !terrainGpuEntity)
        {
            TerrainEntityLodState& state = terrainLodStates[entityId];
            if (state.model != mesh->model.get() || state.revision != terrain->revision)
            {
                state = TerrainEntityLodState{};
                state.model = mesh->model.get();
                state.revision = terrain->revision;
            }
            state.lastFrame = frameInfo.frameNumber;
            terrainState = &state;
        }
        std::array<ChunkLodSelection, 16> chunkLodSelections{};

        const auto ensureTerrainChoice = [&](const SubMesh& subMesh) -> ChunkLodSelection&
        {
            const uint32_t key = subMesh.terrainChunkZ * 4u + subMesh.terrainChunkX;
            ChunkLodSelection& selection = chunkLodSelections[key % 16u];
            if (selection.ready)
                return selection;

            selection.ready = true;
            render::worldBounds(subMesh.boundsMin, subMesh.boundsMax,
                                worldTransform->matrix,
                                selection.worldMin, selection.worldMax);
            selection.visible = subMeshCuller.intersects(
                subMesh.boundsMin, subMesh.boundsMax,
                worldTransform->matrix);
            if (!selection.visible)
            {
                frameInfo.telemetry.subMeshesCulled += 1;
                return selection;
            }

            const glm::vec3 closest = glm::clamp(
                cameraPosition, selection.worldMin, selection.worldMax);
            const float distance = glm::length(cameraPosition - closest);
            const float extent = std::max(
                selection.worldMax.x - selection.worldMin.x,
                selection.worldMax.z - selection.worldMin.z);

            if (terrainState)
            {
                render::TerrainLodState& lodState = terrainState->chunks[key % 16u];
                selection.transition = render::updateTerrainLodSelection(
                    distance, extent, terrainLodDistanceScale, terrainLodCount,
                    frameInfo.frameTime, lodState);
                if (selection.transition.blending)
                {
                    frameInfo.telemetry.terrainLodBlends += 1u;
                    if (lodState.transitionElapsed == 0.0f &&
                        lodState.transitionFrom != lodState.transitionTo)
                        frameInfo.telemetry.terrainLodSwitches += 1u;
                }
            }
            else
            {
                selection.transition.lodA = render::selectTerrainLod(
                    distance, extent, terrainLodDistanceScale, terrainLodCount);
                selection.transition.lodB = selection.transition.lodA;
                selection.transition.blend = 1.0f;
            }
            return selection;
        };

        bool terrainBlending = false;
        uint32_t visibleChunkCount = 0;
        if (isTerrain && !terrainGpuEntity)
        {
            std::array<bool, 16> countedChunks{};
            for (const SubMesh& subMesh : mesh->model->getSubMeshes())
            {
                if (!subMesh.isTerrainChunk)
                    continue;
                const ChunkLodSelection& selection = ensureTerrainChoice(subMesh);
                const uint32_t key = subMesh.terrainChunkZ * 4u + subMesh.terrainChunkX;
                if (selection.visible && !countedChunks[key % 16u])
                {
                    countedChunks[key % 16u] = true;
                    ++visibleChunkCount;
                    if (selection.transition.blending)
                        terrainBlending = true;
                }
            }
        }
        const bool terrainUseIndirect = isTerrain && !terrainGpuEntity &&
            !terrainBlending && vulkanDevice.supportsMultiDrawIndirect() &&
            visibleChunkCount > 0u &&
            indirectCursor + visibleChunkCount <= indirectBuffer.capacity;

        SimplePushConstantData push{};
        push.modelMatrix = worldTransform->matrix;
        push.normalMatrix = worldTransform->normalMatrix;

        const bool objectUsesMaterial = !isSkybox && !isGrid && !isTerrain;
        push.materialValid = objectUsesMaterial ? 1u : 0u;
        push.materialId = fallbackMatId;
        push.terrainMode = isTerrain ? 1u : 0u;

        // 实体自带材质、但材质没有 albedo 贴图 -> 整实体走占位棋盘格。
        // glTF 的子网格材质另算（SubMesh::albedoFallback），不能在这里一刀切。
        const bool entityAlbedoFallback = materialComp && materialComp->material &&
                                          !materialComp->material->albedoMap;

        // 棋盘格密度：让"最大尺寸上约 6 格"。
        // 不用固定常数是因为模型的局部尺寸差异极大（debug_cube 与 Quad-Sphere
        // 相差上百倍），固定频率会让大模型糊成平均灰、小模型只有一格。
        push.checkerScale = 1.0f;
        const auto* bounds = frameInfo.registry.try_get<BoundsComponent>(entity);
        if (bounds && bounds->getLocal().valid)
        {
            const AABB& localBounds = bounds->getLocal();
            const glm::vec3 extent = localBounds.max - localBounds.min;
            const float maxExtent = std::max({ extent.x, extent.y, extent.z });
            if (maxExtent > 1e-6f)
                push.checkerScale = 6.0f / maxExtent;   // 3D 棋盘格周期为 1
        }

        if (isTerrain)
        {
            if (bounds && bounds->getLocal().valid)
            {
                const AABB& localBounds = bounds->getLocal();
                push.terrainMinY = localBounds.min.y;
                push.terrainHeightRange = std::max(localBounds.max.y - localBounds.min.y, 1e-4f);
            }
            if (terrain)
            {
                push.terrainDetailStrength = terrain->config.materialDetailStrength;
                push.terrainUvTiling = terrain->config.uvTiling;
                push.terrainMaterialStyle = static_cast<uint32_t>(terrain->config.surfaceStyle);
            }
        }

        // UV 归一化：只有 UV 本身覆盖 [0,1] 附近时才用 UV 棋盘。
        // Quad-Sphere 的 UV 只占 0.05~0.43（图集子区间），强行归一化会在
        // 球面/接缝处变成大片扭曲的格子；这种情况改用 3D 棋盘。
        push.checkerUvTransform = glm::vec4(0.0f);
        if (mesh->model && mesh->model->hasUv())
        {
            const glm::vec2 uvMin = mesh->model->getUvMin();
            const glm::vec2 uvMax = mesh->model->getUvMax();
            const glm::vec2 uvExtent = uvMax - uvMin;
            if (uvExtent.x > 0.8f && uvExtent.y > 0.8f)
            {
                const float scaleU = 1.0f / uvExtent.x;
                const float scaleV = 1.0f / uvExtent.y;
                push.checkerUvTransform = glm::vec4(scaleU, scaleV, uvMin.x, uvMin.y);
            }
        }

        auto* pipe = &geometryPipeline;
        if (isSkybox)    pipe = &skyboxPipeline;
        else if (isGrid) pipe = &gridPipeline;

        (*pipe)->bind(commandBuffer);
        mesh->model->bind(commandBuffer);

        const uint32_t entityIndirectStart = indirectCursor;
        uint32_t entityIndirectCount = 0;
        uint32_t stableLodCounts[3] = {};
        uint64_t stableIndexCount = 0;

        for (const SubMesh& subMesh : mesh->model->getSubMeshes())
        {
            if (isTerrain && subMesh.isTerrainChunk)
            {
                if (terrainGpuEntity)
                    continue;
                const uint32_t key = subMesh.terrainChunkZ * 4u + subMesh.terrainChunkX;
                const ChunkLodSelection& selection = chunkLodSelections[key % 16u];
                if (!selection.visible)
                    continue;

                const bool drawLodA = subMesh.terrainLod == selection.transition.lodA;
                const bool drawLodB = selection.transition.blending &&
                                      subMesh.terrainLod == selection.transition.lodB;
                if (!drawLodA && !drawLodB)
                {
                    frameInfo.telemetry.lodSubMeshesSkipped += 1;
                    continue;
                }

                if (terrainUseIndirect)
                {
                    VkDrawIndexedIndirectCommand& command =
                        indirectCommands[indirectCursor++];
                    command.indexCount = subMesh.indexCount;
                    command.instanceCount = 1;
                    command.firstIndex = subMesh.firstIndex;
                    command.vertexOffset = 0;
                    command.firstInstance = 0;
                    ++entityIndirectCount;
                    if (subMesh.terrainLod < 3u)
                        stableLodCounts[subMesh.terrainLod] += 1u;
                    stableIndexCount += subMesh.indexCount;
                    continue;
                }
            }

            if (!subMesh.isTerrainChunk && subMesh.hasBounds &&
                !subMeshCuller.intersects(subMesh.boundsMin, subMesh.boundsMax,
                                          worldTransform->matrix))
            {
                frameInfo.telemetry.subMeshesCulled += 1;
                continue;
            }

            // 实体级材质槽位只认 MaterialComponent::materialSetIndex：
            //   * 它跟着实体生命周期走，clearEntities() 释放槽位时会一起清掉；
            //   * Material::descriptorSetIndex 是"材质对象"的字段，材质可能被多个
            //     实体共享，槽位释放后它会过期，不能拿来判断实体有没有槽位。
            // 之前这里优先读 material->descriptorSetIndex，而 createMeshEntity 只写了
            // MaterialComponent 那一份 —— 结果每帧都误判成"没有材质"并刷屏。
            const uint32_t objectMaterialSlot = materialComp ? materialComp->materialSetIndex
                                                             : kInvalidMaterialSlot;

            uint32_t slot = (subMesh.materialSlot != kInvalidMaterialSlot)
                ? subMesh.materialSlot
                : objectMaterialSlot;

            if (objectUsesMaterial && slot == kInvalidMaterialSlot)
            {
                const uint32_t entityId = entt::to_integral(entity);
                if (warnedFallbackEntities.insert(entityId).second)
                {
                    // 只提示一次，并且走 stdout：stderr 必须保持 0 字节（回归判据）。
                    std::cout << "[GeometrySystem] entity " << entityId
                              << " 没有有效材质槽位，使用棋盘格占位着色（仅提示一次）\n";
                }
                slot = fallbackMatId;
                push.materialValid = 0u;
            }
            else if (slot != kInvalidMaterialSlot)
            {
                push.materialValid = objectUsesMaterial ? 1u : 0u;
            }

            push.materialId = (slot == kInvalidMaterialSlot) ? fallbackMatId : slot;
            push.albedoFallback = (subMesh.albedoFallback || entityAlbedoFallback) ? 1u : 0u;
            push.alphaMasked = subMesh.alphaMasked ? 1u : 0u;
            push.alphaCutoff = subMesh.alphaCutoff;
            push.terrainLodBlend = 1.0f;
            push.terrainLodBlendMode = 0u;
            if (isTerrain && subMesh.isTerrainChunk)
            {
                const uint32_t key = subMesh.terrainChunkZ * 4u + subMesh.terrainChunkX;
                const ChunkLodSelection& selection = chunkLodSelections[key % 16u];
                if (selection.transition.blending)
                {
                    push.terrainLodBlend = selection.transition.blend;
                    if (subMesh.terrainLod == selection.transition.lodA)
                        push.terrainLodBlendMode = 1u;
                    else if (subMesh.terrainLod == selection.transition.lodB)
                        push.terrainLodBlendMode = 2u;
                }
            }
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SimplePushConstantData), &push);

            mesh->model->drawSubMesh(commandBuffer, subMesh);

            if (isTerrain && subMesh.isTerrainChunk)
            {
                if (subMesh.terrainLod == 0u)
                    frameInfo.telemetry.terrainLod0Chunks += 1u;
                else if (subMesh.terrainLod == 1u)
                    frameInfo.telemetry.terrainLod1Chunks += 1u;
                else
                    frameInfo.telemetry.terrainLod2Chunks += 1u;
            }

            frameInfo.telemetry.drawCalls += 1;
            frameInfo.telemetry.triangles += subMesh.indexCount / 3;
            frameInfo.telemetry.vertices += subMesh.indexCount;
        }

        if (terrainGpuEntity && gpuRun.frame)
        {
            push.terrainLodBlend = 1.0f;
            push.terrainLodBlendMode = 0u;
            push.materialValid = 0u;
            push.materialId = fallbackMatId;
            push.albedoFallback = 0u;
            push.alphaMasked = 0u;
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SimplePushConstantData), &push);

            const VkDeviceSize commandOffset =
                static_cast<VkDeviceSize>(terrainGpuSlot) *
                kTerrainGpuCommandsPerEntity *
                sizeof(VkDrawIndexedIndirectCommand);
            const VkDeviceSize countOffset =
                static_cast<VkDeviceSize>(terrainGpuSlot) * sizeof(uint32_t);
            vkCmdDrawIndexedIndirectCount(
                commandBuffer, indirectBuffer.buffer, commandOffset,
                gpuRun.frame->counts.buffer, countOffset,
                kTerrainGpuCommandsPerEntity,
                sizeof(VkDrawIndexedIndirectCommand));

            frameInfo.telemetry.terrainIndirectGroups += 1u;
        }
        else if (terrainUseIndirect && entityIndirectCount > 0u)
        {
            push.terrainLodBlend = 1.0f;
            push.terrainLodBlendMode = 0u;
            push.materialValid = 0u;
            push.materialId = fallbackMatId;
            push.albedoFallback = 0u;
            push.alphaMasked = 0u;
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SimplePushConstantData), &push);

            vkCmdDrawIndexedIndirect(
                commandBuffer, indirectBuffer.buffer,
                static_cast<VkDeviceSize>(entityIndirectStart) *
                    sizeof(VkDrawIndexedIndirectCommand),
                entityIndirectCount, sizeof(VkDrawIndexedIndirectCommand));

            frameInfo.telemetry.terrainIndirectGroups += 1u;
            frameInfo.telemetry.drawCalls += entityIndirectCount;
            frameInfo.telemetry.triangles += static_cast<uint32_t>(stableIndexCount / 3u);
            frameInfo.telemetry.vertices += static_cast<uint32_t>(stableIndexCount);
            frameInfo.telemetry.terrainLod0Chunks += stableLodCounts[0];
            frameInfo.telemetry.terrainLod1Chunks += stableLodCounts[1];
            frameInfo.telemetry.terrainLod2Chunks += stableLodCounts[2];
        }
    }

    if (indirectCursor > 0u)
    {
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(),
                           indirectBuffer.allocation, 0,
                           static_cast<VkDeviceSize>(indirectCursor) *
                               sizeof(VkDrawIndexedIndirectCommand));
    }

    // F1：GPU 驱动提交。间接命令与计数由 SceneGpuCull pass 写好，这里只按模型
    // 绑一次顶点/索引缓冲再提交；实例缓冲走 set 3。
    if (sceneGpuBatch != nullptr && sceneGpuBatch->useGpuPath(frameInfo))
    {
        SimplePushConstantData batchPush{};
        batchPush.gpuScene = 1u;
        vkCmdPushConstants(commandBuffer, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SimplePushConstantData), &batchPush);
        geometryPipeline->bind(commandBuffer);
        sceneGpuBatch->recordDraw(commandBuffer, frameInfo);
    }

}