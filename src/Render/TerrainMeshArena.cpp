#include "Render/TerrainMeshArena.h"

#include "RHI/Device.h"
#include "Scene/Model.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

TerrainMeshArena::TerrainMeshArena(Device& deviceArg, uint32_t vertexCapacityArg,
                                   uint32_t indexCapacityArg)
    : device(deviceArg),
      vertexCapacity(std::max(vertexCapacityArg, 1u)),
      indexCapacity(std::max(indexCapacityArg, 1u))
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
        std::cout << "[Terrain] mesh arena ready | vertexMB="
                  << (getVertexBytes() / (1024u * 1024u))
                  << " indexMB=" << (getIndexBytes() / (1024u * 1024u))
                  << " vertexCapacity=" << vertexCapacity
                  << " indexCapacity=" << indexCapacity << "\n";
    }
}

TerrainMeshArena::~TerrainMeshArena()
{
    destroyResources();
}

void TerrainMeshArena::reconfigure(uint32_t vertexCapacityArg, uint32_t indexCapacityArg)
{
    vertexCapacityArg = std::max(vertexCapacityArg, 1u);
    indexCapacityArg = std::max(indexCapacityArg, 1u);
    if (vertexCapacityArg == vertexCapacity && indexCapacityArg == indexCapacity)
        return;

    destroyResources();
    vertexCapacity = vertexCapacityArg;
    indexCapacity = indexCapacityArg;
    try
    {
        available = createResources();
    }
    catch (const std::exception& ex)
    {
        std::cout << "[Terrain] mesh arena unavailable | " << ex.what() << std::endl;
        available = false;
    }

    if (available)
    {
        std::cout << "[Terrain] mesh arena ready | vertexMB="
                  << (getVertexBytes() / (1024u * 1024u))
                  << " indexMB=" << (getIndexBytes() / (1024u * 1024u))
                  << " vertexCapacity=" << vertexCapacity
                  << " indexCapacity=" << indexCapacity << std::endl;
    }
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

    if (!createBuffer(static_cast<VkDeviceSize>(vertexCapacity) * kVertexStride,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      vertexBuffer, vertexAllocation))
    {
        return false;
    }

    if (!createBuffer(static_cast<VkDeviceSize>(indexCapacity) * sizeof(uint32_t),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      indexBuffer, indexAllocation))
    {
        destroyResources();
        return false;
    }

    vertexFreeSpans.assign(1, Span{ 0u, vertexCapacity });
    indexFreeSpans.assign(1, Span{ 0u, indexCapacity });
    usedVertexBytes = 0u;
    usedIndexBytes = 0u;
    allocationCount = 0u;
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
    vertexFreeSpans.clear();
    indexFreeSpans.clear();
    usedVertexBytes = 0u;
    usedIndexBytes = 0u;
    allocationCount = 0u;
}

bool TerrainMeshArena::allocateFrom(std::vector<Span>& freeSpans, uint32_t count,
                                    uint32_t& outOffset)
{
    outOffset = kInvalidOffset;
    if (count == 0u)
        return false;

    for (size_t i = 0; i < freeSpans.size(); ++i)
    {
        Span& span = freeSpans[i];
        if (span.count < count)
            continue;

        outOffset = span.offset;
        if (span.count == count)
            freeSpans.erase(freeSpans.begin() + static_cast<std::ptrdiff_t>(i));
        else
        {
            span.offset += count;
            span.count -= count;
        }
        return true;
    }
    return false;
}

