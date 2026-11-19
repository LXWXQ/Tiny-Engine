#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

// ===========================================================================
// 管线统计与「状态字」（TM-06 第一步）
//
// 为什么先统计再改结构：本项目全部管线都在启动时一次性建好，各 Pass 把
// Pipeline 当成员持有，运行时没有「按状态取管线」这条路径。在这种结构下，
// 「按需建 + 状态缓存」能不能省时间，完全取决于两个数字：
//   1. 启动到底建了多少条、每条花多少毫秒；
//   2. 这些管线里有多少条的状态其实完全一样（可以合并，或至少说明
//      "按需建"永远不会有第二次命中）。
// 没有这两个数字就动手，只是把一个直白的构造顺序换成一整套没人用的缓存。
//
// 关闭时零开销：`--stats-pipelines` 没开时只累加几个整数，不建表、不拼字符串
// （判重表只在 setEnabled(true) 之后才填），report() 也只由 finalizeDiagnostics
// 显式调用一次。
// ===========================================================================
namespace tiny::pipelinestats
{
    // 一条图形管线的「可比较状态」：只收真正会改变驱动派生代码的字段。
    // viewport / scissor 不在此列 —— 它们是动态状态，改它们不需要重建管线。
    struct GraphicsStateKey
    {
        VkPrimitiveTopology topology;
        VkCullModeFlags     cullMode;
        VkFrontFace         frontFace;
        VkCompareOp         depthCompare;
        bool                depthTest;
        bool                depthWrite;
        bool                depthBias;
        bool                blend;          // attachment 0 是否开混合
        VkBlendFactor       srcColorBlend;
        VkBlendFactor       dstColorBlend;
        VkBlendOp           colorBlendOp;
        VkBlendFactor       srcAlphaBlend;
        VkBlendFactor       dstAlphaBlend;
        VkBlendOp           alphaBlendOp;
        VkFormat            colorFormat;    // 0 号颜色附件
        VkFormat            depthFormat;
        uint32_t            colorAttachmentCount;
        // 管线布局也是状态的一部分：两条状态相同但布局不同的管线不能互换，
        // 少算这一项就会把真正不同的管线报成"完全重复"。这里只当不透明句柄用。
        uint64_t            pipelineLayout;
        uint32_t            bindingCount;   // 顶点输入布局
        uint32_t            attributeCount;
    };

    // 默认构造出来的 key 全零（blend/混合因子也都是 0 = 合法枚举的起点之外），
    // 调用方必须显式填满每个字段 —— 用 zero-init 保证"漏填"是稳定的、可复现的
    // 一个值，而不是随机栈内容。
    GraphicsStateKey makeEmptyKey();

    // 逐字段喂 FNV-1a，不对整个结构体做 memcmp：
    //   1. 结构体里有 padding，memcmp 会把未初始化的字节算进去，同一条状态
    //      换个构造顺序就得到两个 hash；
    //   2. 字段宽度不一致，直接哈希内存还会受大小端影响。
    // 参考实现用 union{位域, uint32_t words[4]} 一次比 16 字节是同一个思路，
    // 但前提是状态被压缩到定长无 padding 的表示——这里字段直接来自
    // Vk*CreateInfo，先按字段哈希更省事。
    uint64_t hashGraphicsState(const GraphicsStateKey& key);

    // 判重表只在开启时维护；关闭时 recordGraphics 只做加减。
    void setEnabled(bool value);
    bool enabled();

    void recordGraphics(const GraphicsStateKey& key, const char* vertPath, const char* fragPath, double milliseconds);
    void recordCompute(const char* compPath, double milliseconds);
    void recordShaderModule(uint64_t spirvBytes);
    void recordFailure();
    void setCacheLoadedBytes(uint64_t bytes);

    // 打印一段摘要 + 重复状态示例。只在 --stats-pipelines 下调用。
    void report();

    // 不需要 Vulkan 设备：只验状态字对字段变化敏感、对重复构造稳定。
    bool selfTest();
}
