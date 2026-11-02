#include "Render/CrossQueueProbePass.h"
#include "AssetPaths.h"
#include <iostream>
#include <stdexcept>
#include <vector>

CrossQueueProbePass::CrossQueueProbePass(Device& device, RenderGraph& graph,
                                         DescriptorManager* descriptorManager)
    : device(device), renderGraph(graph), descriptorManager(descriptorManager)
{
}

CrossQueueProbePass::~CrossQueueProbePass()
{
    graphicsPipeline.reset();
    computePipeline.reset();
    if (graphicsPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), graphicsPipelineLayout, nullptr);
    if (computePipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), computePipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
}

void CrossQueueProbePass::init()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::BufferDesc inputDesc;
        inputDesc.name = "TD02.Input.Frame" + std::to_string(frame);
        inputDesc.size = sizeof(uint32_t) * kValueCount;
        inputDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        inputDesc.crossQueue = true;
        inputBuffers[frame] = renderGraph.createBuffer(inputDesc);

        RenderGraph::BufferDesc graphicsResultDesc;
        graphicsResultDesc.name = "TD02.GraphicsResult.Frame" + std::to_string(frame);
        graphicsResultDesc.size = sizeof(ProbeResult);
        graphicsResultDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        graphicsResultDesc.hostVisible = true;
        graphicsResults[frame] = renderGraph.createBuffer(graphicsResultDesc);

        RenderGraph::BufferDesc computeResultDesc;
        computeResultDesc.name = "TD02.ComputeResult.Frame" + std::to_string(frame);
        computeResultDesc.size = sizeof(ProbeResult);
        computeResultDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        computeResultDesc.hostVisible = true;
        computeResults[frame] = renderGraph.createBuffer(computeResultDesc);

        RenderGraph::TextureDesc imageDesc;
        imageDesc.name = "TD02.StorageImage.Frame" + std::to_string(frame);
        imageDesc.format = VK_FORMAT_R32_UINT;
        imageDesc.extent = { kImageSize, kImageSize };
        imageDesc.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        imageDesc.crossQueue = true;
        storageImages[frame] = renderGraph.createTexture(imageDesc);

        RenderGraph::TextureDesc colorDesc;
        colorDesc.name = "TD02.ColorTarget.Frame" + std::to_string(frame);
        colorDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
        colorDesc.extent = { kImageSize, kImageSize };
        colorDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        colorTargets[frame] = renderGraph.createTexture(colorDesc);
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    setLayout = descriptorManager->createSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings, bindings + 3));

    auto writeProbeSet = [&](VkDescriptorSet set, RenderGraph::BufferHandle resultBuffer, uint32_t frame)
    {
        VkDescriptorBufferInfo inputInfo{ renderGraph.getBuffer(inputBuffers[frame]), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo resultInfo{ renderGraph.getBuffer(resultBuffer), 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = VK_NULL_HANDLE;
        imageInfo.imageView = renderGraph.getView(storageImages[frame]);
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &inputInfo;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pBufferInfo = &resultInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = set;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device.getDevice(), 3, writes, 0, nullptr);
    };
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        graphicsSets[frame] = descriptorManager->allocateSet(setLayout);
        computeSets[frame] = descriptorManager->allocateSet(setLayout);
        writeProbeSet(graphicsSets[frame], graphicsResults[frame], frame);
        writeProbeSet(computeSets[frame], computeResults[frame], frame);
    }

    VkPushConstantRange graphicsPushRange{};
    graphicsPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    graphicsPushRange.offset = 0;
    graphicsPushRange.size = sizeof(ProbePush);

    VkPipelineLayoutCreateInfo graphicsLayoutInfo{};
    graphicsLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    graphicsLayoutInfo.setLayoutCount = 1;
    graphicsLayoutInfo.pSetLayouts = &setLayout;
    graphicsLayoutInfo.pushConstantRangeCount = 1;
    graphicsLayoutInfo.pPushConstantRanges = &graphicsPushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &graphicsLayoutInfo, nullptr, &graphicsPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("CrossQueueProbePass: graphics pipeline layout creation failed");

    VkPushConstantRange computePushRange{};
    computePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    computePushRange.offset = 0;
    computePushRange.size = sizeof(ProbePush);

    VkPipelineLayoutCreateInfo computeLayoutInfo{};
    computeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayoutInfo.setLayoutCount = 1;
    computeLayoutInfo.pSetLayouts = &setLayout;
    computeLayoutInfo.pushConstantRangeCount = 1;
    computeLayoutInfo.pPushConstantRanges = &computePushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &computeLayoutInfo, nullptr, &computePipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("CrossQueueProbePass: compute pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, kImageSize, kImageSize);
    config.colorAttachmentFormats = { VK_FORMAT_R8G8B8A8_UNORM };
    config.pipelineLayout = graphicsPipelineLayout;
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    graphicsPipeline = std::make_unique<Pipeline>(device,
        SENTINEL_SHADER_DIR "fullscreen.vert.spv",
        SENTINEL_SHADER_DIR "cross_queue_probe.frag.spv", config);

    computePipeline = std::make_unique<ComputePipeline>(device,
        SENTINEL_SHADER_DIR "cross_queue_probe.comp.spv", computePipelineLayout);
}
void CrossQueueProbePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    if (!enabled)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    const bool computeToGraphicsFrame = (frameInfo.frameNumber % 2u) == 0u;

    auto verifyResult = [&](RenderGraph::BufferHandle handle, uint32_t expectedMagic,
                            bool expectedComputeToGraphics, bool& reported)
    {
        auto* result = static_cast<ProbeResult*>(graph.getBufferMapped(handle));
        if (result == nullptr)
            throw std::runtime_error("CrossQueueProbePass: host-visible result buffer is not mapped");

        graph.invalidateBuffer(handle);
        if (result->magic != expectedMagic)
            return;   // 第一次复用该槽位时还没被 GPU 写过，正常。

        if (static_cast<uint64_t>(result->frameNumber) + kFramesInFlight != frameInfo.frameNumber)
            return;

        const uint32_t expectedDirection = expectedComputeToGraphics ? 0u : 1u;
        if (result->direction != expectedDirection)
            throw std::runtime_error("CrossQueueProbePass: result direction mismatch");

        uint32_t expectedCount = 0;
        uint32_t expectedChecksum = 0;
        if (expectedComputeToGraphics)
        {
            expectedCount = kValueCount;
            for (uint32_t i = 0; i < kValueCount; ++i)
                expectedChecksum += i * 1664525u + result->frameNumber * 1013904223u;
        }
        else
        {
            expectedCount = kImageSize * kImageSize;
            expectedChecksum = expectedCount * (0x10000000u + result->frameNumber);
        }

        if (result->count != expectedCount || result->checksum != expectedChecksum)
            throw std::runtime_error("CrossQueueProbePass: GPU result checksum mismatch");

        if (!reported)
        {
            reported = true;
            std::cout << (expectedComputeToGraphics
                ? "[TD02] compute->graphics verified: frame "
                : "[TD02] graphics->compute verified: frame ")
                << result->frameNumber << " checksum " << result->checksum
                << " count " << result->count << "\n";
        }
    };

    if (computeToGraphicsFrame)
        verifyResult(graphicsResults[frame], kComputeToGraphicsMagic, true, reportedComputeToGraphics);
    else
        verifyResult(computeResults[frame], kGraphicsToComputeMagic, false, reportedGraphicsToCompute);
    if (computeToGraphicsFrame)
    {
        auto& fill = graph.addComputePass("TD02.ComputeFillSSBO");
        fill.writeBuffer(inputBuffers[frame]);
        // The compute descriptor set statically references binding 2 (the
        // storage image), so transition/declare it here as well. Otherwise
        // validation sees the descriptor at layout UNDEFINED on frame 1.
        fill.writeStorage(storageImages[frame]);
        fill.execute([this, frame, frameNumber = static_cast<uint32_t>(frameInfo.frameNumber)](VkCommandBuffer cmd)
        {
            computePipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout,
                0, 1, &computeSets[frame], 0, nullptr);
            ProbePush push{ 0u, frameNumber, kValueCount, 0u };
            vkCmdPushConstants(cmd, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (kValueCount + 63u) / 64u, 1, 1);
        });

        auto& consume = graph.addGraphicsPass("TD02.GraphicsConsumeSSBO");
        consume.readBuffer(inputBuffers[frame],
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        consume.writeBuffer(graphicsResults[frame],
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        consume.writeStorage(storageImages[frame],
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        consume.writeColor(colorTargets[frame], VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        consume.execute([this, frame, frameNumber = static_cast<uint32_t>(frameInfo.frameNumber)](VkCommandBuffer cmd)
        {
            graphicsPipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelineLayout,
                0, 1, &graphicsSets[frame], 0, nullptr);
            ProbePush push{ 0u, frameNumber, kValueCount, 0u };
            vkCmdPushConstants(cmd, graphicsPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        });

        auto& hostBarrier = graph.addGraphicsPass("TD02.GraphicsHostReadBarrier");
        hostBarrier.readBuffer(graphicsResults[frame],
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        return;
    }
    auto& fillImage = graph.addGraphicsPass("TD02.GraphicsFillStorageImage");
    fillImage.writeStorage(storageImages[frame],
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    fillImage.writeColor(colorTargets[frame], VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    fillImage.execute([this, frame, frameNumber = static_cast<uint32_t>(frameInfo.frameNumber)](VkCommandBuffer cmd)
    {
        graphicsPipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelineLayout,
            0, 1, &graphicsSets[frame], 0, nullptr);
        ProbePush push{ 1u, frameNumber, kImageSize * kImageSize, 0u };
        vkCmdPushConstants(cmd, graphicsPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });

    auto& consumeImage = graph.addComputePass("TD02.ComputeConsumeStorageImage");
    consumeImage.readStorage(storageImages[frame],
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    consumeImage.writeStorage(storageImages[frame],
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    consumeImage.writeBuffer(computeResults[frame]);
    consumeImage.execute([this, frame, frameNumber = static_cast<uint32_t>(frameInfo.frameNumber)](VkCommandBuffer cmd)
    {
        computePipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout,
            0, 1, &computeSets[frame], 0, nullptr);
        ProbePush push{ 1u, frameNumber, kImageSize * kImageSize, 0u };
        vkCmdPushConstants(cmd, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
    });

    auto& computeHostBarrier = graph.addComputePass("TD02.ComputeHostReadBarrier");
    computeHostBarrier.readBuffer(computeResults[frame],
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}