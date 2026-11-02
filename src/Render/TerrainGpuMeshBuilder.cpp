#include "Render/TerrainGpuMeshBuilder.h"

#include "Terrain/TerrainMeshBuilder.h"
#include "AssetPaths.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
constexpr uint32_t kMaxLods = 3u;
constexpr uint32_t kVertexFloats = 11u;

uint32_t innerSegments(uint32_t span, uint32_t step)
{
    if (span == 0u)
        return 1u;
    return std::clamp((span + step / 2u) / step, 1u, span);
}

// B-05: the full-resolution picking mirror is CPU data and can be large
// (257x257 + ~393k indices). Build it on a worker so submit/poll never pay
// that cost on the render thread.
void buildPickingMirror(const terrain::TerrainConfig& config,
                        const terrain::HeightField& heightField,
                        TerrainGpuMeshBuildResult& outResult)
{
    const float stepX = config.sizeX / static_cast<float>(config.resolutionX - 1u);
    const float stepZ = config.sizeZ / static_cast<float>(config.resolutionZ - 1u);
    const size_t positionCount =
        static_cast<size_t>(config.resolutionX) * config.resolutionZ;
    outResult.pickingPositions.resize(positionCount);
    for (uint32_t z = 0; z < config.resolutionZ; ++z)
    {
        const float worldZ = -config.sizeZ * 0.5f + static_cast<float>(z) * stepZ;
        const size_t row = static_cast<size_t>(z) * config.resolutionX;
        for (uint32_t x = 0; x < config.resolutionX; ++x)
        {
            const float worldX = -config.sizeX * 0.5f + static_cast<float>(x) * stepX;
            const float h = heightField.heights[row + x];
            outResult.pickingPositions[row + x] =
                glm::vec3(worldX, config.baseHeight + config.heightScale * h, worldZ);
        }
    }

    const size_t cellCount =
        static_cast<size_t>(config.resolutionX - 1u) *
        static_cast<size_t>(config.resolutionZ - 1u);
    outResult.pickingIndices.resize(cellCount * 6u);
    uint32_t* indices = outResult.pickingIndices.data();
    const uint32_t rowStride = config.resolutionX;
    for (uint32_t z = 0; z + 1u < config.resolutionZ; ++z)
    {
        for (uint32_t x = 0; x + 1u < config.resolutionX; ++x)
        {
            const uint32_t a = z * rowStride + x;
            const uint32_t b = (z + 1u) * rowStride + x;
            const uint32_t c = z * rowStride + x + 1u;
            const uint32_t d = (z + 1u) * rowStride + x + 1u;
            *indices++ = a;
            *indices++ = b;
            *indices++ = c;
            *indices++ = b;
            *indices++ = d;
            *indices++ = c;
        }
    }
}
} // namespace

TerrainGpuMeshBuilder::TerrainGpuMeshBuilder(Device& deviceArg,
                                             DescriptorManager& descriptorManagerArg)
    : device(deviceArg), descriptorManager(descriptorManagerArg)
{
    try
    {
        createResources();
        available = true;
        std::cout << "[Terrain] GPU mesh assembly ready | maxLods=" << kMaxLods
                  << " maxResolution=" << kMaxResolution
                  << " mode=singleChunk async slots=4\n";
    }
    catch (...)
    {
        destroyResources();
        throw;
    }
}

TerrainGpuMeshBuilder::~TerrainGpuMeshBuilder()
{
    waitAll();
    destroyResources();
}

bool TerrainGpuMeshBuilder::supports(const terrain::TerrainConfig& config) const
{
    return available && config.chunkGridX == 1u && config.chunkGridZ == 1u &&
           config.resolutionX >= terrain::TerrainConfig::kMinResolution &&
           config.resolutionZ >= terrain::TerrainConfig::kMinResolution;
}


