#include "Render/TerrainComputeGenerator.h"
#include "AssetPaths.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
VkBufferMemoryBarrier bufferBarrier(VkBuffer buffer, VkDeviceSize size,
                                    VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    return barrier;
}
} // namespace

TerrainComputeGenerator::TerrainComputeGenerator(Device& deviceArg,
                                                 DescriptorManager& descriptorManagerArg)
    : device(deviceArg), descriptorManager(descriptorManagerArg)
{
    try
    {
        createCommandPool();
        createBuffers();
        createDescriptorSets();
        createPipeline();
        available = true;
        std::cout << "[Terrain] GPU compute heightfield generator ready | slots="
                  << kSlotCount << " max " << kMaxResolution << "x" << kMaxResolution << "\n";
    }
    catch (const std::exception& ex)
    {
        destroyResources();
        throw std::runtime_error(std::string("TerrainComputeGenerator init failed: ") + ex.what());
    }
}

TerrainComputeGenerator::~TerrainComputeGenerator()
{
    destroyResources();
}

uint32_t TerrainComputeGenerator::inFlightCount() const
{
    uint32_t count = 0;
    for (const Slot& slot : slots)
    {
        if (slot.inFlight)
            ++count;
    }
    return count;
}

void TerrainComputeGenerator::createCommandPool()
{
    const QueueFamilyIndices families = device.findPhysicalQueueFamilies();
    if (!families.computeFamily.has_value())
        throw std::runtime_error("no compute queue family");

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = families.computeFamily.value();
    if (vkCreateCommandPool(device.getDevice(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
        throw std::runtime_error("command pool creation failed");

    for (Slot& slot : slots)
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device.getDevice(), &allocInfo,
                                     &slot.commandBuffer) != VK_SUCCESS)
            throw std::runtime_error("command buffer allocation failed");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device.getDevice(), &fenceInfo, nullptr,
                          &slot.fence) != VK_SUCCESS)
            throw std::runtime_error("fence creation failed");
    }
}

void TerrainComputeGenerator::createBuffers()
{
    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                            VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags flags,
                            VkBuffer& buffer, VmaAllocation& allocation,
                            VmaAllocationInfo* allocationInfo)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = memoryUsage;
        allocInfo.flags = flags;
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &buffer, &allocation, allocationInfo) != VK_SUCCESS)
            throw std::runtime_error("VMA buffer creation failed");
    };

    for (Slot& slot : slots)
    {
        createBuffer(kMaxHeightBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VMA_MEMORY_USAGE_AUTO, 0,
                     slot.outputBuffer, slot.outputAllocation, nullptr);

        VmaAllocationInfo readbackInfo{};
        createBuffer(kMaxHeightBytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VMA_MEMORY_USAGE_AUTO,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT,
                     slot.readbackBuffer, slot.readbackAllocation, &readbackInfo);
        slot.readbackMapped = readbackInfo.pMappedData;
        if (!slot.readbackMapped)
            throw std::runtime_error("readback buffer is not mapped");
    }
}

void TerrainComputeGenerator::createDescriptorSets()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    setLayout = descriptorManager.createSetLayout({ binding });

    for (Slot& slot : slots)
    {
        slot.descriptorSet = descriptorManager.allocateSet(setLayout);
        VkDescriptorBufferInfo outputInfo{ slot.outputBuffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = slot.descriptorSet;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &outputInfo;
        vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);
    }
}

void TerrainComputeGenerator::createPipeline()
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("pipeline layout creation failed");

    pipeline = std::make_unique<ComputePipeline>(
        device, SENTINEL_SHADER_DIR "terrain_height.comp.spv", pipelineLayout);
}

