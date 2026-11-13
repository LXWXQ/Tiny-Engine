#include "GeometrySystem.h"
#include "Core/DescriptorManager.h"
#include "RHI/CommandUtils.h"
#include "RHI/GBuffer.h"
#include "AssetPaths.h"
#include "Render/SubMeshCulling.h"
#include "Render/SceneGpuBatch.h"
#include "Scene/RenderInterpolation.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

GeometrySystem::GeometrySystem(Device& device, VkFormat depthFormat,
                               VkDescriptorSetLayout globalSetLayout,
                               VkDescriptorSetLayout materialSetLayout,
                               VkDescriptorSetLayout terrainSetLayout,
                               VkDescriptorSetLayout sceneInstanceSetLayout,
                               SceneGpuBatch* sceneGpuBatch,
                               TerrainMeshArena* terrainMeshArena)
    : vulkanDevice(device), depthFormat(depthFormat),
      sceneGpuBatch(sceneGpuBatch), sceneInstanceSetLayout(sceneInstanceSetLayout),
      terrainMeshArena(terrainMeshArena)
{
    createPipelineLayout(globalSetLayout, materialSetLayout, terrainSetLayout, sceneInstanceSetLayout);
    createPipelines();
    createTerrainIndirectBuffers();
    createTerrainGpuSelection();
}

GeometrySystem::~GeometrySystem()
{
    destroyVoxelBatchResources();
    destroyTerrainChunkInstances();
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

bool GeometrySystem::ensureTerrainChunkInstances()
{
    if (terrainChunkInstancesReady)
        return true;
    if (sceneInstanceSetLayout == VK_NULL_HANDLE)
        return false;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(kTerrainGpuMaxEntities) *
                               kTerrainChunkInstanceStride;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (terrainChunkInstances[frame].buffer == VK_NULL_HANDLE)
            createTerrainGpuBuffer(terrainChunkInstances[frame], bytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (terrainChunkInstances[frame].mapped != nullptr)
            std::memset(terrainChunkInstances[frame].mapped, 0, static_cast<size_t>(bytes));
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = kFramesInFlight;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &poolInfo, nullptr,
                               &terrainChunkInstancePool) != VK_SUCCESS)
    {
        std::cout << "[Terrain] chunk instance table unavailable | descriptor pool" << std::endl;
        return false;
    }

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(sceneInstanceSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = terrainChunkInstancePool;
    allocInfo.descriptorSetCount = kFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &allocInfo,
                                terrainChunkInstanceSets.data()) != VK_SUCCESS)
    {
        std::cout << "[Terrain] chunk instance table unavailable | descriptor sets" << std::endl;
        return false;
    }

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkDescriptorBufferInfo bufferInfo{ terrainChunkInstances[frame].buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = terrainChunkInstanceSets[frame];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 1, &write, 0, nullptr);
    }

    terrainChunkInstancesReady = true;
    return true;
}

void GeometrySystem::destroyTerrainChunkInstances()
{
    for (TerrainGpuBuffer& buffer : terrainChunkInstances)
    {
        if (buffer.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(vulkanDevice.getVmaAllocator(), buffer.buffer, buffer.allocation);
        buffer = {};
    }
    terrainChunkInstanceSets.fill(VK_NULL_HANDLE);
    if (terrainChunkInstancePool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), terrainChunkInstancePool, nullptr);
    terrainChunkInstancePool = VK_NULL_HANDLE;
    terrainChunkInstancesReady = false;
    // F2-f：arena 归 Scene 所有，这里只清掉借用指针。
    terrainMeshArena = nullptr;
}

