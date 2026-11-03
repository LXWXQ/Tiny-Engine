#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "RHI/VmaConfig.h" // must precede vk_mem_alloc.h
#include <vk_mem_alloc.h>

class Device;
class Model;

// F2-b / F2-g：地形 chunk 网格 arena。
//
// 流送世界每个 chunk 原本各持一份 device-local 顶点/索引缓冲，绘制时逐 chunk
// 绑定、逐 chunk 一条 indirect；这里把所有 chunk 网格拷进同一份大缓冲，绘制侧
// 只绑一次 VBO/IBO，再用一条 vkCmdDrawIndexedIndirect 画完全部 chunk。
//
// F2-g：分配方式从「固定槽位」换成 bump + 空闲区间的首次适配分配：
//   - 大缓冲按总容量分配（顶点 / 索引各一份），每块 chunk 只占自己实际大小；
//   - 释放时把区间还回空闲表并与相邻区间合并；
//   - 因此 33x33 的 chunk（约 1620 顶点 / 8544 索引）不再被 2048/12288 的
//     固定槽位浪费，同一份显存能装下更多 chunk。
// 分配失败（容量不足或单块超出剩余空间）时调用方退回该 chunk 自己的缓冲。
//
// 每个 chunk 的世界平移由顶点着色器按 set 3 的 chunk 实例表施加，所以这里只做
// 原样拷贝，不做任何顶点变换。
class TerrainMeshArena
{
public:
    static constexpr uint32_t kVertexStride = 44u; // pos3 + color3 + normal3 + uv2
    static constexpr uint32_t kVertexFloats = 11u;
    static constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

    // 容量按「半径 12 的整环」估算：625 块 x 约 1620 顶点 x 44B 约 44.5 MB，
    // 索引 625 x 约 8544 x 4B 约 21.4 MB；这里留约 10% 余量。
    static constexpr uint32_t kVertexCapacity = 1100000u;  // 约 46 MB
    static constexpr uint32_t kIndexCapacity = 5900000u;   // 约 22.5 MB

    // 一块 chunk 在 arena 里的切片。offset 以顶点 / 索引为单位，
    // 直接喂给 indirect 命令的 vertexOffset / firstIndex 基址。
    struct Slice
    {
        uint32_t vertexOffset = kInvalidOffset;
        uint32_t vertexCount = 0u;
        uint32_t indexOffset = kInvalidOffset;
        uint32_t indexCount = 0u;

        bool isValid() const { return vertexOffset != kInvalidOffset; }
    };

    explicit TerrainMeshArena(Device& device);
    ~TerrainMeshArena();

    TerrainMeshArena(const TerrainMeshArena&) = delete;
    TerrainMeshArena& operator=(const TerrainMeshArena&) = delete;

    bool isAvailable() const { return available; }

    // 预留一块实际大小的切片；失败时不改动空闲表。
    bool acquire(uint32_t vertexCount, uint32_t indexCount, Slice& outSlice);
    // 记录两次 vkCmdCopyBuffer 与读屏障；切片必须来自 acquire()。
    void copyInto(VkCommandBuffer commandBuffer, const Slice& slice,
                  VkBuffer sourceVertexBuffer, VkBuffer sourceIndexBuffer);
    // 归还切片，并与相邻空闲区间合并。
    void release(const Slice& slice);

    // 便于旧调用点：预留 + 拷贝模型自己的缓冲。
    bool adopt(VkCommandBuffer commandBuffer, const Model& model,
               Slice& outSlice, std::string& errorMessage);

    VkBuffer getVertexBuffer() const { return vertexBuffer; }
    VkBuffer getIndexBuffer() const { return indexBuffer; }
    uint32_t getAllocationCount() const { return allocationCount; }
    VkDeviceSize getVertexBytes() const { return kVertexCapacity * kVertexStride; }
    VkDeviceSize getIndexBytes() const { return kIndexCapacity * sizeof(uint32_t); }
    VkDeviceSize getUsedVertexBytes() const { return usedVertexBytes; }
    VkDeviceSize getUsedIndexBytes() const { return usedIndexBytes; }

private:
    struct Span
    {
        uint32_t offset = 0u;
        uint32_t count = 0u;
    };

    bool createResources();
    void destroyResources();
    static bool allocateFrom(std::vector<Span>& freeSpans, uint32_t count, uint32_t& outOffset);
    static void releaseTo(std::vector<Span>& freeSpans, uint32_t offset, uint32_t count);

    Device& device;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    std::vector<Span> vertexFreeSpans;
    std::vector<Span> indexFreeSpans;
    VkDeviceSize usedVertexBytes = 0u;
    VkDeviceSize usedIndexBytes = 0u;
    uint32_t allocationCount = 0u;
    bool available = false;
};