void TerrainGpuMeshBuilder::createResources()
{
    const auto families = device.findPhysicalQueueFamilies();
    if (!families.graphicsFamily.has_value())
        throw std::runtime_error("graphics queue family unavailable");

    // All assembly submits go to the graphics queue: streamed chunk models are
    // consumed by the same queue family, so no cross-queue ownership transfer
    // is needed. Submission never waits for a fence; Scene polls completion.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = families.graphicsFamily.value();
    if (vkCreateCommandPool(device.getDevice(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
        throw std::runtime_error("terrain mesh command pool creation failed");

    for (uint32_t i = 0; i < kAsyncSlots; ++i)
    {
        Slot& slot = slots[i];
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device.getDevice(), &allocInfo,
                                     &slot.commandBuffer) != VK_SUCCESS)
            throw std::runtime_error("terrain mesh command buffer allocation failed");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device.getDevice(), &fenceInfo, nullptr,
                          &slot.fence) != VK_SUCCESS)
            throw std::runtime_error("terrain mesh fence creation failed");
    }

    VkCommandBufferAllocateInfo readbackAlloc{};
    readbackAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    readbackAlloc.commandPool = commandPool;
    readbackAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    readbackAlloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device.getDevice(), &readbackAlloc,
                                 &readbackCommandBuffer) != VK_SUCCESS)
        throw std::runtime_error("terrain mesh readback command allocation failed");

    VkFenceCreateInfo readbackFenceInfo{};
    readbackFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device.getDevice(), &readbackFenceInfo, nullptr,
                      &readbackFence) != VK_SUCCESS)
        throw std::runtime_error("terrain mesh readback fence creation failed");

    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4u; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    setLayout = descriptorManager.createSetLayout(
        { bindings[0], bindings[1], bindings[2], bindings[3] });

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &pipelineLayoutInfo, nullptr,
                               &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("terrain mesh pipeline layout creation failed");

    pipeline = std::make_unique<ComputePipeline>(
        device, SENTINEL_SHADER_DIR "terrain_mesh.comp.spv", pipelineLayout);

    const VkDeviceSize heightBytes =
        static_cast<VkDeviceSize>(kMaxResolution) * kMaxResolution * sizeof(float);
    for (uint32_t i = 0; i < kAsyncSlots; ++i)
    {
        Slot& slot = slots[i];
        slot.descriptorSet = descriptorManager.allocateSet(setLayout);
        if (!createPersistentBuffer(
                heightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                true, slot.heightBuffer, slot.heightAllocation, &slot.heightMapped))
        {
            throw std::runtime_error("terrain mesh height buffer creation failed");
        }
        if (!createPersistentBuffer(
                sizeof(GpuMeshParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                true, slot.paramsBuffer, slot.paramsAllocation,
                reinterpret_cast<void**>(&slot.paramsMapped)))
        {
            throw std::runtime_error("terrain mesh params buffer creation failed");
        }
    }
}

void TerrainGpuMeshBuilder::destroyResources()
{
    pipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    pipelineLayout = VK_NULL_HANDLE;
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    setLayout = VK_NULL_HANDLE;

    for (Slot& slot : slots)
    {
        destroySlotResult(slot);
        destroyBuffer(slot.heightBuffer, slot.heightAllocation);
        slot.heightMapped = nullptr;
        destroyBuffer(slot.paramsBuffer, slot.paramsAllocation);
        slot.paramsMapped = nullptr;
        if (slot.fence != VK_NULL_HANDLE)
            vkDestroyFence(device.getDevice(), slot.fence, nullptr);
        slot.fence = VK_NULL_HANDLE;
        slot.commandBuffer = VK_NULL_HANDLE;
        slot.descriptorSet = VK_NULL_HANDLE;
    }

    if (readbackFence != VK_NULL_HANDLE)
        vkDestroyFence(device.getDevice(), readbackFence, nullptr);
    readbackFence = VK_NULL_HANDLE;
    readbackCommandBuffer = VK_NULL_HANDLE;

    if (commandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device.getDevice(), commandPool, nullptr);
    commandPool = VK_NULL_HANDLE;
}

bool TerrainGpuMeshBuilder::createPersistentBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
    VkBuffer& buffer, VmaAllocation& allocation, void** mapped)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
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
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocationInfo allocationInfo{};
    if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                        &buffer, &allocation, &allocationInfo) != VK_SUCCESS)
        return false;
    if (mapped)
    {
        *mapped = allocationInfo.pMappedData;
        if (!*mapped)
        {
            destroyBuffer(buffer, allocation);
            return false;
        }
    }
    return true;
}

bool TerrainGpuMeshBuilder::createOutputBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkBuffer& buffer, VmaAllocation& allocation)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                           &buffer, &allocation, nullptr) == VK_SUCCESS;
}