bool GeometrySystem::ensureVoxelBatchResources()
{
    if (voxelChunkInstancesReady)
        return true;
    if (sceneInstanceSetLayout == VK_NULL_HANDLE)
        return false;

    const VkDeviceSize instanceBytes =
        static_cast<VkDeviceSize>(kVoxelBatchMaxChunks) * kVoxelBatchInstanceStride;
    const VkDeviceSize indirectBytes =
        static_cast<VkDeviceSize>(kVoxelBatchMaxCommands) *
        sizeof(VkDrawIndexedIndirectCommand);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (voxelChunkInstances[frame].buffer == VK_NULL_HANDLE)
            createTerrainGpuBuffer(voxelChunkInstances[frame], instanceBytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (voxelChunkInstances[frame].mapped != nullptr)
            std::memset(voxelChunkInstances[frame].mapped, 0, static_cast<size_t>(instanceBytes));
        if (voxelIndirectBuffers[frame].buffer == VK_NULL_HANDLE)
            createTerrainGpuBuffer(voxelIndirectBuffers[frame], indirectBytes,
                                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = kFramesInFlight;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &poolInfo, nullptr,
                               &voxelChunkInstancePool) != VK_SUCCESS)
    {
        voxelChunkInstancePool = VK_NULL_HANDLE;
        return false;
    }

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(sceneInstanceSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = voxelChunkInstancePool;
    allocInfo.descriptorSetCount = kFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &allocInfo,
                                voxelChunkInstanceSets.data()) != VK_SUCCESS)
    {
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), voxelChunkInstancePool, nullptr);
        voxelChunkInstancePool = VK_NULL_HANDLE;
        voxelChunkInstanceSets.fill(VK_NULL_HANDLE);
        return false;
    }

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkDescriptorBufferInfo bufferInfo{ voxelChunkInstances[frame].buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = voxelChunkInstanceSets[frame];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 1, &write, 0, nullptr);
    }

    voxelChunkInstancesReady = true;
    return true;
}

void GeometrySystem::destroyVoxelBatchResources()
{
    for (TerrainGpuBuffer& buffer : voxelChunkInstances)
        destroyTerrainGpuBuffer(buffer);
    for (TerrainGpuBuffer& buffer : voxelIndirectBuffers)
        destroyTerrainGpuBuffer(buffer);
    voxelChunkInstanceSets.fill(VK_NULL_HANDLE);
    if (voxelChunkInstancePool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), voxelChunkInstancePool, nullptr);
    voxelChunkInstancePool = VK_NULL_HANDLE;
    voxelChunkInstancesReady = false;
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
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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
    render(commandBuffer, frameInfo, nullptr);
}

