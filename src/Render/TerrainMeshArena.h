#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "RHI/VmaConfig.h" // must precede vk_mem_alloc.h
#include <vk_mem_alloc.h>

class Device;
class Model;

// F2-b：地形 chunk 网格 arena。
//
// 流送世界每个 chunk 原本各持一份 device-local 顶点/索引缓冲，绘制时逐 chunk
// 绑定、逐 chunk 一条 vkCmdDrawIndexedIndirectCount。chunk 数量上来之后这些
// 绑定本身就是开销，也没法合并成一条 multdraw。
//
// 这里把所有 chunk 网格拷进同一份大缓冲的固定槽位：绘制侧只绑一次 VBO/IBO，
// 再用一条 vkCmdDrawIndexedIndirect 画完全部 chunk（每个 chunk 的命令用
// firstIndex/vertexOffset 指向自己的区间）。
//
// 槽位固定大小：流送 chunk 的配置一致（32 单位 / 33x33 / 3 级 LOD，单块约
// 100 KB 顶点 + 50 KB 索引），槽位给到 160 KB / 80 KB 留足余量。超出槽位的
// chunk 会让 adopt() 失败，调用方退回逐 chunk 绑定的旧路径。
//
// 每个 chunk 的世界平移由顶点着色器按 set 3 的 chunk 实例表施加，所以这里只做
// 原样拷贝，不做任何顶点变换。
class TerrainMeshArena
{
public:
    static constexpr uint32_t kSlotCount = 96;
    // The vertex base passed to vkCmdDrawIndexedIndirect is measured in vertices,
    // so a slot must hold a whole number of vertices or every chunk after the
    // first one starts at a fractional offset inside its own slice.
    static constexpr uint32_t kVertexStride = 44u; // pos3 + color3 + normal3 + uv2
    static constexpr uint32_t kSlotVertexCount = 4096u;
    static constexpr uint32_t kSlotVertexBytes = kSlotVertexCount * kVertexStride;
    static constexpr uint32_t kSlotIndexBytes = 80u * 1024u;
    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;
    static constexpr uint32_t kVertexFloats = 11u;

    explicit TerrainMeshArena(Device& device);
    ~TerrainMeshArena();

    TerrainMeshArena(const TerrainMeshArena&) = delete;
    TerrainMeshArena& operator=(const TerrainMeshArena&) = delete;

    bool isAvailable() const { return available; }
    uint32_t freeSlotCount() const;
    bool hasFreeSlot() const { return freeSlotCount() != 0u; }

    // Copies the model's complete vertex/index buffers into one free slot.
    // The source buffers must be fence-complete (written by an earlier frame).
    bool adopt(VkCommandBuffer commandBuffer, const Model& model,
               uint32_t& outSlot, std::string& errorMessage);
    void release(uint32_t slot);

    VkBuffer getVertexBuffer() const { return vertexBuffer; }
    VkBuffer getIndexBuffer() const { return indexBuffer; }
    uint32_t getVertexOffset(uint32_t slot) const;
    uint32_t getIndexOffset(uint32_t slot) const;
    uint32_t getAdoptedCount() const { return adoptedCount; }
    uint32_t getVertexBytes() const { return kSlotCount * kSlotVertexBytes; }
    uint32_t getIndexBytes() const { return kSlotCount * kSlotIndexBytes; }

private:
    bool createResources();
    void destroyResources();

    Device& device;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    std::vector<uint8_t> slotUsed;
    uint32_t adoptedCount = 0;
    bool available = false;
};
