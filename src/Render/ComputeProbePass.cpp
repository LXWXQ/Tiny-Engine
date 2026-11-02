#include "Render/ComputeProbePass.h"
#include "AssetPaths.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>

ComputeProbePass::ComputeProbePass(Device& device, RenderGraph& graph,
                                   DescriptorManager* descriptorManager)
    : device(device), renderGraph(graph), descriptorManager(descriptorManager)
{
}

ComputeProbePass::~ComputeProbePass()
{
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
}

void ComputeProbePass::init()
{
    // work buffer 只在 GPU 上读写；result buffer 是 host-visible，用来验证
    // compute 真的执行完成，以及 CPU 读回路径没有把内存一致性问题藏起来。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::BufferDesc workDesc;
        workDesc.name = "ComputeProbe.Work.Frame" + std::to_string(frame);
        workDesc.size = sizeof(uint32_t) * kProbeValueCount;
        workDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        workBuffers[frame] = renderGraph.createBuffer(workDesc);

        RenderGraph::BufferDesc resultDesc;
        resultDesc.name = "ComputeProbe.Result.Frame" + std::to_string(frame);
        resultDesc.size = sizeof(ProbeResult);
        resultDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        resultDesc.hostVisible = true;
        resultBuffers[frame] = renderGraph.createBuffer(resultDesc);
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    setLayout = descriptorManager->createSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(), bindings.end()));

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        descriptorSets[frame] = descriptorManager->allocateSet(setLayout);

        VkDescriptorBufferInfo workInfo{ renderGraph.getBuffer(workBuffers[frame]), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo resultInfo{ renderGraph.getBuffer(resultBuffers[frame]), 0, VK_WHOLE_SIZE };

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &workInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &resultInfo;

        vkUpdateDescriptorSets(device.getDevice(), static_cast<uint32_t>(writes.size()),
            writes.data(), 0, nullptr);
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ProbePush);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("ComputeProbePass: pipeline layout creation failed");

    pipeline = std::make_unique<ComputePipeline>(device,
        SENTINEL_SHADER_DIR "compute_probe.comp.spv", pipelineLayout);
}

void ComputeProbePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    if (!enabled)
    {
        static bool reportedDisabled = false;
        if (!reportedDisabled)
        {
            reportedDisabled = true;
            std::cout << "[TD07] ComputeProbe disabled: no probe dispatches or CPU readback\n";
        }
        return;
    }

    const uint32_t frame = frameInfo.frameInFlightIndex;

    // Renderer::beginFrame 已经等过这个 slot 的 compute timeline，因此这里读
    // 上一轮 GPU 写入的 result buffer 是安全的。invalidate 处理非 coherent 内存。
    if (auto* result = static_cast<ProbeResult*>(graph.getBufferMapped(resultBuffers[frame])))
    {
        graph.invalidateBuffer(resultBuffers[frame]);
        const bool matchesExpectedPreviousFrame =
            result->magic == kProbeMagic &&
            result->count == kProbeValueCount &&
            result->frameNumber + kFramesInFlight == frameInfo.frameNumber;

        if (matchesExpectedPreviousFrame && !reportedFirstValidResult)
        {
            reportedFirstValidResult = true;
            std::cout << "[D5] async compute probe verified: frame " << result->frameNumber
                      << " checksum " << result->checksum << " count " << result->count << "\n";
        }
    }

    auto& fillPass = graph.addComputePass("ComputeProbe.Fill");
    fillPass.writeBuffer(workBuffers[frame]);

    // 反面用例：故意跳过写入者，验证 D-5 新增的 buffer 语义校验真的会拦住
    // "checksum 读未初始化 work buffer"。仅用于自动化回归，不参与正常渲染。
    static const bool simulateSkipFill = (std::getenv("SENTINEL_SIMULATE_COMPUTE_SKIP") != nullptr);
    if (simulateSkipFill)
        fillPass.setEnabled(false);
    fillPass.execute([this, frame, frameNumber = frameInfo.frameNumber](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
            0, 1, &descriptorSets[frame], 0, nullptr);

        ProbePush push{ 0u, static_cast<uint32_t>(frameNumber), kProbeValueCount, 0u };
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (kProbeValueCount + 63u) / 64u, 1, 1);
    });

    auto& checksumPass = graph.addComputePass("ComputeProbe.Checksum");
    checksumPass.readBuffer(workBuffers[frame]);
    checksumPass.writeBuffer(resultBuffers[frame]);
    checksumPass.execute([this, frame, frameNumber = frameInfo.frameNumber](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
            0, 1, &descriptorSets[frame], 0, nullptr);

        ProbePush push{ 1u, static_cast<uint32_t>(frameNumber), kProbeValueCount, 0u };
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
    });

    // 让 device 写入对 host 可见。下一轮 CPU 读取前还有 compute timeline wait。
    auto& hostBarrier = graph.addComputePass("ComputeProbe.HostReadBarrier");
    hostBarrier.readBuffer(resultBuffers[frame],
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
