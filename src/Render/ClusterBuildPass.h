#pragma once

#include "Render/IRenderPass.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

class Device;
class RenderGraph;
class DescriptorManager;
class ComputePipeline;

// TM-13 第二步：在图形命令流里跑一次 compute dispatch，把聚簇的
// ranges / indices 两张表整帧重算出来（替代 CPU 侧的 ClusteredLighting::build）。
//
// 为什么不用 RenderGraph::addComputePass：
//   那会把 dispatch 录进**异步 compute** 命令流。聚簇三张表（params / ranges /
//   indices）走的是暂存区里的普通 buffer，没有在渲染图里登记，跨队列的
//   timeline 依赖就建立不起来 —— 这正是 TM-09（跨队列 EXCLUSIVE）判定不做时
//   撞到的那面墙。放在图形流里只需要一条 buffer 屏障，边界清楚得多。
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
};
