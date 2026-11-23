#include "Render/ClusterBuildPass.h"
#include "AssetPaths.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInfo.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/ClusteredLighting.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
    // 四段里最靠后的那一字节。渲染图的 normalizeBufferRange 会拿它当上界校验，
    // 填小了会抛「buffer range exceeds buffer bounds」。
    VkDeviceSize clusterTablesExtent(const DescriptorManager::ClusterBinding& binding)
    {
        const VkDeviceSize extents[4] = {
            binding.paramsOffset + binding.paramsSize,
            binding.rangesOffset + binding.rangesSize,
            binding.indicesOffset + binding.indicesSize,
            binding.lightsOffset + binding.lightsSize,
        };
        return *std::max_element(std::begin(extents), std::end(extents));
    }
}

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

    // ---- 把图外的暂存区大块登记进图 ----
    // 只在底层 VkBuffer 变了才重新登记：beginFrame 不清 buffers 表，
    // 每帧 import 一次会让条目无限增长（而且每帧都是一份全新的状态，等于没记住）。
    if (!clusterBuffer.valid() || clusterBufferVk != binding.buffer)
    {
        RenderGraph::BufferDesc desc{};
        desc.name = "Scratch.ClusterTables";
        desc.size = clusterTablesExtent(binding);
        desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        // 两个队列都会碰这一块，必须声明 crossQueue：图据此允许跨队列访问，
        // 并且知道不需要 EXCLUSIVE 的所有权转移（底层是 CONCURRENT）。
        desc.crossQueue = true;
        clusterBuffer = graph.importBuffer(desc, binding.buffer);
        clusterBufferVk = binding.buffer;
    }

    const RenderGraph::BufferRange rangesRange{ binding.rangesOffset, binding.rangesSize };
    const RenderGraph::BufferRange indicesRange{ binding.indicesOffset, binding.indicesSize };

    // ---- ① 建簇：异步 compute 流 ----
    // params / lights 两张表是 host 写的，不在这里声明；被 GPU 重写的两张表
    // 各声明一次写，图就会把它们记成"compute 写过、待片元读"。
    auto& build = graph.addComputePass("Clusters.Build");
    build.writeBuffer(clusterBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, rangesRange);
    build.writeBuffer(clusterBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, indicesRange);
    build.execute([this, globalSet](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
            0, 1, &globalSet, 0, nullptr);

        const uint32_t groups = (ClusteredLighting::kClusterCount + 63u) / 64u;
        vkCmdDispatch(cmd, groups, 1, 1);
    });

    // ---- ② 片元侧的读：只声明、无回调 ----
    // TD-02 探针里的 GraphicsHostReadBarrier 证明了这种零附件零回调的 Pass 合法：
    // 它不录制任何命令，但会把「图形流要读这两段」记进图里，于是图推出
    // compute->graphics 依赖，并在提交时插 timeline 信号 + 屏障。
    auto& sync = graph.addGraphicsPass("Clusters.ReadSync");
    sync.readBuffer(clusterBuffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, rangesRange);
    sync.readBuffer(clusterBuffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, indicesRange);
}