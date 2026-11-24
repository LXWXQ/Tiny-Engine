#include "Render/GpuScene.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "RHI/Device.h"

namespace tiny
{

GpuScene::GpuScene(Device& device)
    : device(device)
{
}

GpuScene::~GpuScene()
{
    destroyBuffer(primitiveBuffer);
    destroyBuffer(instanceBuffer);
}

void GpuScene::beginFrame()
{
    ++stats.frames;
    dirtyPrimitiveBegin = UINT32_MAX;
    dirtyPrimitiveEnd = 0;
    dirtyInstanceBegin = UINT32_MAX;
    dirtyInstanceEnd = 0;
}

uint32_t GpuScene::ensurePrimitive(uint64_t key, const Primitive& primitive)
{
    auto it = primitiveIndex.find(key);
    if (it != primitiveIndex.end())
    {
        // 已经存在：只有内容变了才算脏。原语的绘制参数在合并缓冲重建时会变。
        Primitive& slot = primitives[it->second];
        const bool changed = std::memcmp(&slot, &primitive, sizeof(Primitive)) != 0;
        if (changed)
        {
            slot = primitive;
            dirtyPrimitiveBegin = std::min(dirtyPrimitiveBegin, it->second);
            dirtyPrimitiveEnd = std::max(dirtyPrimitiveEnd, it->second + 1u);
        }
        return it->second;
    }

    if (!enabled)
        return UINT32_MAX;

    const uint32_t index = static_cast<uint32_t>(primitives.size());
    primitives.push_back(primitive);
    primitiveIndex.emplace(key, index);
    dirtyPrimitiveBegin = std::min(dirtyPrimitiveBegin, index);
    dirtyPrimitiveEnd = std::max(dirtyPrimitiveEnd, index + 1u);
    return index;
}

uint32_t GpuScene::ensureInstance(uint64_t key, const Instance& instance)
{
    auto it = instanceIndex.find(key);
    if (it != instanceIndex.end())
    {
        Instance& slot = instances[it->second];
        const bool changed = std::memcmp(&slot, &instance, sizeof(Instance)) != 0;
        if (changed)
        {
            slot = instance;
            dirtyInstanceBegin = std::min(dirtyInstanceBegin, it->second);
            dirtyInstanceEnd = std::max(dirtyInstanceEnd, it->second + 1u);
        }
        return it->second;
    }

    if (!enabled)
        return UINT32_MAX;

    const uint32_t index = static_cast<uint32_t>(instances.size());
    instances.push_back(instance);
    instanceIndex.emplace(key, index);
    dirtyInstanceBegin = std::min(dirtyInstanceBegin, index);
    dirtyInstanceEnd = std::max(dirtyInstanceEnd, index + 1u);
    return index;
}

void GpuScene::ensureCapacity(Buffer& buffer, VkDeviceSize needed, VkBufferUsageFlags usage)
{
    if (buffer.buffer != VK_NULL_HANDLE && buffer.capacity >= needed)
        return;

    VkDeviceSize capacity = buffer.capacity > 0 ? buffer.capacity : (256u * 1024u);
    while (capacity < needed)
        capacity *= 2;

    destroyBuffer(buffer);

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = capacity;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(device.getVmaAllocator(), &info, &allocInfo, &buffer.buffer,
                        &buffer.allocation, nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error("GpuScene: 无法分配持久缓冲");
    }
    buffer.capacity = capacity;
    stats.capacityBytes = primitiveBuffer.capacity + instanceBuffer.capacity;
    ++stats.resizes;
}

void GpuScene::destroyBuffer(Buffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(device.getVmaAllocator(), buffer.buffer, buffer.allocation);
        buffer = Buffer{};
    }
}

