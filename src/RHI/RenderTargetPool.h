#pragma once

#include "RHI/VmaConfig.h"
#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Device;

// ===========================================================================
// T-04：跨帧渲染目标池
//
// 为什么要有这个文件：--resize-test 实测（60 帧、6 次尺寸变化）总共创建了 424 张图，
// 基线是 160 张，也就是「一次尺寸变化约 44 张」—— 44 次 vkCreateImage + 44 次
// vmaAllocateMemory + 44 次 vkCreateImageView，全发生在 vkDeviceWaitIdle 之后的那一帧。
// 尺寸来回切换时这些图往往只是被换下来，不是真的需要新显存。
//
// 池的键是「格式 + 精确尺寸 + usage + mip/layer + createFlags」。尺寸必须精确相等：
// VkImageView 没有 extent 概念，拿一张大图当小图用会让采样按大图的归一化坐标算，
// 视觉上是缩放错误，不是浪费一点显存的问题。
//
// 释放不立即回收：图可能还在 GPU 上被上一帧的命令引用，所以释放时记下帧号，要过
// kRetireFrames 帧才重新进入可分配状态。resize 路径在重建前已经 vkDeviceWaitIdle，
// 理论上可以立即复用，但这里仍然走同一条退休路径 —— 一条规则比两条规则少一类 bug。
//
// 空闲图不是无限留：超过 kMaxIdleBytes 就按最久未用淘汰，否则来回切几个尺寸
// 就能把显存吃满。
// ===========================================================================
namespace tiny
{
    class RenderTargetPool
    {
    public:
        struct Desc
        {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{};
            VkImageUsageFlags usage = 0;
            VkImageCreateFlags flags = 0;
            uint32_t mipLevels = 1;
            uint32_t arrayLayers = 1;
            bool crossQueue = false;
            bool isDepth = false;
        };

        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
        };

        struct Stats
        {
            uint64_t hits = 0;
            uint64_t misses = 0;
            uint64_t created = 0;
            uint64_t destroyed = 0;
            uint64_t evicted = 0;
        };

        static constexpr uint64_t kRetireFrames = 2;
        static constexpr VkDeviceSize kMaxIdleBytes = 256ull * 1024ull * 1024ull;

        explicit RenderTargetPool(Device& device);
        ~RenderTargetPool();

        RenderTargetPool(const RenderTargetPool&) = delete;
        RenderTargetPool& operator=(const RenderTargetPool&) = delete;

        // 由 RenderGraph::beginFrame 调用：推进帧号并淘汰超预算的空闲图。
        void beginFrame(uint64_t frameNumber);

        // 命中空闲条目就复用（view 一起复用），否则新建。
        Image acquire(const Desc& desc, const std::string& name);

        // 交回池里；image 为空句柄时是空操作。
        void release(const Desc& desc, const Image& image);

        void setEnabled(bool value) { enabled = value; }
        bool isEnabled() const { return enabled; }

        const Stats& getStats() const { return stats; }
        VkDeviceSize getIdleBytes() const { return idleBytes; }
        size_t getIdleCount() const { return idle.size(); }

        void report() const;
        void noteCreated(const std::string& name);

        // 纯逻辑自检：键比较、字节估算、退休窗口。不需要 Vulkan 设备。
        static bool selfTest();

    private:
        struct IdleEntry
        {
            Desc desc;
            Image image;
            uint64_t releasedFrame = 0;
        };

        static bool sameDesc(const Desc& a, const Desc& b);
        static VkDeviceSize bytesPerPixel(VkFormat format);
        static VkDeviceSize estimateBytes(const Desc& desc);

        Image createImage(const Desc& desc, const std::string& name);
        void destroyImage(const Image& image);
        void evictOverBudget();

        Device& device;
        bool enabled = false;   // 默认关：--rt-pool 才开（见 docs 里 T-04 的结论）
        uint64_t frameNumber = 0;
        VkDeviceSize idleBytes = 0;
        std::vector<IdleEntry> idle;
        // 统计用：按名字记创建次数，报告时给 top 8。
        struct NameCount { std::string name; uint64_t created = 0; };
        std::vector<NameCount> createdByName;
        Stats stats;
    };
}
