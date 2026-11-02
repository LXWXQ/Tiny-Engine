#include "Render/TerrainMeshArena.h"

#include "RHI/Device.h"
#include "Scene/Model.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
VkDeviceSize terrainArenaAlign(VkDeviceSize value)
{
    const VkDeviceSize alignment = 4;
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

TerrainMeshArena::TerrainMeshArena(Device& deviceArg)
    : device(deviceArg)
{
    try
    {
        available = createResources();
    }
    catch (const std::exception& ex)
    {
        std::cout << "[Terrain] mesh arena unavailable | " << ex.what() << "\n";
        available = false;
    }

    if (available)
    {
        const uint32_t vertexBytes = getVertexBytes();
        const uint32_t indexBytes = getIndexBytes();
        std::cout << "[Terrain] mesh arena ready | slots=" << kSlotCount
                  << " vertexMB=" << (vertexBytes / (1024u * 1024u))
                  << " indexMB=" << (indexBytes / (1024u * 1024u)) << "\n";
    }
}

TerrainMeshArena::~TerrainMeshArena()
{
    destroyResources();
}

bool TerrainMeshArena::createResources()
{
    const VmaAllocator allocator = device.getVmaAllocator();
    if (allocator == VK_NULL_HANDLE || device.getDevice() == VK_NULL_HANDLE)
        return false;

    const auto createBuffer = [allocator](VkDeviceSize size, VkBufferUsageFlags usage,
                                          VkBuffer& buffer, VmaAllocation& allocation)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        return vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
                               &buffer, &allocation, nullptr) == VK_SUCCESS;
    };

    if (!createBuffer(terrainArenaAlign(static_cast<VkDeviceSize>(kSlotCount) * kSlotVertexBytes),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      vertexBuffer, vertexAllocation))
    {
        return false;
    }

    if (!createBuffer(terrainArenaAlign(static_cast<VkDeviceSize>(kSlotCount) * kSlotIndexBytes),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      indexBuffer, indexAllocation))
    {
        destroyResources();
        return false;
    }

    slotUsed.assign(kSlotCount, 0u);
    adoptedCount = 0u;
    return true;
}

void TerrainMeshArena::destroyResources()
{
    const VmaAllocator allocator = device.getVmaAllocator();
    if (allocator == VK_NULL_HANDLE)
        return;

    if (indexBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(allocator, indexBuffer, indexAllocation);
        indexBuffer = VK_NULL_HANDLE;
        indexAllocation = VK_NULL_HANDLE;
    }
    if (vertexBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(allocator, vertexBuffer, vertexAllocation);
        vertexBuffer = VK_NULL_HANDLE;
        vertexAllocation = VK_NULL_HANDLE;
    }
    slotUsed.clear();
    adoptedCount = 0u;
}

uint32_t TerrainMeshArena::freeSlotCount() const
{
    uint32_t freeCount = 0u;
    for (uint8_t used : slotUsed)
    {
        if (used == 0u)
            ++freeCount;
    }
    return freeCount;
}

uint32_t TerrainMeshArena::getVertexOffset(uint32_t slot) const
{
    return slot * kSlotVertexCount;
}

uint32_t TerrainMeshArena::getIndexOffset(uint32_t slot) const
{
    const uint32_t slotIndices = kSlotIndexBytes / sizeof(uint32_t);
    return slot * slotIndices;
}

bool TerrainMeshArena::adopt(VkCommandBuffer commandBuffer, const Model& model,
                             uint32_t& outSlot, std::string& errorMessage)
{
    outSlot = kInvalidSlot;
    if (!available)
    {
        errorMessage = "mesh arena is not available";
        return false;
    }
    if (commandBuffer == VK_NULL_HANDLE)
    {
        errorMessage = "mesh arena adopt needs a command buffer";
        return false;
    }

    const VkBuffer sourceVertices = model.getVertexBufferHandle();
    const VkBuffer sourceIndices = model.getIndexBufferHandle();
    if (sourceVertices == VK_NULL_HANDLE || !model.hasIndexBufferData() ||
        sourceIndices == VK_NULL_HANDLE)
    {
        errorMessage = "terrain model has no GPU vertex/index buffer";
        return false;
    }

    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(model.getVertexCount()) * kVertexFloats * sizeof(float);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(model.getIndexCount()) * sizeof(uint32_t);
    if (vertexBytes > kSlotVertexBytes || indexBytes > kSlotIndexBytes)
    {
        errorMessage = "terrain chunk mesh exceeds the arena slot size";
        return false;
    }

    uint32_t slot = kInvalidSlot;
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        if (slotUsed[i] == 0u)
        {
            slot = i;
            break;
        }
    }
    if (slot == kInvalidSlot)
    {
        errorMessage = "mesh arena has no free slot";
        return false;
    }

    VkBufferCopy vertexCopy{};
    vertexCopy.srcOffset = 0;
    vertexCopy.dstOffset = static_cast<VkDeviceSize>(slot) * kSlotVertexBytes;
    vertexCopy.size = vertexBytes;
    vkCmdCopyBuffer(commandBuffer, sourceVertices, vertexBuffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.srcOffset = 0;
    indexCopy.dstOffset = static_cast<VkDeviceSize>(slot) * kSlotIndexBytes;
    indexCopy.size = indexBytes;
    vkCmdCopyBuffer(commandBuffer, sourceIndices, indexBuffer, 1, &indexCopy);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vertexBuffer;
    barrier.offset = vertexCopy.dstOffset;
    barrier.size = vertexCopy.size;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);

    barrier.buffer = indexBuffer;
    barrier.offset = indexCopy.dstOffset;
    barrier.size = indexCopy.size;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);

    slotUsed[slot] = 1u;
    ++adoptedCount;
    outSlot = slot;
    return true;
}

void TerrainMeshArena::release(uint32_t slot)
{
    if (slot >= kSlotCount)
        return;
    if (slotUsed[slot] == 0u)
        return;
    slotUsed[slot] = 0u;
    if (adoptedCount > 0u)
        --adoptedCount;
}
