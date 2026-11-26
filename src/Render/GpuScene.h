#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "Core/FrameInFlight.h"

class Device;

namespace tiny
{

// T-08：持久 GPU 场景（先只读骨架）。
//
// 现在的提交路径每帧把「结构」与「变换」一起重新摊平上传：一万个立方体约 1.4 MB/帧，
// `flattenUs` 近 19 ms。P2 要做的是把两件事拆开 ——
//   1. 原语（模型 + 子网格 + 合并后的绘制参数）在 GPU 上有稳定编号，跨帧不变；
//   2. 实例（世界矩阵 + 它属于哪个原语）在 GPU 上也有稳定槽位，只有变换变了才重传。
//
// 这个类先只做「有稳定编号的持久缓冲」，**不接任何消费者**：
// 剔除仍走原来的每帧上传路径，唯一目的是验证编号稳定、脏区间上传量能降到 0。
// 接消费者是 P2 的下一步（`scene_cull.comp` 改读这里的实例缓冲）。
class GpuScene
{
public:
    // 一个原语 = 一个可独立出绘制命令的 (模型, 子网格)。
    // 与将来的 shader 结构逐字节对齐：64 字节。
    struct Primitive
    {
        glm::vec4 boundsMin;
        glm::vec4 boundsMax;
        glm::uvec4 draw;   // x=firstIndex y=indexCount z=vertexOffset w=materialSlot
    };

    // 一个实例 = 世界矩阵 + 法线矩阵 + 它属于哪个原语 + 上一帧世界矩阵。208 字节。
    struct Instance
    {
        glm::mat4 transform;
        glm::mat4 normal;
        glm::uvec4 ids;    // x=primitiveIndex y=instanceInPrimitive z=flags w=保留
        glm::mat4 previousTransform; // 上一次结构上传时的世界矩阵（P2-2 收尾）
    };

    struct Stats
    {
        uint64_t frames = 0;
        uint64_t primitives = 0;
        uint64_t instances = 0;
        uint64_t dirtyFrames = 0;      // 有过上传的帧数
        uint64_t uploadedBytes = 0;    // 累计上传字节
        uint64_t uploadCalls = 0;
        uint64_t capacityBytes = 0;
        uint64_t resizes = 0;
        uint64_t instancesPerFrame = 0;  // 最近一帧
    };

    explicit GpuScene(Device& device);
    ~GpuScene();

    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void setEnabled(bool value) { enabled = value; }
    bool isEnabled() const { return enabled; }

    // 帧首：清本帧的脏范围与计数。
    void beginFrame();

    // 按 key 取稳定编号（不存在则追加，返回新编号）。key 由调用方保证全局唯一。
    uint32_t ensurePrimitive(uint64_t key, const Primitive& primitive);
    uint32_t ensureInstance(uint64_t key, const Instance& instance);

    // 把本帧的脏区间上传到设备缓冲。必须录在图形命令缓冲上（有 transfer 能力）。
    void flush(VkCommandBuffer commandBuffer);

    // 一帧结束后调用：更新统计（脏帧计数、每帧实例数）。
    void endFrame();

    void report() const;
    bool selfTest();

    const Stats& getStats() const { return stats; }

    // 一致性检查用：最近一帧写入的原语/实例数与几何路径的组/实例数应当相等。
    uint32_t getPrimitiveCount() const { return static_cast<uint32_t>(primitives.size()); }
    uint32_t getInstanceCount() const { return static_cast<uint32_t>(instances.size()); }

private:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize capacity = 0;
    };

    void ensureCapacity(Buffer& buffer, VkDeviceSize needed, VkBufferUsageFlags usage);
    void destroyBuffer(Buffer& buffer);

    Device& device;
    bool enabled = true;

    Buffer primitiveBuffer;
    Buffer instanceBuffer;

    std::vector<Primitive> primitives;
    std::vector<Instance> instances;
    std::unordered_map<uint64_t, uint32_t> primitiveIndex;
    std::unordered_map<uint64_t, uint32_t> instanceIndex;

    // 脏区间按元素编号记：一张连续上传比逐元素省得多，也够当前场景用。
    uint32_t dirtyPrimitiveBegin = UINT32_MAX;
    uint32_t dirtyPrimitiveEnd = 0;
    uint32_t dirtyInstanceBegin = UINT32_MAX;
    uint32_t dirtyInstanceEnd = 0;

    Stats stats;
    bool reportedFirstFrame = false;
};

} // namespace tiny