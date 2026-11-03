#include "GeometryPass.h"
#include "Core/DescriptorManager.h"
#include <iostream>

GeometryPass::GeometryPass(Device& device, const std::array<GBuffer*, kFramesInFlight>& gBuffers,
                           RenderGraph& graph, RenderResources& resources,
                           VkDescriptorSetLayout globalSetLayout,
                           VkDescriptorSetLayout materialSetLayout,
                           VkDescriptorSetLayout terrainSetLayout,
                           TerrainMeshArena* terrainMeshArena)
    : vulkanDevice(device), gBuffers(gBuffers), renderGraph(graph), resources(resources),
      globalSetLayout(globalSetLayout), materialSetLayout(materialSetLayout),
      terrainSetLayout(terrainSetLayout), terrainMeshArena(terrainMeshArena)
{
}

GeometryPass::~GeometryPass() = default;

void GeometryPass::init()
{
    // D-4：每帧槽位导入自己的一套 G-Buffer。
    // initialLayout 用 UNDEFINED：它们每帧都会被完整重写，内容不需要保留。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        GBuffer& gBuffer = *gBuffers[frame];
        auto import = [&](const char* name, const FrameBufferAttachment& att, VkImageUsageFlags usage)
        {
            RenderGraph::TextureDesc desc;
            desc.name = std::string(name) + ".Frame" + std::to_string(frame);
            desc.format = att.format;
            desc.extent = gBuffer.getExtent();
            desc.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
            return renderGraph.importTexture(desc, att.image, att.view, VK_IMAGE_LAYOUT_UNDEFINED);
        };

        const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        resources.gbPosition[frame] = import("GBuffer.Position", gBuffer.getPosition(), colorUsage);
        resources.gbNormal[frame]   = import("GBuffer.Normal",   gBuffer.getNormal(),   colorUsage);
        resources.gbAlbedo[frame]   = import("GBuffer.Albedo",   gBuffer.getAlbedo(),   colorUsage);
        resources.gbPbr[frame]      = import("GBuffer.Pbr",      gBuffer.getPbr(),      colorUsage);
        resources.gbDepth[frame]    = import("GBuffer.Depth",    gBuffer.getDepth(),    depthUsage);
    }

    // F1：GPU 驱动提交。先建实例缓冲，再把它的布局与裸指针交给几何系统
    // （set 3 必须是管线布局的一部分，插不了管道，只能建的时候一起给出）。
    sceneGpuBatch = std::make_unique<SceneGpuBatch>(vulkanDevice);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        sceneGpuBatch->setHiZSource(frame, gBuffers[frame]->getPosition().view,
                                    gBuffers[frame]->getExtent());

    // 所有 G-Buffer 的格式相同，几何管线只建一份。
    geometrySystem = std::make_unique<GeometrySystem>(vulkanDevice, gBuffers[0]->getDepth().format,
                                                      globalSetLayout, materialSetLayout, terrainSetLayout,
                                                      sceneGpuBatch->getInstanceSetLayout(),
                                                      sceneGpuBatch.get(),
                                                      terrainMeshArena);
}

void GeometryPass::resize(VkExtent2D extent)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        GBuffer& gBuffer = *gBuffers[frame];
        // G-Buffer 重建后视图会变，Hi-Z 的采样描述符要跟着更新。
        sceneGpuBatch->setHiZSource(frame, gBuffer.getPosition().view, gBuffer.getExtent());
        auto update = [&](const char* name, const FrameBufferAttachment& att, VkImageUsageFlags usage,
                          RenderGraph::TextureHandle handle)
        {
            RenderGraph::TextureDesc desc;
            desc.name = std::string(name) + ".Frame" + std::to_string(frame);
            desc.format = att.format;
            desc.extent = extent;
            desc.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
            renderGraph.updateImportedTexture(handle, desc, att.image, att.view, VK_IMAGE_LAYOUT_UNDEFINED);
        };

        update("GBuffer.Position", gBuffer.getPosition(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, resources.gbPosition[frame]);
        update("GBuffer.Normal",   gBuffer.getNormal(),   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, resources.gbNormal[frame]);
        update("GBuffer.Albedo",   gBuffer.getAlbedo(),   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, resources.gbAlbedo[frame]);
        update("GBuffer.Pbr",      gBuffer.getPbr(),      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, resources.gbPbr[frame]);
        update("GBuffer.Depth",    gBuffer.getDepth(),    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, resources.gbDepth[frame]);
    }
}

void GeometryPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const uint32_t frame = frameInfo.frameInFlightIndex;

    VkClearValue clearColor{};
    clearColor.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkClearValue clearDepth{};
    clearDepth.depthStencil = { 1.0f, 0 };

    // B-03: dispatch GPU terrain selection before dynamic rendering begins.
    // This pass has no attachments, so RenderGraph executes it outside a render
    // pass while still recording onto the graphics command buffer.
    auto& selectionPass = graph.addGraphicsPass("TerrainGpuSelection");
    selectionPass.execute([this, &frameInfo](VkCommandBuffer cmd)
    {
        geometrySystem->prepareTerrainGpuSelection(cmd, frameInfo);
    });

    // F1：普通网格的 GPU 剔除。没有附件，所以在渲染通道之外执行（纯 compute），
    // 必须在 Geometry 之前把间接命令与计数写好。
    auto& cullPass = graph.addGraphicsPass("SceneGpuCull");
    cullPass.execute([this, &frameInfo](VkCommandBuffer cmd)
    {
        sceneGpuBatch->prepare(cmd, frameInfo);
    });

    auto& pass = graph.addGraphicsPass("Geometry");
    pass.writeColor(resources.gbPosition[frame], VK_ATTACHMENT_LOAD_OP_CLEAR, clearColor);
    pass.writeColor(resources.gbNormal[frame],   VK_ATTACHMENT_LOAD_OP_CLEAR, clearColor);
    pass.writeColor(resources.gbAlbedo[frame],   VK_ATTACHMENT_LOAD_OP_CLEAR, clearColor);
    pass.writeColor(resources.gbPbr[frame],      VK_ATTACHMENT_LOAD_OP_CLEAR, clearColor);
    pass.writeDepth(resources.gbDepth[frame],    VK_ATTACHMENT_LOAD_OP_CLEAR, clearDepth);

    pass.execute([this, &frameInfo](VkCommandBuffer cmd)
    {
        geometrySystem->render(cmd, frameInfo);
    });

    // F1-c：几何画完再用这一帧的位置缓冲重建 Hi-Z 金字塔。
    // 剔除发生在 SceneGpuCull，读的是上一帧建好的那份（错开一帧）。
    auto& hizPass = graph.addGraphicsPass("HiZBuild");
    hizPass.read(resources.gbPosition[frame], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
    hizPass.execute([this, &frameInfo](VkCommandBuffer cmd)
    {
        GBuffer& gBuffer = *gBuffers[frameInfo.frameInFlightIndex];
        sceneGpuBatch->buildHiZ(cmd, frameInfo, gBuffer.getPosition().view,
                               gBuffer.getExtent());
    });
}
