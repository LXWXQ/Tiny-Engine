#pragma once

#include "Render/IRenderPass.h"
#include "RHI/RenderGraph.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

class Device;
class DescriptorManager;
class ComputePipeline;

// TM-13 第二步：把聚簇的 ranges / indices 两张表整帧重算出来
// （替代 CPU 侧的 ClusteredLighting::build）。
//
// T-02：dispatch 重新回到**异步 compute** 命令流。之前判定不能这么做，理由是
// 「四张表走暂存区的普通 buffer，没有在渲染图里登记，跨队列的 timeline 依赖建立
// 不起来」—— 那面墙有两块砖，现在两块都拆了：
//   1. 暂存区的大块改成 VK_SHARING_MODE_CONCURRENT（ScratchArena::createSlotBuffer），
//      于是不需要 ownership transfer —— TM-09 判掉的是 EXCLUSIVE 转移，不是多提交；
//   2. 大块用 RenderGraph::importBuffer 登记进图，dispatch 声明 writeBuffer，
//      片元侧的读由一条只声明、无回调的图形 Pass 声明 readBuffer。
// 图据此推出 compute->graphics 依赖，Renderer 在提交时插一条 timeline 信号完成排序
// （Renderer::endFrame 里 computeWaitsForGraphics 那条路径早就在跑，只是没人用过）。
//
// 记住 import 出来的 handle：beginFrame 不清 buffers 表，每帧 import 一次会让条目
// 无限增长；只有底层 VkBuffer 换了（暂存区扩容）才重新登记。
class ClusterBuildPass : public IRenderPass
{
public:
    ClusterBuildPass(Device& device, DescriptorManager* descriptorManager,
        VkDescriptorSetLayout globalSetLayout);
    ~ClusterBuildPass();

    void setEnabled(bool value) { enabled = value; }
    bool isEnabled() const { return enabled; }

    // IRenderPass：管线在构造里就建好（不依赖渲染图创建的资源），
    // 所以 init 是空的 —— 真正的活在 addToGraph 里。
    void init() override {}
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Clusters.Build"; }

private:
    Device& device;
    DescriptorManager* descriptorManager = nullptr;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> pipeline;
    bool enabled = false;

    // 图外资源的登记结果。clusterBufferVk 是"上次登记时那块 VkBuffer"，
    // 与本次 binding.buffer 不同就说明暂存区扩容换过块，必须重新 import。
    RenderGraph::BufferHandle clusterBuffer{};
    VkBuffer clusterBufferVk = VK_NULL_HANDLE;
};