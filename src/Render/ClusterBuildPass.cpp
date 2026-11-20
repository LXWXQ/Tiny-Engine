#include "Render/ClusterBuildPass.h"
#include "AssetPaths.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInfo.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/ClusteredLighting.h"

#include <stdexcept>
#include <string>

ClusterBuildPass::ClusterBuildPass(Device& device, DescriptorManager* descriptorManager,
    VkDescriptorSetLayout globalSetLayout)
    : device(device), descriptorManager(descriptorManager), globalSetLayout(globalSetLayout)
{
    // 只挂全局 set：四张聚簇表都在 set 0 的 binding 15~18 上，
    // 与 lighting.frag 用的是同一份描述符，省一次布局与一次绑定。
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &this->globalSetLayout;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create cluster build pipeline layout!");

    const std::string shaderPath = std::string(TINY_SHADER_DIR) + "cluster_build.comp.spv";
    pipeline = std::make_unique<ComputePipeline>(device, shaderPath, pipelineLayout);
}

ClusterBuildPass::~ClusterBuildPass()
{
    pipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
}

void ClusterBuildPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    if (!enabled || pipeline == nullptr || descriptorManager == nullptr)
        return;

    const DescriptorManager::ClusterBinding binding = descriptorManager->lastClusterBinding();
    if (binding.buffer == VK_NULL_HANDLE || binding.indicesSize == 0 || binding.rangesSize == 0)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    VkDescriptorSet globalSet = descriptorManager->getGlobalSet(frame);
    if (globalSet == VK_NULL_HANDLE)
        return;

    // 三张表都由这一次 dispatch 重写，所以声明成"图里的写者"没有必要 ——
    // 它们不在渲染图的资源表里。屏障在 dispatch 之后自己发一条。
    auto& pass = graph.addGraphicsPass("Clusters.Build");
    pass.execute([this, frame, globalSet, binding](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
            0, 1, &globalSet, 0, nullptr);

        const uint32_t groups =
            (ClusteredLighting::kClusterCount + 63u) / 64u;
        vkCmdDispatch(cmd, groups, 1, 1);

        // compute 写完 -> 片元读。范围只覆盖两张"被 GPU 写过"的表，
        // params / lights 是 host 写的，不需要这条屏障。
        VkBufferMemoryBarrier2 barriers[2]{};
        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = binding.buffer;
        barriers[0].offset = binding.rangesOffset;
        barriers[0].size = binding.rangesSize;

        barriers[1] = barriers[0];
        barriers[1].offset = binding.indicesOffset;
        barriers[1].size = binding.indicesSize;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 2;
        dependency.pBufferMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(cmd, &dependency);
    });
}
