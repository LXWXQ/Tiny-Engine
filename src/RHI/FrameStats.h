#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// ===========================================================================
// T-10：每帧结构计数器（P0 度量基线）
//
// 为什么要有这个文件：改造计划里每个验收门槛都是「相对基线下降 X%」，而在这之前
// 引擎没有任何地方统计过这两类最贵的东西：
//   1. 屏障条数 —— RenderGraph 现在每个 mip/layer 各发一条 vkCmdPipelineBarrier2，
//      T-03（屏障批处理）能省多少只能靠这个数字证明，凭感觉优化等于赌博；
//   2. 管线状态切换 —— 每次 bind 记一笔，其中「和上一次绑的是同一条」的那部分是
//      T-09（MeshDrawCommand 状态缓存）能消掉的上限。上限只有 3% 就不值得做。
//
// 为什么不做成 RenderGraph 的成员：管线 bind 发生在 RHI 的 Pipeline 里，屏障发生在
// RenderGraph 里，两者必须落在同一份计数里才有意义（同一帧、同一分母）。所以放到
// 一个独立的小模块，谁也不拥有谁。
//
// 关闭时零开销：计数全是整数 ++，不做字符串、不做表；report() 只在 --stats-frame 下
// 由退出路径调用一次。
// ===========================================================================
namespace tiny::framestats
{
    // 帧边界：beginFrame 清本帧计数并把「上一次绑的是谁」复位；endFrame 把本帧计数
    // 累加进总计、frames+1。由 RenderGraph::beginFrame / endFrame 调用 —— 渲染图是
    // 每帧唯一必然经过的地方，不需要额外的调用点。
    void beginFrame();
    void endFrame();

    void countPass(bool recorded);
    void countBarrierCall(uint32_t entries);
    void countBarrierSkippedSameState();
    void countBarrierSkippedCrossQueue();
    // compute=true / false 各自记「上一次绑的是谁」：两条命令流的状态缓存互相独立，
    // 混在一起会把「图形与计算交替绑定」误判成冗余。
    void countPipelineBind(VkPipeline pipeline, bool compute);

    void setEnabled(bool value);
    bool enabled();

    // 退出时打印每帧均值（--stats-frame）。
    void report();

    // 不需要 Vulkan 设备：只验重复绑定判定、帧边界复位、以及自检不改动真实总计。
    bool selfTest();
}