void TerrainMeshArena::releaseTo(std::vector<Span>& freeSpans, uint32_t offset, uint32_t count)
{
    if (count == 0u)
        return;

    const auto position = std::lower_bound(
        freeSpans.begin(), freeSpans.end(), offset,
        [](const Span& span, uint32_t value) { return span.offset < value; });
    const size_t index = static_cast<size_t>(position - freeSpans.begin());
    freeSpans.insert(position, Span{ offset, count });

    if (index + 1u < freeSpans.size())
    {
        Span& current = freeSpans[index];
        const Span& next = freeSpans[index + 1u];
        if (current.offset + current.count == next.offset)
        {
            current.count += next.count;
            freeSpans.erase(freeSpans.begin() + static_cast<std::ptrdiff_t>(index + 1u));
        }
    }
    if (index > 0u)
    {
        Span& previous = freeSpans[index - 1u];
        const Span& current = freeSpans[index];
        if (previous.offset + previous.count == current.offset)
        {
            previous.count += current.count;
            freeSpans.erase(freeSpans.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

bool TerrainMeshArena::acquire(uint32_t vertexCount, uint32_t indexCount, Slice& outSlice)
{
    outSlice = Slice{};
    if (!available || vertexCount == 0u || indexCount == 0u)
        return false;

    uint32_t vertexOffset = kInvalidOffset;
    if (!allocateFrom(vertexFreeSpans, vertexCount, vertexOffset))
        return false;

    uint32_t indexOffset = kInvalidOffset;
    if (!allocateFrom(indexFreeSpans, indexCount, indexOffset))
    {
        releaseTo(vertexFreeSpans, vertexOffset, vertexCount);
        return false;
    }

    outSlice.vertexOffset = vertexOffset;
    outSlice.vertexCount = vertexCount;
    outSlice.indexOffset = indexOffset;
    outSlice.indexCount = indexCount;
    usedVertexBytes += static_cast<VkDeviceSize>(vertexCount) * kVertexStride;
    usedIndexBytes += static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);
    ++allocationCount;
    return true;
}

void TerrainMeshArena::copyInto(VkCommandBuffer commandBuffer, const Slice& slice,
                                VkBuffer sourceVertexBuffer, VkBuffer sourceIndexBuffer)
{
    if (!available || commandBuffer == VK_NULL_HANDLE || !slice.isValid())
        return;
    if (sourceVertexBuffer == VK_NULL_HANDLE || sourceIndexBuffer == VK_NULL_HANDLE)
        return;

    VkBufferCopy vertexCopy{};
    vertexCopy.dstOffset = static_cast<VkDeviceSize>(slice.vertexOffset) * kVertexStride;
    vertexCopy.size = static_cast<VkDeviceSize>(slice.vertexCount) * kVertexStride;
    vkCmdCopyBuffer(commandBuffer, sourceVertexBuffer, vertexBuffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.dstOffset = static_cast<VkDeviceSize>(slice.indexOffset) * sizeof(uint32_t);
    indexCopy.size = static_cast<VkDeviceSize>(slice.indexCount) * sizeof(uint32_t);
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
}

void TerrainMeshArena::release(const Slice& slice)
{
    if (!available || !slice.isValid())
        return;

    releaseTo(vertexFreeSpans, slice.vertexOffset, slice.vertexCount);
    releaseTo(indexFreeSpans, slice.indexOffset, slice.indexCount);
    usedVertexBytes -= std::min<VkDeviceSize>(
        usedVertexBytes, static_cast<VkDeviceSize>(slice.vertexCount) * kVertexStride);
    usedIndexBytes -= std::min<VkDeviceSize>(
        usedIndexBytes, static_cast<VkDeviceSize>(slice.indexCount) * sizeof(uint32_t));
    if (allocationCount > 0u)
        --allocationCount;
}

bool TerrainMeshArena::adopt(VkCommandBuffer commandBuffer, const Model& model,
                             Slice& outSlice, std::string& errorMessage)
{
    outSlice = Slice{};
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

    if (!acquire(model.getVertexCount(), model.getIndexCount(), outSlice))
    {
        errorMessage = "mesh arena has no room for this mesh";
        return false;
    }
    copyInto(commandBuffer, outSlice, sourceVertices, sourceIndices);
    return true;
}