void GeometrySystem::render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo,
                            const GeometryViewOverride* viewOverride)
{
    DescriptorManager& descMgr = *frameInfo.descriptorManager;
    const bool reflection = viewOverride != nullptr && viewOverride->reflection;
    const VkDescriptorSet globalSet =
        (viewOverride != nullptr && viewOverride->globalSet != VK_NULL_HANDLE)
            ? viewOverride->globalSet
            : descMgr.getGlobalSet(frameInfo.frameInFlightIndex);

    // -----------------------------------------------------------------------
    // D-3 bindless：**整帧只绑一次 set 1**。
    //
    // 以前是"每个子网格切材质就 vkCmdBindDescriptorSets 一次"—— Sponza 有
    // 103 个子网格，于是 104 次 draw 伴随多达 104 次描述符集绑定。
    // 现在材质纹理在一个数组里，下标走 push constant，绑定与绘制彻底解耦。
    // -----------------------------------------------------------------------
    VkDescriptorSet sets[] = { globalSet, descMgr.getMaterialSet(), descMgr.getTerrainSet() };
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
    const glm::mat4 viewProjection = viewOverride != nullptr
        ? viewOverride->viewProjection
        : frameInfo.projectionMatrix * frameInfo.viewMatrix;
    const render::SubMeshCuller subMeshCuller(viewProjection);
    const glm::vec3 cameraPosition = viewOverride != nullptr
        ? viewOverride->cameraPosition
        : (frameInfo.cameraTransform
            ? glm::vec3(frameInfo.cameraTransform->getTranslation())
            : glm::vec3(0.0f));

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
    // F2-b: the arena batch owns indirect commands [0, 2048) in this buffer;
    // the CPU-side chunk path must not overwrite them.
    uint32_t indirectCursor =
        currentTerrainGpuRun.enabled ? kTerrainGpuMaxEntities * kTerrainGpuCommandsPerEntity : 0u;

    // 观测：逐 chunk push 时实际使用的高度归一化范围。与合批路径一样，
    // 流送地形的所有 chunk 必须共享全局 baseHeight/heightScale。
    float terrainShadingMinY = 0.0f;
    float terrainShadingHeightRange = 0.0f;
    bool terrainShadingCaptured = false;
    bool terrainShadingMixed = false;

    // GPU compute selection was recorded in a pre-graphics graph pass.
    const TerrainGpuRunContext& gpuRun = currentTerrainGpuRun;

    // V-04：先把本帧可见的 arena-backed 体素 chunk 收集成实例表 + 间接命令。
    // 主循环随后跳过这些实体，整批只用一条 vkCmdDrawIndexedIndirect 提交。
    // 反射相机复用同一份可见集合，但必须逐实体画（不能覆盖主视角的命令）。
    bool voxelBatchActive = false;
    uint32_t voxelBatchChunks = 0u;
    uint32_t voxelBatchCommands = 0u;
    uint64_t voxelBatchIndices = 0u;
    uint64_t voxelBatchVertices = 0u;
    if (!reflection)
    {
        if (ensureVoxelBatchResources())
        {
            auto* voxelInstances = static_cast<SceneGpuBatch::InstanceRecord*>(
                voxelChunkInstances[frameInfo.frameInFlightIndex].mapped);
            auto* voxelCommands = static_cast<VkDrawIndexedIndirectCommand*>(
                voxelIndirectBuffers[frameInfo.frameInFlightIndex].mapped);
            bool capacityExceeded = false;
            for (entt::entity entity : frameInfo.visibleEntities)
            {
                const MeshComponent* mesh = frameInfo.registry.try_get<MeshComponent>(entity);
                const WorldTransformComponent* world = frameInfo.registry.try_get<WorldTransformComponent>(entity);
                const VoxelChunkComponent* voxel = frameInfo.registry.try_get<VoxelChunkComponent>(entity);
                if (!mesh || !mesh->model || !world || !world->valid || !voxel ||
                    voxel->arenaIndexBase == TerrainMeshArena::kInvalidOffset ||
                    !terrainMeshArena || mesh->model->getIndexCount() == 0u)
                    continue;

                const std::vector<SubMesh>& subMeshes = mesh->model->getSubMeshes();
                uint32_t drawableSubMeshes = 0u;
                for (const SubMesh& subMesh : subMeshes)
                    drawableSubMeshes += (subMesh.indexCount > 0u) ? 1u : 0u;
                if (voxelBatchChunks >= kVoxelBatchMaxChunks ||
                    voxelBatchCommands + drawableSubMeshes > kVoxelBatchMaxCommands)
                {
                    capacityExceeded = true;
                    break;
                }

                const MaterialComponent* materialComp =
                    frameInfo.registry.try_get<MaterialComponent>(entity);
                uint32_t materialSlot = materialComp ? materialComp->materialSetIndex
                                                    : kInvalidMaterialSlot;
                const bool materialValid = materialSlot != kInvalidMaterialSlot;
                if (!materialValid)
                    materialSlot = fallbackMatId;

                float checkerScale = 1.0f;
                glm::vec3 localMin = mesh->model->getBoundsMin();
                glm::vec3 localMax = mesh->model->getBoundsMax();
                const BoundsComponent* boundsComp =
                    frameInfo.registry.try_get<BoundsComponent>(entity);
                if (boundsComp && boundsComp->getLocal().valid)
                {
                    const AABB& local = boundsComp->getLocal();
                    localMin = local.min;
                    localMax = local.max;
                    const glm::vec3 extent = local.max - local.min;
                    const float maxExtent = std::max({ extent.x, extent.y, extent.z });
                    if (maxExtent > 1e-6f)
                        checkerScale = 6.0f / maxExtent;
                }

                SceneGpuBatch::InstanceRecord& record = voxelInstances[voxelBatchChunks];
                record = SceneGpuBatch::InstanceRecord{};
                record.model = world->matrix;
                record.normal = world->normalMatrix;
                record.flags = glm::uvec4(materialSlot, materialValid ? 1u : 0u, 0u, 0u);
                record.misc = glm::vec4(0.0f, checkerScale, 0.0f, 0.0f);
                record.uvTransform = glm::vec4(0.0f);
                record.boundsMin = glm::vec4(localMin, 1.0f);
                record.boundsMax = glm::vec4(localMax, 0.0f);
                record.draw = glm::uvec4(0u);

                const uint32_t instanceIndex = voxelBatchChunks;
                for (const SubMesh& subMesh : subMeshes)
                {
                    if (subMesh.indexCount == 0u || subMesh.isTerrainChunk)
                        continue;
                    VkDrawIndexedIndirectCommand& command =
                        voxelCommands[voxelBatchCommands++];
                    command.indexCount = subMesh.indexCount;
                    command.instanceCount = 1u;
                    command.firstIndex = subMesh.firstIndex + voxel->arenaIndexBase;
                    command.vertexOffset = static_cast<int32_t>(voxel->arenaVertexBase);
                    command.firstInstance = instanceIndex;
                    voxelBatchIndices += subMesh.indexCount;
                }
                voxelBatchVertices += voxel->arenaVertexCount;
                ++voxelBatchChunks;
            }

            if (!capacityExceeded && voxelBatchChunks > 0u)
            {
                voxelBatchActive = true;
                vmaFlushAllocation(vulkanDevice.getVmaAllocator(),
                                   voxelChunkInstances[frameInfo.frameInFlightIndex].allocation,
                                   0, static_cast<VkDeviceSize>(voxelBatchChunks) *
                                          kVoxelBatchInstanceStride);
                vmaFlushAllocation(vulkanDevice.getVmaAllocator(),
                                   voxelIndirectBuffers[frameInfo.frameInFlightIndex].allocation,
                                   0, static_cast<VkDeviceSize>(voxelBatchCommands) *
                                          sizeof(VkDrawIndexedIndirectCommand));
                if (!voxelBatchLogged)
                {
                    voxelBatchLogged = true;
                    std::cout << "[Voxel] arena batch | chunks=" << voxelBatchChunks
                              << " commands=" << voxelBatchCommands
                              << " vertices=" << voxelBatchVertices
                              << " indices=" << voxelBatchIndices
                              << " stride=" << kVoxelBatchInstanceStride
                              << " mode=indirect" << std::endl;
                }
            }
            else if (capacityExceeded && !voxelBatchWarned)
            {
                voxelBatchWarned = true;
                std::cout << "[Voxel] arena batch capacity exceeded | chunks="
                          << voxelBatchChunks << " commands=" << voxelBatchCommands
                          << "; using per-chunk arena draws" << std::endl;
            }
        }
        else if (!voxelBatchWarned)
        {
            voxelBatchWarned = true;
            std::cout << "[Voxel] arena batch unavailable; using per-chunk arena draws" << std::endl;
        }
    }

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
        // 反射里不重建编辑器网格辅助线；它没有材质，只会多出一层线。
        if (reflection && isGrid)
            continue;
        const TerrainComponent* terrain = frameInfo.registry.try_get<TerrainComponent>(entity);
        const VoxelChunkComponent* voxelChunk =
            frameInfo.registry.try_get<VoxelChunkComponent>(entity);
        const bool voxelArenaBacked = voxelChunk != nullptr && terrainMeshArena != nullptr &&
            voxelChunk->arenaIndexBase != TerrainMeshArena::kInvalidOffset;
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
        if (!reflection && sceneGpuBatch != nullptr && sceneGpuBatch->useGpuPath(frameInfo) &&
            !isSkybox && !isGrid && !isTerrain && !voxelArenaBacked &&
            !mesh->model->isDynamic() &&
            mesh->model->getIndexCount() > 0)
        {
            continue;
        }

        // 体素专用间接批已经覆盖本实体时不再逐块提交。
        if (!reflection && voxelArenaBacked && voxelBatchActive)
            continue;

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
        // 反射用独立相机，绝不能把主视角的 LOD 状态改掉。这里只读选择结果，
        // 不写 terrainLodStates（否则主画面会跟着反射相机抖动/跳 LOD）。
        if (!reflection && isTerrain && terrain && mesh->model && !terrainGpuEntity)
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
        // 反射路径禁止重写间接命令：那块缓冲本帧已被主视角填过，
        // 再写会把主画面的地形间接绘制指到反射相机的可见集合上。
        const bool terrainUseIndirect = !reflection && isTerrain && !terrainGpuEntity &&
            !terrainBlending && vulkanDevice.supportsMultiDrawIndirect() &&
            visibleChunkCount > 0u &&
            indirectCursor + visibleChunkCount <= indirectBuffer.capacity;

        // C1c: blend the previous tick pose with the current one. Outside the
        // editor play mode renderAlpha is 0 and this is the authoritative matrix.
        const glm::mat4 entityMatrix = (frameInfo.renderAlpha > 0.0f)
            ? scene::interpolateTransform(worldTransform->previousMatrix,
                                          worldTransform->matrix, frameInfo.renderAlpha)
            : worldTransform->matrix;

        SimplePushConstantData push{};
        push.modelMatrix = entityMatrix;
        push.normalMatrix = (frameInfo.renderAlpha > 0.0f)
            ? glm::mat4(glm::transpose(glm::inverse(glm::mat3(entityMatrix))))
            : worldTransform->normalMatrix;
        push.clipY = viewOverride != nullptr ? viewOverride->clipY : 0.0f;
        push.clipMode = viewOverride != nullptr ? viewOverride->clipMode : 0u;

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
            if (terrain)
            {
                // Keep the height normalisation identical for every chunk of a
                // streaming terrain. Per-chunk local bounds shift the layer
                // weights at chunk borders (visible as rectangular colour seams).
                push.terrainMinY = terrain->config.baseHeight;
                push.terrainHeightRange = std::max(terrain->config.heightScale, 1e-4f);
                push.terrainDetailStrength = terrain->config.materialDetailStrength;
                push.terrainUvTiling = terrain->config.uvTiling;
                push.terrainMaterialStyle = static_cast<uint32_t>(terrain->config.surfaceStyle);
                if (!terrainShadingCaptured)
                {
                    terrainShadingCaptured = true;
                    terrainShadingMinY = push.terrainMinY;
                    terrainShadingHeightRange = push.terrainHeightRange;
                }
                else if (std::fabs(push.terrainMinY - terrainShadingMinY) > 1e-4f ||
                         std::fabs(push.terrainHeightRange - terrainShadingHeightRange) > 1e-4f)
                {
                    terrainShadingMixed = true;
                }
            }
            else if (bounds && bounds->getLocal().valid)
            {
                const AABB& localBounds = bounds->getLocal();
                push.terrainMinY = localBounds.min.y;
                push.terrainHeightRange = std::max(localBounds.max.y - localBounds.min.y, 1e-4f);
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
        // F2-f / V-04：地形与体素几何都在共享 arena 里（Model 只剩元数据），
        // 其余实体仍绑自己的缓冲。
        const bool terrainArenaBacked = isTerrain && terrain != nullptr &&
            terrainMeshArena != nullptr &&
            terrain->arenaIndexBase != TerrainMeshArena::kInvalidOffset;
        const bool arenaBacked = terrainArenaBacked || voxelArenaBacked;
        uint32_t arenaIndexBase = 0u;
        uint32_t arenaVertexBase = 0u;
        if (terrainArenaBacked)
        {
            arenaIndexBase = terrain->arenaIndexBase;
            arenaVertexBase = terrain->arenaVertexBase;
        }
        else if (voxelArenaBacked)
        {
            arenaIndexBase = voxelChunk->arenaIndexBase;
            arenaVertexBase = voxelChunk->arenaVertexBase;
        }
        if (arenaBacked)
        {
            VkBuffer arenaVertexBuffer = terrainMeshArena->getVertexBuffer();
            VkDeviceSize arenaVertexOffset = 0;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &arenaVertexBuffer, &arenaVertexOffset);
            vkCmdBindIndexBuffer(commandBuffer, terrainMeshArena->getIndexBuffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
        }
        else
        {
            mesh->model->bind(commandBuffer);
        }

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

            // F4：子网格几何存在它所属节点的局部空间，push 常量必须带上节点世界矩阵。
            // 没有节点信息（OBJ / 地形 / 体素 / 蒙皮）时维持实体矩阵，行为与以前一致。
            if (subMesh.node != kNoModelNode)
            {
                const glm::mat4 instanceMatrix =
                    entityMatrix * mesh->model->getNodeWorldMatrix(subMesh.node);
                push.modelMatrix = instanceMatrix;
                push.normalMatrix =
                    glm::mat4(glm::transpose(glm::inverse(glm::mat3(instanceMatrix))));
            }
            else
            {
                push.modelMatrix = entityMatrix;
                push.normalMatrix = (frameInfo.renderAlpha > 0.0f)
                    ? glm::mat4(glm::transpose(glm::inverse(glm::mat3(entityMatrix))))
                    : worldTransform->normalMatrix;
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

            if (arenaBacked)
            {
                // 几何在 arena，subMesh 的 firstIndex 是模型内偏移，要加上槽位基址。
                vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, 1,
                                 subMesh.firstIndex + arenaIndexBase,
                                 arenaVertexBase, 0);
            }
            else
            {
                mesh->model->drawSubMesh(commandBuffer, subMesh);
            }

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

        if (terrainGpuEntity && gpuRun.frame && !gpuRun.batchActive)
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

    if (gpuRun.batchActive && terrainMeshArena && terrainChunkInstancesReady)
    {
        SimplePushConstantData batchTerrainPush{};
        batchTerrainPush.modelMatrix = glm::mat4(1.0f);
        batchTerrainPush.normalMatrix = glm::mat4(1.0f);
        batchTerrainPush.terrainMode = 1u;
        batchTerrainPush.terrainBatched = 1u;
        batchTerrainPush.terrainLodBlend = 1.0f;
        batchTerrainPush.terrainLodBlendMode = 0u;
        batchTerrainPush.materialValid = 0u;
        batchTerrainPush.materialId = fallbackMatId;
        batchTerrainPush.clipY = viewOverride != nullptr ? viewOverride->clipY : 0.0f;
        batchTerrainPush.clipMode = viewOverride != nullptr ? viewOverride->clipMode : 0u;

        geometryPipeline->bind(commandBuffer);
        VkBuffer arenaVertices = terrainMeshArena->getVertexBuffer();
        VkBuffer arenaIndices = terrainMeshArena->getIndexBuffer();
        VkDeviceSize arenaVertexOffset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &arenaVertices, &arenaVertexOffset);
        vkCmdBindIndexBuffer(commandBuffer, arenaIndices, 0, VK_INDEX_TYPE_UINT32);
        VkDescriptorSet chunkSets[] =
            { terrainChunkInstanceSets[frameInfo.frameInFlightIndex] };
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 3, 1, chunkSets, 0, nullptr);
        frameInfo.telemetry.descriptorBinds += 1u;
        vkCmdPushConstants(commandBuffer, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SimplePushConstantData), &batchTerrainPush);
        vkCmdDrawIndexedIndirect(
            commandBuffer, indirectBuffer.buffer, 0,
            kTerrainGpuMaxEntities * kTerrainGpuCommandsPerEntity,
            sizeof(VkDrawIndexedIndirectCommand));

        frameInfo.telemetry.drawCalls += 1u;
        frameInfo.telemetry.terrainIndirectGroups += 1u;
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
    // 反射不复用主视角 compute 生成的间接命令：那批命令只包含主相机可见集合，
    // 反射相机看到的对岸/山体可能在其外。普通网格在上面已走 CPU 路径。
    if (!reflection && sceneGpuBatch != nullptr && sceneGpuBatch->useGpuPath(frameInfo))
    {
        SimplePushConstantData batchPush{};
        batchPush.gpuScene = 1u;
        vkCmdPushConstants(commandBuffer, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SimplePushConstantData), &batchPush);
        geometryPipeline->bind(commandBuffer);
        // 地形合批会把 set 3 换成 chunk 实例表，这里必须换回场景实例表，
        // 否则实例矩阵取到地形行，网格会被画到错误位置。
        VkDescriptorSet sceneSets[] =
            { sceneGpuBatch->getInstanceSet(frameInfo.frameInFlightIndex) };
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 3, 1, sceneSets, 0, nullptr);
        frameInfo.telemetry.descriptorBinds += 1u;
        sceneGpuBatch->recordDraw(commandBuffer, frameInfo);
    }

    // V-04：体素 chunk 合批。实例矩阵/材质在 set 3 的专用实例表里，
    // 命令已在上面按 arena 切片写好；这里只需一次绑定 + 一条 indirect。
    if (voxelBatchActive && terrainMeshArena != nullptr && voxelChunkInstancesReady)
    {
        SimplePushConstantData batchVoxelPush{};
        batchVoxelPush.modelMatrix = glm::mat4(1.0f);
        batchVoxelPush.normalMatrix = glm::mat4(1.0f);
        batchVoxelPush.materialValid = 0u;
        batchVoxelPush.materialId = fallbackMatId;
        batchVoxelPush.gpuScene = 1u;
        batchVoxelPush.clipY = viewOverride != nullptr ? viewOverride->clipY : 0.0f;
        batchVoxelPush.clipMode = viewOverride != nullptr ? viewOverride->clipMode : 0u;

        geometryPipeline->bind(commandBuffer);
        VkBuffer arenaVertices = terrainMeshArena->getVertexBuffer();
        VkBuffer arenaIndices = terrainMeshArena->getIndexBuffer();
        VkDeviceSize arenaVertexOffset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &arenaVertices, &arenaVertexOffset);
        vkCmdBindIndexBuffer(commandBuffer, arenaIndices, 0, VK_INDEX_TYPE_UINT32);
        VkDescriptorSet voxelSets[] =
            { voxelChunkInstanceSets[frameInfo.frameInFlightIndex] };
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 3, 1, voxelSets, 0, nullptr);
        frameInfo.telemetry.descriptorBinds += 1u;
        vkCmdPushConstants(commandBuffer, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SimplePushConstantData), &batchVoxelPush);
        vkCmdDrawIndexedIndirect(
            commandBuffer, voxelIndirectBuffers[frameInfo.frameInFlightIndex].buffer, 0,
            voxelBatchCommands, sizeof(VkDrawIndexedIndirectCommand));

        frameInfo.telemetry.drawCalls += 1u;
        frameInfo.telemetry.triangles += static_cast<uint32_t>(voxelBatchIndices / 3u);
        frameInfo.telemetry.vertices += static_cast<uint32_t>(voxelBatchIndices);
    }

    if (terrainShadingCaptured)
    {
        static bool loggedTerrainChunkShading = false;
        if (!loggedTerrainChunkShading)
        {
            loggedTerrainChunkShading = true;
            std::cout << "[Terrain] chunk shading | heightCaptured=1"
                      << " heightMin=" << terrainShadingMinY
                      << " heightRange=" << terrainShadingHeightRange
                      << " heightMixed=" << (terrainShadingMixed ? 1 : 0)
                      << std::endl;
        }
    }

}