void TerrainGpuMeshBuilder::destroyBuffer(VkBuffer& buffer, VmaAllocation& allocation)
{
    if (buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(device.getVmaAllocator(), buffer, allocation);
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

TerrainGpuMeshBuilder::Layout TerrainGpuMeshBuilder::computeLayout(
    const terrain::TerrainConfig& config) const
{
    Layout layout;
    layout.chunkCellsX = config.resolutionX - 1u;
    layout.chunkCellsZ = config.resolutionZ - 1u;
    layout.skirtDepth = std::max(0.25f, config.heightScale * 0.08f);
    const uint32_t lodCount = std::clamp(config.lodLevelCount, 1u, kMaxLods);

    uint32_t vertexOffset = 0u;
    uint32_t indexOffset = 0u;
    for (uint32_t lod = 0; lod < lodCount; ++lod)
    {
        LodLayout& out = layout.lods[lod];
        out.step = 1u << lod;
        out.canStitch = layout.chunkCellsX > 2u * out.step &&
                        layout.chunkCellsZ > 2u * out.step;
        out.outerRingCount = 2u * (layout.chunkCellsX + layout.chunkCellsZ);

        if (out.canStitch)
        {
            out.innerSegX = innerSegments(
                layout.chunkCellsX - 2u * out.step, out.step);
            out.innerSegZ = innerSegments(
                layout.chunkCellsZ - 2u * out.step, out.step);
            out.innerVertexCount =
                (out.innerSegX + 1u) * (out.innerSegZ + 1u);
            out.surfaceVertexCount = out.innerVertexCount + out.outerRingCount;
            const uint32_t quads = out.innerSegX * out.innerSegZ * 6u;
            const uint32_t transition =
                2u * (layout.chunkCellsX + layout.chunkCellsZ +
                      out.innerSegX + out.innerSegZ) * 3u;
            out.surfaceIndexCount = quads + transition;
        }
        else
        {
            out.innerSegX = 0u;
            out.innerSegZ = 0u;
            out.innerVertexCount = 0u;
            out.surfaceVertexCount =
                (layout.chunkCellsX + 1u) * (layout.chunkCellsZ + 1u);
            out.surfaceIndexCount =
                layout.chunkCellsX * layout.chunkCellsZ * 6u;
        }

        out.vertexCount = out.surfaceVertexCount +
            (config.skirtsEnabled ? out.outerRingCount : 0u);
        out.indexCount = out.surfaceIndexCount +
            (config.skirtsEnabled ? out.outerRingCount * 6u : 0u);
        out.vertexOffset = vertexOffset;
        out.indexOffset = indexOffset;
        vertexOffset += out.vertexCount;
        indexOffset += out.indexCount;
    }

    layout.vertexCount = vertexOffset;
    layout.indexCount = indexOffset;
    return layout;
}

uint32_t TerrainGpuMeshBuilder::freeSlotCount()
{
    collectFinished();
    uint32_t freeCount = 0u;
    for (const Slot& slot : slots)
    {
        if (!slot.active)
            ++freeCount;
    }
    return freeCount;
}

uint64_t TerrainGpuMeshBuilder::submit(const terrain::TerrainConfig& config,
                                       const terrain::HeightField& heightField,
                                       std::string& errorMessage)
{
    errorMessage.clear();
    collectFinished();
    if (!supports(config))
    {
        errorMessage = "GPU mesh assembly requires chunkGridX=Z=1";
        ++stats.rejected;
        return 0u;
    }
    if (heightField.width != config.resolutionX ||
        heightField.height != config.resolutionZ ||
        heightField.heights.size() !=
            static_cast<size_t>(config.resolutionX) * config.resolutionZ)
    {
        errorMessage = "GPU mesh assembly height-field shape mismatch";
        ++stats.rejected;
        return 0u;
    }

    Slot* freeSlot = nullptr;
    for (Slot& slot : slots)
    {
        if (!slot.active)
        {
            freeSlot = &slot;
            break;
        }
    }
    if (!freeSlot)
    {
        errorMessage = "GPU mesh assembly slots busy";
        ++stats.rejected;
        return 0u;
    }

    Slot& slot = *freeSlot;
    slot.active = true;
    slot.abandoned = false;
    slot.submitted = false;
    slot.requestId = nextRequestId++;
    slot.startTime = std::chrono::steady_clock::now();

    const auto submitStart = slot.startTime;
    if (!prepareBuild(config, heightField, slot.result, slot.layout, errorMessage))
    {
        destroySlotResult(slot);
        clearSlot(slot);
        ++stats.rejected;
        return 0u;
    }

    // Overlap the CPU picking mirror with GPU submission/assembly. The worker
    // starts only after prepareBuild() has finished writing the result fields
    // it owns, so the two never touch the same vector.
    slot.pickingFuture = std::async(
        std::launch::async,
        [config, heightField, &slotResult = slot.result]()
        {
            buildPickingMirror(config, heightField, slotResult);
        });
    if (!recordAndSubmit(slot, config, heightField, errorMessage))
    {
        destroySlotResult(slot);
        clearSlot(slot);
        ++stats.rejected;
        return 0u;
    }

    slot.submitted = true;
    ++stats.submitted;
    const double submitMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - submitStart).count();
    stats.maxSubmitMs = std::max(stats.maxSubmitMs, submitMs);
    return slot.requestId;
}

TerrainGpuMeshBuilder::PollStatus TerrainGpuMeshBuilder::poll(
    uint64_t requestId,
    TerrainGpuMeshBuildResult& outResult,
    std::string& errorMessage)
{
    errorMessage.clear();
    const auto pollStart = std::chrono::steady_clock::now();
    if (requestId == 0u)
    {
        errorMessage = "GPU mesh poll with invalid request id";
        return PollStatus::Failed;
    }

    Slot* slot = nullptr;
    for (Slot& candidate : slots)
    {
        if (candidate.active && candidate.requestId == requestId)
        {
            slot = &candidate;
            break;
        }
    }
    if (!slot)
    {
        errorMessage = "GPU mesh request not found";
        return PollStatus::Failed;
    }

    const VkResult fenceStatus =
        vkGetFenceStatus(device.getDevice(), slot->fence);
    if (fenceStatus == VK_NOT_READY)
    {
        stats.maxPollMs = std::max(
            stats.maxPollMs,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pollStart).count());
        return PollStatus::Pending;
    }
    if (fenceStatus != VK_SUCCESS)
    {
        errorMessage = "GPU mesh fence status failed";
        destroySlotResult(*slot);
        clearSlot(*slot);
        return PollStatus::Failed;
    }

    // The GPU mesh is complete, but the off-thread CPU picking mirror may
    // still be running. Do not wait on the render thread; retry next poll.
    if (slot->pickingFuture.valid() &&
        slot->pickingFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        stats.maxPollMs = std::max(
            stats.maxPollMs,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pollStart).count());
        return PollStatus::Pending;
    }
    if (slot->pickingFuture.valid())
    {
        try
        {
            slot->pickingFuture.get();
        }
        catch (...)
        {
            errorMessage = "GPU mesh picking mirror failed";
            destroySlotResult(*slot);
            clearSlot(*slot);
            return PollStatus::Failed;
        }
    }

    const double assemblyMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - slot->startTime).count();
    slot->result.assemblyMs = assemblyMs;
    outResult = std::move(slot->result);
    clearSlot(*slot);

    ++stats.completed;
    stats.maxAssemblyMs = std::max(stats.maxAssemblyMs, assemblyMs);
    stats.maxPollMs = std::max(
        stats.maxPollMs,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pollStart).count());
    return PollStatus::Completed;
}

