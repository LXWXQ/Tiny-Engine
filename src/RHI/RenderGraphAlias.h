#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

// TM-08 第一步：渲染图资源别名化的「只读报告」。
//
// 这里不分配、不绑定任何显存 —— 它只回答两个问题：
//   1) 这一帧里哪些中间目标是「用完就该还」的（瞬时、有写、有生命周期区间）；
//   2) 把它们的生命周期画在 Pass 序号轴上之后，能重排进几块显存、省多少。
//
// 为什么先只出报告：别名化一旦写错，症状是「画面偶尔花屏」而不是崩溃，而且只在
// 特定分辨率 / 特定 Pass 开关组合下出现。先让数字可见、逐对核对，再打开复用。
namespace tiny::aliasreport
{
    // 一个候选资源在本帧的生命周期。Pass 序号是「本帧第几个真正执行的 Pass」，
    // 跳过（setEnabled(false)）的 Pass 不占号 —— 与 lastFrameExecutedPasses 一致。
    struct Resource
    {
        std::string name;
        uint64_t bytes = 0;
        int32_t firstWrite = -1;   // 第一次被写；-1 = 整帧没写过（不能参与别名）
        int32_t lastUse = -1;      // 最后一次被读或被写
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        bool transient = true;
        // false = 外部创建后导入的资源（例如 GBuffer 的附件）：显存不归渲染图管，
        // 别名化在它身上无从谈起，所以只统计不参与。
        bool owned = true;
    };

    // 线性扫描后的一个物理块：容量取放进来的最大那个资源。
    struct Slot
    {
        uint64_t capacity = 0;
        int32_t end = -1;                  // 块内最后一个成员的 lastUse
        std::vector<const Resource*> members;
    };

    void setEnabled(bool on);
    bool enabled();

    // 每帧调用（只在 enabled() 时有用）。只保留最近一帧的清单：
    // 第一帧还在建纹理、最后一帧在收尾，两头都不代表稳态。
    void recordFrame(std::vector<Resource> resources);

    // 退出时打印。没有数据时什么都不做。
    void report();

    bool selfTest();

    // 纯逻辑，report 与 selfTest 共用：按 firstWrite 升序贪心塞进物理块。
    // 规则保守 —— 只有「格式 / 尺寸 / mip / 层数全同」才认为可以落在一起，
    // 因为 Vulkan 的 image 别名要求 image view 的兼容性，跨格式复用得重建视图。
    std::vector<Slot> pack(const std::vector<Resource>& resources);

    // 单张图的字节数（含 mip 链与层数）。未知格式按 4 字节估，报告里标 est。
    uint64_t estimateBytes(VkFormat format, uint32_t width, uint32_t height,
                           uint32_t mipLevels, uint32_t arrayLayers);

    // 人读的单位（"8.0 MiB"）。0 返回 "0"。
    std::string formatBytes(uint64_t bytes);
}
