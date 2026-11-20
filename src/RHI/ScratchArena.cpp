#include "RHI/ScratchArena.h"

#include "RHI/Device.h"
#include "RHI/VmaNaming.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tiny
{

namespace
{
const char* const kSlotNames[kFramesInFlight] = { "Scratch.Frame0", "Scratch.Frame1" };
}

ScratchArena::ScratchArena(Device& deviceRef, VkDeviceSize initialCapacityRef)
    : device(deviceRef), initialCapacity(initialCapacityRef)
{
    if (initialCapacity < 64 * 1024)
        initialCapacity = 64 * 1024;

    slots.resize(kFramesInFlight);

    // 每个槽位先按初始容量建好。第一帧就有 buffer 可用，扩容只在"真用超了"时发生。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        createSlotBuffer(frame, initialCapacity);

    statistics.capacity = initialCapacity;
}

ScratchArena::~ScratchArena()
{
    for (Slot& slot : slots)
    {
        for (Retired& retired : slot.retired)
        {
            if (retired.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(device.getVmaAllocator(), retired.buffer, retired.allocation);
        }
        slot.retired.clear();

        if (slot.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), slot.buffer, slot.allocation);
        slot.buffer = VK_NULL_HANDLE;
        slot.allocation = VK_NULL_HANDLE;
        slot.mapped = nullptr;
    }
}

void ScratchArena::createSlotBuffer(uint32_t index, VkDeviceSize capacity)
{
    Slot& slot = slots[index];

    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    // 一份大块要能被当作多种角色用：常量、顶点、索引、实例（storage）、间接参数。
    // 多要几个 usage 位不花钱，省得将来接一个新 Pass 时又得改这里。
    bufferInfo.size = capacity;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // SEQENTIAL_WRITE = 只在 CPU 侧顺序写、GPU 侧读；MAPPED = 建好就一直是映射状态，
    // 每帧只动指针，不碰 vmaMapMemory / vmaUnmapMemory。
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocInfo.flags |= tiny_vmaNameFlag();
    allocInfo.pUserData = const_cast<char*>(kSlotNames[index]);

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                        &slot.buffer, &slot.allocation, &info) != VK_SUCCESS)
    {
        throw std::runtime_error("ScratchArena: 每帧暂存区创建失败");
    }

    slot.mapped = info.pMappedData;
    slot.capacity = capacity;
    slot.offset = 0;
    slot.dirty = false;

    if (slot.mapped == nullptr)
        throw std::runtime_error("ScratchArena: 每帧暂存区没有拿到映射地址");
}

void ScratchArena::beginFrame(uint32_t frameSlot)
{
    const uint32_t slotIndex = frameSlot % kFramesInFlight;
    if (inFrame && currentSlot == slotIndex)
        return;   // 同一帧重复开帧（UBO 先取暂存）：保留已经用掉的偏移

    currentSlot = slotIndex;
    inFrame = true;

    Slot& slot = slots[currentSlot];

    // 这个槽位上一次用过的旧块，到这里才确定没人再用（见头文件里的说明）。
    // 旧块**不在这里回收**：它可能已经被别处的描述符集记住（典型：平面反射
    // 通道自建的 Set 0 用 applyClusterBindings 抓过一次），而那些 set 只在
    // 重建时才改写。销毁后仍被 draw 引用就是 UAF（验证层会报 08114）。
    // 扩容本身不频繁（每次翻倍），旧块留到本对象销毁时统一还。

    slot.offset = 0;
    slot.dirty = false;
    ++statistics.frames;
}

VkDeviceSize ScratchArena::alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    if (alignment <= 1)
        return value;
    return (value + alignment - 1) / alignment * alignment;
}

VkDeviceSize ScratchArena::grownCapacity(VkDeviceSize capacity, VkDeviceSize required)
{
    // 翻倍而不是"刚好够"：扩容本身要重建 VkBuffer，是这一项里唯一的分配动作，
    // 一次到位比每帧挤牙膏强。下界取 required 是为了处理"单次分配就超了"。
    return std::max(capacity * 2, required);
}

void ScratchArena::grow(uint32_t index, VkDeviceSize required)
{
    Slot& slot = slots[index];
    const VkDeviceSize capacity = grownCapacity(slot.capacity, required);

    // 旧块不能立刻销毁：本帧早先拿走的那些 Block 还指着它（描述符已经写进命令流）。
    if (slot.buffer != VK_NULL_HANDLE)
        slot.retired.push_back({ slot.buffer, slot.allocation });

    createSlotBuffer(index, capacity);

    ++statistics.grows;
    statistics.capacity = capacity;

    std::printf("[Scratch] 扩容到 %.2f MiB（这一槽位之前的分配仍然有效，旧块留到销毁时还",
                static_cast<double>(capacity) / (1024.0 * 1024.0));
}