void TerrainGpuMeshBuilder::release(uint64_t requestId)
{
    if (requestId == 0u)
        return;
    for (Slot& slot : slots)
    {
        if (slot.active && slot.requestId == requestId)
        {
            slot.abandoned = true;
            break;
        }
    }
    collectFinished();
}

void TerrainGpuMeshBuilder::collectFinished()
{
    for (Slot& slot : slots)
    {
        if (!slot.active || !slot.abandoned)
            continue;
        if (slot.pickingFuture.valid() &&
            slot.pickingFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            continue;
        }
        if (!slot.submitted)
        {
            destroySlotResult(slot);
            clearSlot(slot);
            continue;
        }
        const VkResult fenceStatus =
            vkGetFenceStatus(device.getDevice(), slot.fence);
        if (fenceStatus == VK_SUCCESS || fenceStatus < VK_SUCCESS)
        {
            destroySlotResult(slot);
            clearSlot(slot);
        }
    }
}

void TerrainGpuMeshBuilder::waitAll()
{
    for (Slot& slot : slots)
    {
        if (!slot.active)
            continue;
        if (slot.submitted)
        {
            vkWaitForFences(device.getDevice(), 1, &slot.fence, VK_TRUE,
                            std::numeric_limits<uint64_t>::max());
        }
        if (slot.pickingFuture.valid())
            slot.pickingFuture.wait();
        destroySlotResult(slot);
        clearSlot(slot);
    }
}