bool TerrainComputeGenerator::recordAndSubmit(Slot& slot,
                                              const terrain::TerrainConfig& config,
                                              std::string& errorMessage)
{
    const uint32_t count = config.resolutionX * config.resolutionZ;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(count) * sizeof(float);
    if (bytes > kMaxHeightBytes)
    {
        errorMessage = "terrain resolution exceeds GPU generator capacity";
        return false;
    }

    if (vkResetFences(device.getDevice(), 1, &slot.fence) != VK_SUCCESS ||
        vkResetCommandBuffer(slot.commandBuffer, 0) != VK_SUCCESS)
    {
        errorMessage = "failed to reset GPU terrain command state";
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        errorMessage = "failed to begin GPU terrain command buffer";
        return false;
    }

    PushConstants push{};
    push.resolutionX = config.resolutionX;
    push.resolutionZ = config.resolutionZ;
    push.sizeX = config.sizeX;
    push.sizeZ = config.sizeZ;
    push.sampleOriginX = config.sampleOriginX;
    push.sampleOriginZ = config.sampleOriginZ;
    push.seed = config.seed;
    push.octaveCount = config.octaveCount;
    push.noiseFrequency = config.noiseFrequency;
    push.persistence = config.persistence;
    push.lacunarity = config.lacunarity;
    push.ridgeWeight = config.ridgeWeight;
    push.detailWeight = config.detailWeight;
    push.edgeFalloff = config.edgeFalloff;
    push.heightCurve = config.heightCurve;
    push.islandWeight = config.islandWeight;
    push.islandRadius = config.islandRadius;
    push.ridgeSharpness = config.ridgeSharpness;
    push.detailFrequency = config.detailFrequency;

    pipeline->bind(slot.commandBuffer);
    vkCmdBindDescriptorSets(slot.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &slot.descriptorSet, 0, nullptr);
    vkCmdPushConstants(slot.commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(slot.commandBuffer, (count + 63u) / 64u, 1, 1);

    VkBufferMemoryBarrier toTransfer = bufferBarrier(
        slot.outputBuffer, bytes, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(slot.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                         &toTransfer, 0, nullptr);
    VkBufferCopy copy{ 0, 0, bytes };
    vkCmdCopyBuffer(slot.commandBuffer, slot.outputBuffer, slot.readbackBuffer, 1, &copy);

    VkBufferMemoryBarrier toHost = bufferBarrier(
        slot.readbackBuffer, bytes, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    vkCmdPipelineBarrier(slot.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                         &toHost, 0, nullptr);

    if (vkEndCommandBuffer(slot.commandBuffer) != VK_SUCCESS)
    {
        errorMessage = "failed to end GPU terrain command buffer";
        return false;
    }

    slot.config = config;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commandBuffer;
    if (vkQueueSubmit(device.getComputeQueue(), 1, &submitInfo, slot.fence) != VK_SUCCESS)
    {
        errorMessage = "failed to submit GPU terrain compute";
        return false;
    }
    return true;
}

uint64_t TerrainComputeGenerator::submit(const terrain::TerrainConfig& config,
                                         std::string& errorMessage)
{
    errorMessage.clear();
    if (!available)
    {
        errorMessage = "GPU terrain generator is unavailable";
        return 0;
    }
    if (!config.validate(errorMessage))
        return 0;

    collectFinished();
    for (Slot& slot : slots)
    {
        if (slot.inFlight)
            continue;
        const uint64_t requestId = nextRequestId++;
        slot.requestId = requestId;
        slot.abandoned = false;
        if (!recordAndSubmit(slot, config, errorMessage))
        {
            resetSlot(slot);
            return 0;
        }
        slot.inFlight = true;
        return requestId;
    }

    errorMessage = "GPU terrain generator has no free in-flight slot";
    return 0;
}

TerrainComputeGenerator::PollStatus TerrainComputeGenerator::poll(
    uint64_t requestId, terrain::HeightField& outHeightField, std::string& errorMessage)
{
    errorMessage.clear();
    Slot* slot = findRequest(requestId);
    if (!slot)
    {
        errorMessage = "unknown GPU terrain request";
        return PollStatus::Failed;
    }

    const VkResult status = vkGetFenceStatus(device.getDevice(), slot->fence);
    if (slot->abandoned)
    {
        // release() 之后仍可能有人来 poll：明确报 abandoned。
        // 只有 fence 真的完成才回收槽位，否则复用会覆盖 GPU 仍在写的 readback。
        if (status != VK_NOT_READY)
            resetSlot(*slot);
        errorMessage = "GPU terrain request was abandoned";
        return PollStatus::Failed;
    }

    if (status == VK_NOT_READY)
        return PollStatus::Pending;
    if (status != VK_SUCCESS)
    {
        errorMessage = "GPU terrain fence failed";
        resetSlot(*slot);
        return PollStatus::Failed;
    }

    const terrain::TerrainConfig config = slot->config;
    const uint32_t count = config.resolutionX * config.resolutionZ;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(count) * sizeof(float);
    if (vmaInvalidateAllocation(device.getVmaAllocator(), slot->readbackAllocation,
                                0, bytes) != VK_SUCCESS)
    {
        errorMessage = "failed to invalidate GPU terrain readback";
        resetSlot(*slot);
        return PollStatus::Failed;
    }

    outHeightField.generatedOnGpu = true;
    outHeightField.width = config.resolutionX;
    outHeightField.height = config.resolutionZ;
    outHeightField.sizeX = config.sizeX;
    outHeightField.sizeZ = config.sizeZ;
    outHeightField.sampleOriginX = config.sampleOriginX;
    outHeightField.sampleOriginZ = config.sampleOriginZ;
    outHeightField.baseHeight = config.baseHeight;
    outHeightField.heightScale = config.heightScale;
    const float* source = static_cast<const float*>(slot->readbackMapped);
    outHeightField.heights.assign(source, source + count);
    outHeightField.refreshMinMax();
    resetSlot(*slot);
    return PollStatus::Completed;
}

void TerrainComputeGenerator::release(uint64_t requestId)
{
    if (Slot* slot = findRequest(requestId))
        slot->abandoned = true;
}

void TerrainComputeGenerator::collectFinished()
{
    for (Slot& slot : slots)
    {
        if (!slot.inFlight || !slot.abandoned)
            continue;
        const VkResult status = vkGetFenceStatus(device.getDevice(), slot.fence);
        // 只需要“fence 已不再 pending”；成功/设备丢失都可以安全回收槽位。
        if (status != VK_NOT_READY)
            resetSlot(slot);
    }
}

void TerrainComputeGenerator::waitAll()
{
    std::array<VkFence, kSlotCount> fences{};
    uint32_t count = 0;
    for (Slot& slot : slots)
    {
        if (slot.inFlight)
            fences[count++] = slot.fence;
    }
    if (count > 0)
        vkWaitForFences(device.getDevice(), count, fences.data(), VK_TRUE,
                        std::numeric_limits<uint64_t>::max());
    for (Slot& slot : slots)
    {
        if (slot.inFlight)
            resetSlot(slot);
    }
}

void TerrainComputeGenerator::resetSlot(Slot& slot)
{
    slot.inFlight = false;
    slot.abandoned = false;
    slot.requestId = 0;
    slot.config = {};
}

TerrainComputeGenerator::Slot* TerrainComputeGenerator::findRequest(uint64_t requestId)
{
    for (Slot& slot : slots)
    {
        if (slot.inFlight && slot.requestId == requestId)
            return &slot;
    }
    return nullptr;
}

bool TerrainComputeGenerator::generate(const terrain::TerrainConfig& config,
                                       terrain::HeightField& outHeightField,
                                       std::string& errorMessage)
{
    uint64_t requestId = 0;
    for (uint32_t attempt = 0; attempt < 5000 && requestId == 0; ++attempt)
    {
        requestId = submit(config, errorMessage);
        if (requestId == 0 && errorMessage.find("no free") == std::string::npos)
            return false;
        if (requestId == 0)
        {
            collectFinished();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (requestId == 0)
    {
        errorMessage = "timed out waiting for a GPU terrain slot";
        return false;
    }

    while (true)
    {
        const PollStatus status = poll(requestId, outHeightField, errorMessage);
        if (status == PollStatus::Completed)
            return true;
        if (status == PollStatus::Failed)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void TerrainComputeGenerator::destroyResources()
{
    available = false;
    if (commandPool != VK_NULL_HANDLE)
        vkQueueWaitIdle(device.getComputeQueue());

    pipeline.reset();
    for (Slot& slot : slots)
    {
        if (slot.readbackBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device.getVmaAllocator(), slot.readbackBuffer,
                             slot.readbackAllocation);
        }
        if (slot.outputBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device.getVmaAllocator(), slot.outputBuffer,
                             slot.outputAllocation);
        }
        if (slot.fence != VK_NULL_HANDLE)
            vkDestroyFence(device.getDevice(), slot.fence, nullptr);
        slot = {};
    }
    if (commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device.getDevice(), commandPool, nullptr);
        commandPool = VK_NULL_HANDLE;
    }
    if (pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
    if (setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
        setLayout = VK_NULL_HANDLE;
    }
}