ScratchArena::Block ScratchArena::allocate(uint32_t frameSlot, VkDeviceSize bytes, VkDeviceSize alignment)
{
    beginFrame(frameSlot);   // 幂等：同一帧第二次调用不会清掉已分配的偏移
    return allocate(bytes, alignment);
}

void ScratchArena::reserve(uint32_t frameSlot, VkDeviceSize bytes)
{
    beginFrame(frameSlot);
    if (bytes == 0)
        return;

    Slot& slot = slots[currentSlot];
    const VkDeviceSize required = slot.offset + bytes;
    if (required > slot.capacity)
        grow(currentSlot, required);
}

ScratchArena::Block ScratchArena::allocate(VkDeviceSize bytes, VkDeviceSize alignment)
{
    Block block{};
    if (!inFrame || bytes == 0)
        return block;

    if (alignment < 1)
        alignment = 1;

    Slot& slot = slots[currentSlot];
    const VkDeviceSize aligned = alignUp(bytes, alignment);

    if (slot.offset + aligned > slot.capacity)
        grow(currentSlot, slot.offset + aligned);

    // grow（重建）之后必须重新取引用 —— slots 本身没变，但 slot 状态已被重置。
    Slot& current = slots[currentSlot];

    block.buffer = current.buffer;
    block.offset = current.offset;
    block.size = bytes;
    block.mapped = static_cast<unsigned char*>(current.mapped) + current.offset;

    current.offset += aligned;
    current.dirty = true;

    ++statistics.allocations;
    statistics.bytes += bytes;
    if (current.offset > statistics.peakOffset)
        statistics.peakOffset = current.offset;

    return block;
}

void ScratchArena::flushIfNeeded()
{
    inFrame = false;

    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &properties);

    for (Slot& slot : slots)
    {
        if (!slot.dirty || slot.allocation == VK_NULL_HANDLE)
            continue;

        VmaAllocationInfo info{};
        vmaGetAllocationInfo(device.getVmaAllocator(), slot.allocation, &info);

        // COHERENT 内存写完就对 GPU 可见，刷新是多余的驱动调用。桌面独显的
        // host visible 内存几乎都是 COHERENT，所以多数机器上这里一次都不会发。
        const bool coherent = info.memoryType < properties.memoryTypeCount &&
                              (properties.memoryTypes[info.memoryType].propertyFlags &
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (coherent)
            ++statistics.coherentSkips;
        else
        {
            vmaFlushAllocation(device.getVmaAllocator(), slot.allocation, 0, slot.offset);
            ++statistics.flushes;
        }

        slot.dirty = false;
    }
}

void ScratchArena::report() const
{
    if (statistics.frames == 0)
        return;

    const double frames = static_cast<double>(statistics.frames);
    std::printf("[Scratch] %llu 帧 | 槽位 %u × %.2f MiB | 分配 %.1f 次/帧 / %.1f KB/帧 | "
                "峰值偏移 %.1f KB | 扩容 %llu 次 | 刷新 %llu 次（COHERENT 跳过 %llu 次）\n",
                static_cast<unsigned long long>(statistics.frames),
                static_cast<unsigned>(slots.size()),
                static_cast<double>(statistics.capacity) / (1024.0 * 1024.0),
                static_cast<double>(statistics.allocations) / frames,
                static_cast<double>(statistics.bytes) / frames / 1024.0,
                static_cast<double>(statistics.peakOffset) / 1024.0,
                static_cast<unsigned long long>(statistics.grows),
                static_cast<unsigned long long>(statistics.flushes),
                static_cast<unsigned long long>(statistics.coherentSkips));
}

bool ScratchArena::selfTest()
{
    bool ok = true;
    auto check = [&ok](bool condition, const char* what)
    {
        if (!condition)
        {
            std::printf("[Scratch] selfTest FAIL | %s\n", what);
            ok = false;
        }
    };

    // 对齐：已经对齐的不能白涨一格，未对齐的必须推到下一个边界。
    check(alignUp(16, 16) == 16, "已对齐的 16 字节不该再涨");
    check(alignUp(17, 16) == 32, "17 字节要涨到 32");
    check(alignUp(1, 256) == 256, "1 字节按 256 对齐要占 256");
    check(alignUp(1024, 256) == 1024, "1024 按 256 对齐原地不动");
    check(alignUp(0, 256) == 0, "0 字节仍是 0");

    // 扩容判据：翻倍优先，但必须够用。
    check(grownCapacity(1024 * 1024, 1024 * 1024 + 4096) == 2 * 1024 * 1024, "略超翻倍");
    check(grownCapacity(1024 * 1024, 3 * 1024 * 1024) == 3 * 1024 * 1024, "远超按需求");

    if (ok)
        std::printf("[Scratch] selfTest PASS | 对齐往返 + 扩容判据\n");
    return ok;
}

} // namespace tiny