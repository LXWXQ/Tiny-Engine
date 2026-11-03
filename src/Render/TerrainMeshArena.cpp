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
    slotVertexBytes.assign(kSlotCount, 0u);
    slotIndexBytes.assign(kSlotCount, 0u);
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

bool TerrainMeshArena::acquire(uint32_t vertexCount, uint32_t indexCount, uint32_t& outSlot)
{
    outSlot = kInvalidSlot;
    if (!available)
        return false;

    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(vertexCount) * kVertexFloats * sizeof(float);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);
    if (vertexBytes > kSlotVertexBytes || indexBytes > kSlotIndexBytes)
        return false;

    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        if (slotUsed[i] == 0u)
        {
            slotUsed[i] = 1u;
            slotVertexBytes[i] = vertexBytes;
            slotIndexBytes[i] = indexBytes;
            outSlot = i;
            return true;
        }
    }
    return false;
}

void TerrainMeshArena::copyInto(VkCommandBuffer commandBuffer, uint32_t slot,
                                VkBuffer sourceVertexBuffer, VkBuffer sourceIndexBuffer)
{
    if (!available || commandBuffer == VK_NULL_HANDLE || slot >= kSlotCount ||
        slotUsed[slot] == 0u)
        return;
    if (sourceVertexBuffer == VK_NULL_HANDLE || sourceIndexBuffer == VK_NULL_HANDLE)
        return;

    const VkDeviceSize vertexBytes = slotVertexBytes[slot];
    const VkDeviceSize indexBytes = slotIndexBytes[slot];

    VkBufferCopy vertexCopy{};
    vertexCopy.dstOffset = static_cast<VkDeviceSize>(slot) * kSlotVertexBytes;
    vertexCopy.size = vertexBytes;
    vkCmdCopyBuffer(commandBuffer, sourceVertexBuffer, vertexBuffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.dstOffset = static_cast<VkDeviceSize>(slot) * kSlotIndexBytes;
    indexCopy.size = indexBytes;
    vkCmdCopyBuffer(commandBuffer, sourceIndexBuffer, indexBuffer, 1, &indexCopy);

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

    ++adoptedCount;
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

    uint32_t slot = kInvalidSlot;
    if (!acquire(model.getVertexCount(), model.getIndexCount(), slot))
    {
        errorMessage = "mesh arena has no free slot or the mesh is too large";
        return false;
    }
    copyInto(commandBuffer, slot, sourceVertices, sourceIndices);
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
