#include "LightingPass.h"
#include "Core/DescriptorManager.h"
#include <stdexcept>
#include <array>

LightingPass::LightingPass(Device& device, GBuffer& gBuffer, Swapchain& swapchain,
                           RenderGraph& graph, RenderResources& resources,
                           VkDescriptorSetLayout globalLayout)
    : vulkanDevice(device), gBuffer(gBuffer), vulkanSwapchain(swapchain),
      renderGraph(graph), resources(resources), globalSetLayout(globalLayout)
{
}

LightingPass::~LightingPass() = default;

void LightingPass::init()
{
    // 把交换链的所有图像导入渲染图。
    // initialLayout 用 UNDEFINED：present 之后 Vulkan 规定交换链图像布局回到未定义，
    // 每帧都是全新的，所以我们每帧都从 UNDEFINED 开始（内容由 clear 负责）。
    resources.swapchainImages.resize(vulkanSwapchain.imageCount());
    for (uint32_t i = 0; i < vulkanSwapchain.imageCount(); ++i)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "Swapchain[" + std::to_string(i) + "]";
        desc.format = vulkanSwapchain.getSwapChainImageFormat();
        desc.extent = vulkanSwapchain.getSwapChainExtent();
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        resources.swapchainImages[i] = renderGraph.importTexture(
            desc, vulkanSwapchain.getImage(i), vulkanSwapchain.getImageView(i),
            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // D-2：光照的终点是线性 HDR 图，不再是交换链。
    // D-4：每个 frame slot 一张，避免第 N+1 帧覆盖 GPU 仍在读的第 N 帧结果。
    // 这里顺便承担了"交换链资源导入"的职责，所以 PresentPass 依赖 LightingPass 先 init。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::TextureDesc hdrDesc;
        hdrDesc.name = "HDR.Scene.Frame" + std::to_string(frame);
        hdrDesc.format = kHdrSceneFormat;
        hdrDesc.extent = vulkanSwapchain.getSwapChainExtent();
        hdrDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resources.hdrScene[frame] = renderGraph.createTexture(hdrDesc);
    }

    lightingSystem = std::make_unique<LightingSystem>(vulkanDevice, globalSetLayout);
}

void LightingPass::resize(VkExtent2D extent)
{
    const uint32_t imageCount = vulkanSwapchain.imageCount();
    resources.swapchainImages.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "Swapchain[" + std::to_string(i) + "]";
        desc.format = vulkanSwapchain.getSwapChainImageFormat();
        desc.extent = vulkanSwapchain.getSwapChainExtent();
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        if (!resources.swapchainImages[i].valid())
        {
            resources.swapchainImages[i] = renderGraph.importTexture(
                desc, vulkanSwapchain.getImage(i), vulkanSwapchain.getImageView(i),
                VK_IMAGE_LAYOUT_UNDEFINED);
        }
        else
        {
            renderGraph.updateImportedTexture(resources.swapchainImages[i], desc,
                vulkanSwapchain.getImage(i), vulkanSwapchain.getImageView(i),
                VK_IMAGE_LAYOUT_UNDEFINED);
        }
    }

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        renderGraph.resizeOwnedTexture(resources.hdrScene[frame], extent);
}

void LightingPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    VkClearValue clear{};
    clear.color = { { 0.01f, 0.01f, 0.01f, 1.0f } };

    const uint32_t frame = frameInfo.frameInFlightIndex;
    auto& pass = graph.addGraphicsPass("Lighting");

    // 声明所有采样输入 —— 渲染图靠这些声明推导布局转换和屏障，
    // 之前这些依赖是「隐式」的，全靠手工维护，非常容易漏。
    pass.read(resources.gbPosition[frame]);
    pass.read(resources.gbNormal[frame]);
    pass.read(resources.gbAlbedo[frame]);
    pass.read(resources.gbPbr[frame]);
    pass.read(resources.ssao[frame], VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    pass.read(resources.environment);
    pass.read(resources.irradiance);
    pass.read(resources.prefiltered);
    pass.read(resources.brdfLut);

    // D-2：写 HDR 中间目标。这一句就是"把后效链接进去"的全部代价 ——
    // 交换链不再出现在这里，谁最终写交换链由 PresentPass 决定。
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_CLEAR, clear);

    pass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            lightingSystem->getPipelineLayout(), 0, 1, &globalSet, 0, nullptr);

        lightingSystem->render(cmd, frameInfo);
    });
}