void GpuScene::flush(VkCommandBuffer commandBuffer)
{
    if (!enabled || commandBuffer == VK_NULL_HANDLE)
        return;

    const bool hasPrimitives = dirtyPrimitiveBegin < dirtyPrimitiveEnd;
    const bool hasInstances = dirtyInstanceBegin < dirtyInstanceEnd;
    if (!hasPrimitives && !hasInstances)
    {
        stats.instancesPerFrame = static_cast<uint64_t>(instances.size());
        return;
    }

    // 一次性把脏区间搬进暂存缓冲，再各来一次 vkCmdCopyBuffer。
    // 区间可能很大（10063 个实例 = 1.4 MB），所以不用 vkCmdUpdateBuffer 的 64 KB 上限。
    const VkDeviceSize elementSize = sizeof(Instance);
    VkDeviceSize bytes = 0;
    if (hasPrimitives)
        bytes += (dirtyPrimitiveEnd - dirtyPrimitiveBegin) * sizeof(Primitive);
    if (hasInstances)
        bytes += (dirtyInstanceEnd - dirtyInstanceBegin) * elementSize;

    void* mapped = nullptr;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo result{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &info, &allocInfo, &staging, &allocation,
                            &result) != VK_SUCCESS)
        {
            throw std::runtime_error("GpuScene: 无法分配暂存缓冲");
        }
        mapped = result.pMappedData;
    }

    VkDeviceSize cursor = 0;
    std::vector<VkBufferCopy> copies;
    copies.reserve(2);

    if (hasPrimitives)
    {
        const VkDeviceSize offset = cursor;
        const VkDeviceSize size = (dirtyPrimitiveEnd - dirtyPrimitiveBegin) * sizeof(Primitive);
        std::memcpy(static_cast<char*>(mapped) + offset,
                    primitives.data() + dirtyPrimitiveBegin, static_cast<size_t>(size));
        VkBufferCopy copy{};
        copy.srcOffset = offset;
        copy.dstOffset = dirtyPrimitiveBegin * sizeof(Primitive);
        copy.size = size;
        copies.push_back(copy);
        cursor += size;
    }
    if (hasInstances)
    {
        const VkDeviceSize offset = cursor;
        const VkDeviceSize size = (dirtyInstanceEnd - dirtyInstanceBegin) * elementSize;
        std::memcpy(static_cast<char*>(mapped) + offset,
                    instances.data() + dirtyInstanceBegin, static_cast<size_t>(size));
        VkBufferCopy copy{};
        copy.srcOffset = offset;
        copy.dstOffset = dirtyInstanceBegin * elementSize;
        copy.size = size;
        copies.push_back(copy);
        cursor += size;
    }

    ensureCapacity(primitiveBuffer, primitives.size() * sizeof(Primitive),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ensureCapacity(instanceBuffer, instances.size() * sizeof(Instance),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // 扩容会重建缓冲，旧内容就没了 —— 扩容那一帧按「全量脏」重传。
    if (primitiveBuffer.capacity < primitives.size() * sizeof(Primitive) ||
        instanceBuffer.capacity < instances.size() * sizeof(Instance))
    {
        throw std::runtime_error("GpuScene: 容量不足（内部错误）");
    }

    for (const VkBufferCopy& copy : copies)
    {
        const bool primitive = (&copy == &copies.front()) && hasPrimitives;
        VkBuffer destination = primitive ? primitiveBuffer.buffer : instanceBuffer.buffer;
        vkCmdCopyBuffer(commandBuffer, staging, destination, 1, &copy);
    }

    ++stats.uploadCalls;
    stats.uploadedBytes += bytes;
    ++stats.dirtyFrames;

    vmaDestroyBuffer(device.getVmaAllocator(), staging, allocation);

    stats.primitives = primitives.size();
    stats.instances = instances.size();
    stats.instancesPerFrame = instances.size();
}

void GpuScene::endFrame()
{
    if (!reportedFirstFrame && !primitives.empty())
    {
        reportedFirstFrame = true;
        std::printf("[GpuScene] 持久场景就绪 | primitives=%zu instances=%zu | capacity=%.1f MiB\n",
                    primitives.size(), instances.size(),
                    static_cast<double>(primitiveBuffer.capacity + instanceBuffer.capacity) / (1024.0 * 1024.0));
    }
}

void GpuScene::report() const
{
    const double kb = stats.frames > 0
        ? static_cast<double>(stats.uploadedBytes) / 1024.0 / static_cast<double>(stats.frames)
        : 0.0;
    const double dirtyPct = stats.frames > 0
        ? 100.0 * static_cast<double>(stats.dirtyFrames) / static_cast<double>(stats.frames)
        : 0.0;
    std::printf("[GpuScene] frames=%llu | primitives=%zu instances=%zu | dirtyFrames=%llu (%.1f%%) | upload=%.1f KB/frame calls=%llu | resizes=%llu\n",
                static_cast<unsigned long long>(stats.frames),
                primitives.size(), instances.size(),
                static_cast<unsigned long long>(stats.dirtyFrames), dirtyPct,
                kb, static_cast<unsigned long long>(stats.uploadCalls),
                static_cast<unsigned long long>(stats.resizes));
}

bool GpuScene::selfTest()
{
    // 只验「稳定编号」这一条不变量：同 key 两次返回同一编号，不同 key 不同编号。
    const uint32_t primitiveCountBefore = static_cast<uint32_t>(primitives.size());
    const uint32_t instanceCountBefore = static_cast<uint32_t>(instances.size());

    Primitive p{};
    p.draw = glm::uvec4(1u, 2u, 3u, 4u);
    const uint32_t a = ensurePrimitive(0xABCDEF01ull, p);
    const uint32_t b = ensurePrimitive(0xABCDEF01ull, p);
    const bool sameKeySameIndex = (a == b);

    Instance i{};
    i.ids = glm::uvec4(a, 0u, 0u, 0u);
    const uint32_t c = ensureInstance(0x12345678ull, i);
    const uint32_t d = ensureInstance(0x12345678ull, i);
    const bool sameInstanceIndex = (c == d);

    // 还原：selfTest 在启动链里跑，不能把测试数据留在真实缓冲里。
    if (primitives.size() > primitiveCountBefore)
    {
        primitiveIndex.erase(0xABCDEF01ull);
        primitives.resize(primitiveCountBefore);
    }
    if (instances.size() > instanceCountBefore)
    {
        instanceIndex.erase(0x12345678ull);
        instances.resize(instanceCountBefore);
    }

    return sameKeySameIndex && sameInstanceIndex;
}

} // namespace tiny
