#include "Render/SubresourceProbePass.h"
#include <iostream>

SubresourceProbePass::SubresourceProbePass(Device& device, RenderGraph& renderGraph)
    : device(device), renderGraph(renderGraph)
{
}

void SubresourceProbePass::init()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "TD05.SubresourceProbe.Frame" + std::to_string(frame);
        desc.format = VK_FORMAT_R8G8B8A8_UNORM;
        desc.extent = { 8, 8 };
        desc.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        desc.mipLevels = 2;
        desc.arrayLayers = 2;
        testImages[frame] = renderGraph.createTexture(desc);

        RenderGraph::BufferDesc bufferDesc;
        bufferDesc.name = "TD05.SubresourceProbe.Buffer.Frame" + std::to_string(frame);
        bufferDesc.size = 256;
        bufferDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        testBuffers[frame] = renderGraph.createBuffer(bufferDesc);
    }
}

void SubresourceProbePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    if (currentMode == Mode::Disabled)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    const RenderGraph::TextureHandle image = testImages[frame];
    const RenderGraph::SubresourceRange mip0Layer0{ 0, 1, 0, 1 };
    const RenderGraph::SubresourceRange mip1Layer0{ 1, 1, 0, 1 };
    const RenderGraph::SubresourceRange mip1Layer1{ 1, 1, 1, 1 };

    auto& write0 = graph.addComputePass("TD05.WriteMip0Layer0");
    write0.writeStorage(image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, mip0Layer0);

    if (currentMode == Mode::SemanticError)
    {
        auto& badRead = graph.addComputePass("TD05.ReadMip1Layer0");
        badRead.readStorage(image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT, mip1Layer0);
        return;
    }

    auto& read0 = graph.addComputePass("TD05.ReadMip0Layer0");
    read0.readStorage(image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, mip0Layer0);

    auto& write1 = graph.addComputePass("TD05.WriteMip1Layer1");
    write1.writeStorage(image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, mip1Layer1);

    auto& read1 = graph.addComputePass("TD05.ReadMip1Layer1");
    read1.readStorage(image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, mip1Layer1);

    const RenderGraph::BufferHandle buffer = testBuffers[frame];
    const RenderGraph::BufferRange firstHalf{ 0, 128 };
    const RenderGraph::BufferRange secondHalf{ 128, 128 };

    auto& writeBuffer0 = graph.addComputePass("TD05.WriteBufferFirstHalf");
    writeBuffer0.writeBuffer(buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, firstHalf);
    auto& readBuffer0 = graph.addComputePass("TD05.ReadBufferFirstHalf");
    readBuffer0.readBuffer(buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, firstHalf);

    auto& writeBuffer1 = graph.addComputePass("TD05.WriteBufferSecondHalf");
    writeBuffer1.writeBuffer(buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, secondHalf);
    auto& readBuffer1 = graph.addComputePass("TD05.ReadBufferSecondHalf");
    readBuffer1.readBuffer(buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, secondHalf);

    if (!reportedPositive)
    {
        reportedPositive = true;
        std::cout << "[TD05] subresource probe active:"
                  << " write/read mip0-layer0 + mip1-layer1,"
                  << " and buffer [0,128) + [128,256);"
                  << " other subresources are untouched\n";
    }
}