bool TerrainGpuMeshBuilder::build(const terrain::TerrainConfig& config,
                                  const terrain::HeightField& heightField,
                                  TerrainGpuMeshBuildResult& outResult,
                                  std::string& errorMessage)
{
    outResult = {};
    const uint64_t requestId = submit(config, heightField, errorMessage);
    if (requestId == 0u)
        return false;

    for (;;)
    {
        const PollStatus status = poll(requestId, outResult, errorMessage);
        if (status == PollStatus::Completed)
            return true;
        if (status == PollStatus::Failed)
            return false;
        std::this_thread::yield();
    }
}

bool TerrainGpuMeshBuilder::prepareBuild(
    const terrain::TerrainConfig& config,
    const terrain::HeightField& heightField,
    TerrainGpuMeshBuildResult& outResult,
    Layout& outLayout,
    std::string& errorMessage)
{
    errorMessage.clear();
    const Layout layout = computeLayout(config);
    if (layout.vertexCount == 0u || layout.indexCount == 0u)
    {
        errorMessage = "GPU mesh assembly produced an empty layout";
        return false;
    }

    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(layout.vertexCount) * kVertexFloats * sizeof(float);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(layout.indexCount) * sizeof(uint32_t);
    if (!createOutputBuffer(vertexBytes,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            outResult.vertexBuffer, outResult.vertexAllocation) ||
        !createOutputBuffer(indexBytes,
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                            outResult.indexBuffer, outResult.indexAllocation))
    {
        errorMessage = "GPU mesh output buffer creation failed";
        destroyBuildResult(outResult);
        return false;
    }
    outResult.vertexCount = layout.vertexCount;
    outResult.indexCount = layout.indexCount;

    const float worldMinY = config.baseHeight + config.heightScale * heightField.minValue;
    const float worldMaxY = config.baseHeight + config.heightScale * heightField.maxValue;
    const float skirtDepth = config.skirtsEnabled ? layout.skirtDepth : 0.0f;
    outResult.boundsMin = glm::vec3(-config.sizeX * 0.5f,
                                    worldMinY - skirtDepth,
                                    -config.sizeZ * 0.5f);
    outResult.boundsMax = glm::vec3(config.sizeX * 0.5f, worldMaxY,
                                    config.sizeZ * 0.5f);
    outResult.uvMin = glm::vec2(0.0f);
    outResult.uvMax = glm::vec2(config.uvTiling);
    outResult.uvValid = config.uvTiling > 1e-5f;

    const uint32_t lodCount = std::clamp(config.lodLevelCount, 1u, kMaxLods);
    outResult.subMeshes.reserve(lodCount);
    for (uint32_t lod = 0; lod < lodCount; ++lod)
    {
        const LodLayout& lodLayout = layout.lods[lod];
        SubMesh subMesh;
        subMesh.firstIndex = lodLayout.indexOffset;
        subMesh.indexCount = lodLayout.indexCount;
        subMesh.hasBounds = true;
        subMesh.boundsMin = outResult.boundsMin;
        subMesh.boundsMax = outResult.boundsMax;
        subMesh.isTerrainChunk = true;
        subMesh.terrainChunkX = 0u;
        subMesh.terrainChunkZ = 0u;
        subMesh.terrainLod = lod;
        subMesh.terrainSurfaceIndexCount = lodLayout.surfaceIndexCount;
        outResult.subMeshes.push_back(subMesh);
    }

    outLayout = layout;
    return true;
}

