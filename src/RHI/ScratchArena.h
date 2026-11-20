#pragma once

// 每帧暂存区（TM-07）。
//
// 解决的问题：每个 Pass 自己建一份 host visible 的映射 buffer 来放"这一帧才有效"的
// 数据（相机参数、SSAO 参数、实例列表 …），于是出现三件坏事：
//   1. 每份 buffer 各带一个 VmaAllocation，帧数 × Pass 数地涨；
//   2. 每份都要自己 memcpy + vmaFlushAllocation —— 一帧里十几次刷新；
//   3. 写成固定大小（比如编辑器选中框只留两个盒子的空间），数量一涨就悄悄越界。
//
// 这里改成"每帧一个大块，bump 分配"：帧首把 offset 归零，之后每个消费者拿走一段，
// 整帧结束只刷新一次；空间不够就整块翻倍，旧块留到下次轮到这个槽位再销毁。
//
// 为什么可以"留到下一次 beginFrame 再销毁"：槽位是按 kFramesInFlight 轮转的，
// 同一个槽位被复用，前提就是它上一次提交已经完成（Renderer 的 waitForFrameSlot
// 保证这一点）。所以轮转回来时，旧块一定已经不被 GPU 使用。

#include <vulkan/vulkan.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

#include "Core/FrameInFlight.h"

class Device;   // 全局作用域：RHI/Device.h 里的 Device 不在 tiny 命名空间里

namespace tiny
{

class ScratchArena
{
public:
    // 一段分配。buffer 与 offset 直接拿去填描述符，mapped 是加上 offset 之后的写指针，
    // 照着自己那一段 memcpy 即可 —— 不需要再查映射地址。
    struct Block
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    struct Stats
    {
        uint64_t frames = 0;        // beginFrame 次数
        uint64_t allocations = 0;   // allocate 次数
        uint64_t bytes = 0;         // 累计分配字节（用户请求的，不含对齐填充）
        uint64_t grows = 0;         // 扩容次数（理想情况是 0）
        uint64_t flushes = 0;       // 真正发出的 vmaFlushAllocation 次数
        uint64_t coherentSkips = 0; // 内存本来就是 COHERENT、跳过刷新的次数
        VkDeviceSize peakOffset = 0; // 单帧峰值占用（含对齐填充）
        VkDeviceSize capacity = 0;   // 当前单槽容量
    };

    explicit ScratchArena(Device& device, VkDeviceSize initialCapacity = 1024 * 1024);
    ~ScratchArena();

    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;

    // 帧首：轮转到 frameSlot，销毁这一槽位上一轮淘汰的旧块，offset 归零。
    void beginFrame(uint32_t frameSlot);

    // 取一段。alignment 默认 16（顶点 / 索引 / 实例数据的自然对齐）；
    // uniform buffer 必须传 256 —— 规范保证 minUniformBufferOffsetAlignment 不会超过它。
    Block allocate(VkDeviceSize bytes, VkDeviceSize alignment = 16);

    // 在 beginFrame 之前取暂存（UBO 要在渲染图开帧前就填好）。frameSlot 与
    // beginFrame 的参数同义；同一帧重复开帧不会重置偏移，所以渲染图照常开帧即可。
    Block allocate(uint32_t frameSlot, VkDeviceSize bytes, VkDeviceSize alignment = 16);

    // 先把这一槽位撑到至少 bytes，再开始分配。
    // 为什么需要它：allocate 在容量不够时会**新建** buffer 并把偏移归零，
    // 于是同一帧里先后拿到的两个 Block 可能属于不同的 buffer。凡是"一次分配
    // 一组、之后当作同一块内存用（带偏移写描述符）"的地方，都必须先 reserve。
    void reserve(uint32_t frameSlot, VkDeviceSize bytes);

    // 帧尾：把这一帧写过的槽位刷新一次。COHERENT 内存直接跳过（省掉一次驱动调用）。
    void flushIfNeeded();

    const Stats& getStats() const { return statistics; }
    void report() const;

    static bool selfTest();

    // 纯算术，做成 static 是为了自检能在不建 Device 的情况下跑。
    static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment);
    static VkDeviceSize grownCapacity(VkDeviceSize capacity, VkDeviceSize required);

private:
    struct Retired
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };

    struct Slot
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkDeviceSize offset = 0;
        bool dirty = false;
        // 扩容淘汰的旧 buffer。只在 ScratchArena 析构时销毁 —— 见 beginFrame 里的注释。
        std::vector<Retired> retired;
    };

    void createSlotBuffer(uint32_t index, VkDeviceSize capacity);
    void grow(uint32_t index, VkDeviceSize required);

    Device& device;
    VkDeviceSize initialCapacity;
    std::vector<Slot> slots;
    uint32_t currentSlot = 0;
    bool inFrame = false;
    Stats statistics{};
};

} // namespace tiny