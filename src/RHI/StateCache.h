#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace tiny::statecache
{
// 每帧的管线绑定缓存（T-09）。
//
// vkCmdBindPipeline 是"设置状态"而不是"执行"：重复绑定同一个管线对渲染结果零影响，
// 而 VkCommandBuffer 会一直记住上次绑定的管线。所以"同一条命令缓冲里连续绑定同一个
// 管线"可以整条指令省掉。这条结论只对同一条命令缓冲成立，因此缓存按命令缓冲分别记账，
// 并在每帧开头重置（命令缓冲每帧 vkResetCommandBuffer，状态归零）。
//
// 为什么值得做：`--stats-frame` 量出 23.5 次绑定里有 10.5 次（44.7%）是绑给同一个管线，
// 集中在几何 Pass 的"地形合批 → GPU 场景合批 → 体素合批"这三段（同一个 geometryPipeline）。
// 只缓存这两条命令缓冲：它们是整帧唯一的图形/计算流。其它命令缓冲（地形构建、
// 纹理上传、回读）每次录制都 vkResetCommandBuffer 重来，记在缓存里会让下一次
// 绑定被误判成"已经绑着"—— 实测就是这么崩的（terrain-render，0xC0000005）。
void beginFrame(VkCommandBuffer graphicsCommandBuffer, VkCommandBuffer computeCommandBuffer);

// 声明"这条命令缓冲每帧/每次使用都会先 reset，我现在从干净状态开始录制"。
// 引擎的两条主缓冲由 beginFrame 自动登记；地形构建、纹理上传这类一次性缓冲
// 必须在 vkBeginCommandBuffer 之后自己登记一次，否则缓存会误以为"上次绑的还在"。
void trackCommandBuffer(VkCommandBuffer commandBuffer);

// 整体回退开关（--no-pipeline-cache）：关掉之后每次绑定都照发，行为与 T-09 之前一致。
void setEnabled(bool enabled);
bool isEnabled();

// 管线被重建/销毁时调用：VkPipeline 的句柄值可能被驱动复用，缓存里的旧值就不可信了。
void invalidateAll();

void bindGraphics(VkCommandBuffer commandBuffer, VkPipeline pipeline);
void bindCompute(VkCommandBuffer commandBuffer, VkPipeline pipeline);

// P3-2：顶点/索引缓冲的重复绑定同样可以省。引擎里所有"绑 0 号绑定、偏移 0"的
// 单缓冲调用都走这里；一次绑多个绑定的调用（编辑器叠加层）不要走，缓存只记一条。
void bindVertexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset);
void bindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                     VkIndexType indexType);

struct Stats
{
    uint64_t bindCalls = 0;  // 调用方请求绑定几次
    uint64_t issued = 0;     // 真正发出去的 vkCmdBindPipeline
    uint64_t skipped = 0;    // 因为"已经绑着同一个"而省掉的
    // 未受管命令缓冲（地形构建 / 纹理上传 / 回读）上的绑定：每次都必须发。
    // 记下来是为了让 bindCalls = 受管部分 + 未受管部分能对上账。
    uint64_t unmanaged = 0;

    // P3-2：顶点/索引缓冲的账（和管线分开算，管线的 bindCalls 里不含它们）。
    uint64_t vertexBindCalls = 0;
    uint64_t vertexIssued = 0;
    uint64_t vertexSkipped = 0;
    uint64_t indexBindCalls = 0;
    uint64_t indexIssued = 0;
    uint64_t indexSkipped = 0;
};

const Stats& frameStats();
uint64_t totalIssued();
uint64_t totalSkipped();

void report();
bool selfTest();
}