bool TerrainGpuMeshBuilder::updateDescriptors(Slot& slot,
                                             std::string& errorMessage)
{
    VkDescriptorBufferInfo infos[4]{};
    infos[0] = { slot.heightBuffer, 0, VK_WHOLE_SIZE };
    infos[1] = { slot.result.vertexBuffer, 0, VK_WHOLE_SIZE };
    infos[2] = { slot.result.indexBuffer, 0, VK_WHOLE_SIZE };
    infos[3] = { slot.paramsBuffer, 0, VK_WHOLE_SIZE };

    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = slot.descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device.getDevice(), 4, writes, 0, nullptr);
    errorMessage.clear();
    return true;
}

bool TerrainGpuMeshBuilder::recordAndSubmit(
    Slot& slot,
    const terrain::TerrainConfig& config,
    const terrain::HeightField& heightField,
    std::string& errorMessage)
{
    const size_t heightCount =
        static_cast<size_t>(config.resolutionX) * config.resolutionZ;
    std::memcpy(slot.heightMapped, heightField.heights.data(),
                heightCount * sizeof(float));
    if (vmaFlushAllocation(device.getVmaAllocator(), slot.heightAllocation,
                           0, VK_WHOLE_SIZE) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh height buffer flush failed";
        return false;
    }

    GpuMeshParams params;
    params.sizes = glm::uvec4(config.resolutionX, config.resolutionZ,
                              std::clamp(config.lodLevelCount, 1u, kMaxLods), 0u);
    params.world = glm::vec4(config.sizeX, config.sizeZ,
                             config.baseHeight, config.heightScale);
    params.colorParams = glm::vec4(heightField.minValue, heightField.maxValue,
                                   slot.layout.skirtDepth,
                                   config.uvTiling);
    params.flags = glm::uvec4(config.skirtsEnabled ? 1u : 0u,
                              config.localColorRange ? 1u : 0u,
                              slot.layout.chunkCellsX, slot.layout.chunkCellsZ);
    for (uint32_t lod = 0; lod < kMaxLods; ++lod)
    {
        params.lodVertexOffset[lod] = slot.layout.lods[lod].vertexOffset;
        params.lodVertexCount[lod] = slot.layout.lods[lod].vertexCount;
        params.lodIndexOffset[lod] = slot.layout.lods[lod].indexOffset;
        params.lodIndexCount[lod] = slot.layout.lods[lod].indexCount;
        params.lodSurfaceIndexCount[lod] = slot.layout.lods[lod].surfaceIndexCount;
    }
    *slot.paramsMapped = params;
    if (vmaFlushAllocation(device.getVmaAllocator(), slot.paramsAllocation,
                           0, VK_WHOLE_SIZE) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh params buffer flush failed";
        return false;
    }

    if (!updateDescriptors(slot, errorMessage))
        return false;

    if (vkResetFences(device.getDevice(), 1, &slot.fence) != VK_SUCCESS ||
        vkResetCommandBuffer(slot.commandBuffer, 0) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh command state reset failed";
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh command buffer begin failed";
        return false;
    }

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(slot.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &hostBarrier, 0, nullptr, 0, nullptr);

    pipeline->bind(slot.commandBuffer);
    vkCmdBindDescriptorSets(slot.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &slot.descriptorSet, 0, nullptr);

    PushConstants push{};
    push.mode = 0u;
    push.itemCount = slot.layout.vertexCount;
    vkCmdPushConstants(slot.commandBuffer, pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(slot.commandBuffer,
                  (slot.layout.vertexCount + 63u) / 64u, 1, 1);

    push.mode = 1u;
    push.itemCount = slot.layout.indexCount;
    vkCmdPushConstants(slot.commandBuffer, pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(slot.commandBuffer,
                  (slot.layout.indexCount + 63u) / 64u, 1, 1);

    VkMemoryBarrier meshBarrier{};
    meshBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    meshBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    meshBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                VK_ACCESS_INDEX_READ_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(slot.commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &meshBarrier, 0, nullptr, 0, nullptr);

    if (vkEndCommandBuffer(slot.commandBuffer) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh command buffer end failed";
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commandBuffer;
    if (vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo,
                      slot.fence) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh compute submission failed";
        return false;
    }
    return true;
}

void TerrainGpuMeshBuilder::destroyBuildResult(TerrainGpuMeshBuildResult& result)
{
    destroyBuffer(result.vertexBuffer, result.vertexAllocation);
    destroyBuffer(result.indexBuffer, result.indexAllocation);
    result.vertexCount = 0;
    result.indexCount = 0;
    result.subMeshes.clear();
    result.pickingPositions.clear();
    result.pickingIndices.clear();
}

void TerrainGpuMeshBuilder::clearSlot(Slot& slot)
{
    slot.requestId = 0u;
    slot.active = false;
    slot.abandoned = false;
    slot.submitted = false;
    slot.pickingFuture = {};
    slot.layout = {};
}

void TerrainGpuMeshBuilder::destroySlotResult(Slot& slot)
{
    if (slot.pickingFuture.valid())
        slot.pickingFuture.wait();
    destroyBuildResult(slot.result);
    slot.result = {};
}

bool TerrainGpuMeshBuilder::readbackBuffers(
    const TerrainGpuMeshBuildResult& result,
    std::vector<float>& outVertexFloats,
    std::vector<uint32_t>& outIndices,
    std::string& errorMessage)
{
    outVertexFloats.clear();
    outIndices.clear();
    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(result.vertexCount) * kVertexFloats * sizeof(float);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(result.indexCount) * sizeof(uint32_t);
    const VkDeviceSize totalBytes = vertexBytes + indexBytes;

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    if (!createPersistentBuffer(totalBytes,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                true, staging, stagingAllocation, &stagingMapped))
    {
        errorMessage = "terrain mesh readback staging creation failed";
        return false;
    }

    if (vkResetFences(device.getDevice(), 1, &readbackFence) != VK_SUCCESS ||
        vkResetCommandBuffer(readbackCommandBuffer, 0) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh readback command reset failed";
        destroyBuffer(staging, stagingAllocation);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(readbackCommandBuffer, &beginInfo) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh readback begin failed";
        destroyBuffer(staging, stagingAllocation);
        return false;
    }

    VkBufferCopy vertexCopy{};
    vertexCopy.srcOffset = 0;
    vertexCopy.dstOffset = 0;
    vertexCopy.size = vertexBytes;
    vkCmdCopyBuffer(readbackCommandBuffer, result.vertexBuffer, staging, 1, &vertexCopy);
    VkBufferCopy indexCopy{};
    indexCopy.srcOffset = 0;
    indexCopy.dstOffset = vertexBytes;
    indexCopy.size = indexBytes;
    vkCmdCopyBuffer(readbackCommandBuffer, result.indexBuffer, staging, 1, &indexCopy);

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(readbackCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &hostBarrier, 0, nullptr, 0, nullptr);

    if (vkEndCommandBuffer(readbackCommandBuffer) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh readback end failed";
        destroyBuffer(staging, stagingAllocation);
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &readbackCommandBuffer;
    if (vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, readbackFence) != VK_SUCCESS ||
        vkWaitForFences(device.getDevice(), 1, &readbackFence, VK_TRUE,
                        std::numeric_limits<uint64_t>::max()) != VK_SUCCESS)
    {
        errorMessage = "terrain mesh readback submission failed";
        destroyBuffer(staging, stagingAllocation);
        return false;
    }

    vmaInvalidateAllocation(device.getVmaAllocator(), stagingAllocation,
                            0, VK_WHOLE_SIZE);
    outVertexFloats.resize(static_cast<size_t>(result.vertexCount) * kVertexFloats);
    outIndices.resize(result.indexCount);
    std::memcpy(outVertexFloats.data(), stagingMapped,
                static_cast<size_t>(vertexBytes));
    std::memcpy(outIndices.data(),
                static_cast<const uint8_t*>(stagingMapped) + vertexBytes,
                static_cast<size_t>(indexBytes));
    destroyBuffer(staging, stagingAllocation);
    return true;
}

bool TerrainGpuMeshBuilder::selfTest(std::string& errorMessage)
{
    errorMessage.clear();
    if (!available)
    {
        errorMessage = "GPU mesh assembly is unavailable";
        return false;
    }

    terrain::TerrainConfig config;
    config.resolutionX = 65u;
    config.resolutionZ = 65u;
    config.sizeX = 40.0f;
    config.sizeZ = 60.0f;
    config.chunkGridX = 1u;
    config.chunkGridZ = 1u;
    config.lodLevelCount = 3u;
    // Exercise the conservative default skirt path plus local-height vertex
    // colors. Streamed chunks cover the no-skirt / absolute-color variant in
    // the editor and engine streaming smoke tests.
    config.skirtsEnabled = true;
    config.localColorRange = true;
    config.uvTiling = 1.25f;

    terrain::HeightField heightField;
    if (!heightField.build(config, errorMessage))
        return false;

    terrain::TerrainMeshData cpuMesh;
    if (!terrain::TerrainMeshBuilder::build(config, heightField, cpuMesh, errorMessage))
        return false;

    TerrainGpuMeshBuildResult gpuMesh;
    if (!build(config, heightField, gpuMesh, errorMessage))
        return false;

    std::vector<float> gpuVertices;
    std::vector<uint32_t> gpuIndices;
    if (!readbackBuffers(gpuMesh, gpuVertices, gpuIndices, errorMessage))
    {
        destroyBuildResult(gpuMesh);
        return false;
    }

    bool ok = true;
    if (gpuMesh.vertexCount != cpuMesh.vertices.size() ||
        gpuMesh.indexCount != cpuMesh.indices.size() ||
        gpuMesh.subMeshes.size() != cpuMesh.subMeshes.size())
    {
        errorMessage = "GPU mesh count/submesh mismatch";
        ok = false;
    }

    float maxPositionError = 0.0f;
    float maxAttributeError = 0.0f;
    if (ok)
    {
        for (size_t i = 0; i < cpuMesh.vertices.size(); ++i)
        {
            const terrain::TerrainVertex& expected = cpuMesh.vertices[i];
            const float* actual = gpuVertices.data() + i * kVertexFloats;
            const glm::vec3 actualPosition(actual[0], actual[1], actual[2]);
            const glm::vec3 actualColor(actual[3], actual[4], actual[5]);
            const glm::vec3 actualNormal(actual[6], actual[7], actual[8]);
            const glm::vec2 actualUv(actual[9], actual[10]);
            const float positionError = glm::length(actualPosition - expected.position);
            const float colorError = glm::length(actualColor - expected.color);
            const float normalError = glm::length(actualNormal - expected.normal);
            const float uvError = glm::length(actualUv - expected.uv);
            maxPositionError = std::max(maxPositionError, positionError);
            maxAttributeError = std::max({ maxAttributeError, colorError,
                                           normalError, uvError });
            if (positionError > 2e-5f || colorError > 2e-5f ||
                normalError > 2e-5f || uvError > 2e-5f)
            {
                errorMessage = "GPU mesh vertex mismatch at index " +
                    std::to_string(i);
                ok = false;
                break;
            }
        }
    }

    if (ok && gpuIndices != cpuMesh.indices)
    {
        size_t mismatch = 0u;
        while (mismatch < gpuIndices.size() &&
               gpuIndices[mismatch] == cpuMesh.indices[mismatch])
            ++mismatch;
        errorMessage = "GPU mesh index mismatch at " + std::to_string(mismatch) +
            " gpu=" + std::to_string(gpuIndices[mismatch]) +
            " cpu=" + std::to_string(cpuMesh.indices[mismatch]) +
            " counts=" + std::to_string(gpuMesh.vertexCount) + "/" +
            std::to_string(gpuMesh.indexCount);
        ok = false;
    }

    if (ok)
    {
        for (size_t i = 0; i < cpuMesh.subMeshes.size(); ++i)
        {
            const terrain::TerrainSubMesh& expected = cpuMesh.subMeshes[i];
            const SubMesh& actual = gpuMesh.subMeshes[i];
            if (actual.firstIndex != expected.firstIndex ||
                actual.indexCount != expected.indexCount ||
                actual.terrainSurfaceIndexCount != expected.surfaceIndexCount)
            {
                errorMessage = "GPU mesh submesh metadata mismatch";
                ok = false;
                break;
            }
        }
    }

    if (ok)
    {
        std::cout << "[Terrain] GPU mesh assembly selfTest PASS"
                  << " | vertices=" << gpuMesh.vertexCount
                  << " indices=" << gpuMesh.indexCount
                  << " lods=" << gpuMesh.subMeshes.size()
                  << " skirts=on localColor=on"
                  << " maxPosError=" << maxPositionError
                  << " maxAttrError=" << maxAttributeError << "\n";
    }

    destroyBuildResult(gpuMesh);
    return ok;
}