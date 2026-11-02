#include "BloomPass.h"
#include "RHI/CommandUtils.h"
#include "AssetPaths.h"
#include <stdexcept>
#include <algorithm>
#include <array>
#include <iostream>

// push constant 的字节布局必须和 shaders/bloom_*.frag 里的声明逐一对应。
// GLSL 的 push_constant 块按 std430 布局：vec2 占 8 字节、对齐 8，
// 因此在 vec2 后面跟一个 int 时偏移是 8 而不是 12 —— C++ 这边的默认对齐
// 恰好一致，但"恰好一致"是靠不住的，用 static_assert 钉死。
static_assert(sizeof(BloomPass::BrightPush) == 12, "BrightPush 必须与 bloom_bright.frag 的 push_constant 等大");
static_assert(sizeof(BloomPass::DownPush) == 16, "DownPush 必须与 bloom_down.frag 的 push_constant 等大");
static_assert(sizeof(BloomPass::UpPush) == 16, "UpPush 必须与 bloom_up.frag 的 push_constant 等大");

namespace
{
    // 每级固定减半；分辨率低到 1 像素时就不再缩小，避免 0 尺寸图像
    VkExtent2D halfExtent(VkExtent2D e)
    {
        return VkExtent2D{ std::max(1u, e.width / 2u), std::max(1u, e.height / 2u) };
    }
}

BloomPass::BloomPass(Device& device, RenderGraph& graph, RenderResources& resources,
                     DescriptorManager* descMgr)
    : device(device), renderGraph(graph), resources(resources), descriptorManager(descMgr)
{
}

BloomPass::~BloomPass()
{
    if (brightLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device.getDevice(), brightLayout, nullptr);
    if (downLayout != VK_NULL_HANDLE)   vkDestroyPipelineLayout(device.getDevice(), downLayout, nullptr);
    if (upLayout != VK_NULL_HANDLE)     vkDestroyPipelineLayout(device.getDevice(), upLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)    vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE)      vkDestroySampler(device.getDevice(), sampler, nullptr);
}

void BloomPass::init()
{
    createDescriptorLayout();
    createSampler();

    // 描述符集数量：每帧 1 个亮部提取 + 2*(kLevelCount-1) 个降/上采样。
    // 每个 frame slot 各有一组，描述符指向该帧自己的 HDR 与 Bloom 金字塔。
    const uint32_t setCount = 1 + 2 * (kLevelCount - 1);
    sets.resize(kFramesInFlight * setCount);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        // ---- 金字塔各级：每级都是上一级的一半 ----
        // 各级同时要能当颜色附件（被写）和采样源（被读），所以 usage 两个都要。
        std::vector<RenderGraph::TextureHandle>& levels = resources.bloomLevels[frame];
        levels.clear();
        levels.reserve(kLevelCount);

        VkExtent2D extent = renderGraph.getExtent(resources.hdrScene[frame]);
        for (uint32_t i = 0; i < kLevelCount; ++i)
        {
            extent = halfExtent(extent);

            RenderGraph::TextureDesc desc;
            desc.name = "Bloom.Frame" + std::to_string(frame) + ".Level" + std::to_string(i);
            desc.format = renderGraph.getFormat(resources.hdrScene[frame]);
            desc.extent = extent;
            desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

            levels.push_back(renderGraph.createTexture(desc));
        }

        // PresentPass 采样的就是最细的那一级
        resources.bloomResult[frame] = levels.front();

        const uint32_t base = frame * setCount;
        for (uint32_t i = 0; i < setCount; ++i)
            sets[base + i] = descriptorManager->allocateSet(setLayout);

        // 纹理由渲染图持有，VkImageView 只在 start/end 时重建 —— 启动时写一次就够，
        // 不需要每帧 vkUpdateDescriptorSets。
        updateDescriptor(base, renderGraph.getView(resources.hdrScene[frame]));
        for (uint32_t i = 0; i < kLevelCount - 1; ++i)
        {
            updateDescriptor(base + 1 + i, renderGraph.getView(levels[i]));
            updateDescriptor(base + 1 + (kLevelCount - 1) + i, renderGraph.getView(levels[i + 1]));
        }
    }

    createPipelines();
}

void BloomPass::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    info.bindingCount = 1;
    info.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device.getDevice(), &info, nullptr, &setLayout) != VK_SUCCESS)
        throw std::runtime_error("BloomPass: 描述符集布局创建失败");
}

void BloomPass::createSampler()
{
    // 金字塔各级都靠双线性抽样来"顺便"做一次平均，所以线性过滤是必须的
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;

    if (vkCreateSampler(device.getDevice(), &info, nullptr, &sampler) != VK_SUCCESS)
        throw std::runtime_error("BloomPass: 采样器创建失败");
}

void BloomPass::updateDescriptor(uint32_t index, VkImageView view)
{
    VkDescriptorImageInfo imageInfo{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = sets[index];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);
}

void BloomPass::createPipelines()
{
    const VkFormat format = renderGraph.getFormat(resources.hdrScene[0]);
    const std::string vert = SENTINEL_SHADER_DIR "fullscreen.vert.spv";

    auto makeLayout = [&](uint32_t pushSize)
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = pushSize;

        VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        info.setLayoutCount = 1;
        info.pSetLayouts = &setLayout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device.getDevice(), &info, nullptr, &layout) != VK_SUCCESS)
            throw std::runtime_error("BloomPass: 管线布局创建失败");
        return layout;
    };

    brightLayout = makeLayout(sizeof(BrightPush));
    downLayout   = makeLayout(sizeof(DownPush));
    upLayout     = makeLayout(sizeof(UpPush));

    auto baseConfig = [&]()
    {
        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
        config.colorAttachmentFormats = { format };
        // 全屏三角形不需要顶点输入
        config.bindingDescriptions.clear();
        config.attributeDescriptions.clear();
        config.depthStencilInfo.depthTestEnable = VK_FALSE;
        config.depthStencilInfo.depthWriteEnable = VK_FALSE;
        return config;
    };

    {
        PipelineConfigInfo config = baseConfig();
        config.pipelineLayout = brightLayout;
        brightPipeline = std::make_unique<Pipeline>(device, vert,
            SENTINEL_SHADER_DIR "bloom_bright.frag.spv", config);
    }

    {
        PipelineConfigInfo config = baseConfig();
        config.pipelineLayout = downLayout;
        downPipeline = std::make_unique<Pipeline>(device, vert,
            SENTINEL_SHADER_DIR "bloom_down.frag.spv", config);
    }

    {
        PipelineConfigInfo config = baseConfig();
        config.pipelineLayout = upLayout;

        // 上采样是「把粗级累加回细级」，必须用加法混合，否则粗级会覆盖掉
        // 细级已经写好的内容 —— 辉光会变成一片模糊而没有细节。
        config.colorBlendAttachments[0].blendEnable = VK_TRUE;
        config.colorBlendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        config.colorBlendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        config.colorBlendAttachments[0].colorBlendOp = VK_BLEND_OP_ADD;
        config.colorBlendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        config.colorBlendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        config.colorBlendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;

        upPipeline = std::make_unique<Pipeline>(device, vert,
            SENTINEL_SHADER_DIR "bloom_up.frag.spv", config);
    }
}

void BloomPass::resize(VkExtent2D extent)
{
    const uint32_t setCount = 1 + 2 * (kLevelCount - 1);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        std::vector<RenderGraph::TextureHandle>& levels = resources.bloomLevels[frame];
        VkExtent2D levelExtent = extent;
        for (uint32_t i = 0; i < kLevelCount; ++i)
        {
            levelExtent = halfExtent(levelExtent);
            renderGraph.resizeOwnedTexture(levels[i], levelExtent);
        }

        resources.bloomResult[frame] = levels.front();
        const uint32_t base = frame * setCount;
        updateDescriptor(base, renderGraph.getView(resources.hdrScene[frame]));
        for (uint32_t i = 0; i < kLevelCount - 1; ++i)
        {
            updateDescriptor(base + 1 + i, renderGraph.getView(levels[i]));
            updateDescriptor(base + 1 + (kLevelCount - 1) + i, renderGraph.getView(levels[i + 1]));
        }
    }
}

void BloomPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const EngineSettings& s = frameInfo.settings;
    const uint32_t downs = kLevelCount - 1;
    const uint32_t frame = frameInfo.frameInFlightIndex;
    const auto& levels = resources.bloomLevels[frame];
    const uint32_t setCount = 1 + 2 * (kLevelCount - 1);
    const uint32_t setBase = frame * setCount;

    // TD-06: Bloom disabled means no Bloom passes at all. PresentPass has a
    // dedicated single-input descriptor set/pipeline in this case, so it does
    // not bind bloomResult and does not require a layout transition for it.
    if (!s.bloomEnabled)
    {
        static bool reportedOnce = false;
        if (!reportedOnce)
        {
            reportedOnce = true;
            std::cout << "[TD06] Bloom disabled: skipping Bright + Down + Up passes\n";
        }
        return;
    }

    VkClearValue clear{};
    clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    // ---- 1) 亮部提取：HDR -> 最细一级 ----
    {
        auto& pass = graph.addGraphicsPass("Bloom.Bright");
        pass.read(resources.hdrScene[frame]);
        pass.writeColor(levels[0], VK_ATTACHMENT_LOAD_OP_CLEAR, clear);

        BrightPush push{ s.bloomThreshold, s.bloomKnee, 1.0f };
        const uint32_t setIndex = setBase;
        pass.execute([this, push, setIndex](VkCommandBuffer cmd)
        {
            brightPipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, brightLayout,
                0, 1, &sets[setIndex], 0, nullptr);
            vkCmdPushConstants(cmd, brightLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        });
    }

    // ---- 2) 降采样链 ----
    // 首级做 Karis 加权：它直接吃 HDR，是萤火虫（孤立超亮像素）进来的唯一入口。
    for (uint32_t i = 0; i < downs; ++i)
    {
        auto& pass = graph.addGraphicsPass("Bloom.Down" + std::to_string(i));
        pass.read(levels[i]);
        pass.writeColor(levels[i + 1], VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        const VkExtent2D srcExtent = graph.getExtent(levels[i]);
        DownPush push{};
        push.srcTexelSize = glm::vec2(1.0f / float(srcExtent.width), 1.0f / float(srcExtent.height));
        push.karisAverage = (i == 0) ? 1 : 0;

        const uint32_t setIndex = setBase + 1 + i;
        pass.execute([this, push, setIndex](VkCommandBuffer cmd)
        {
            downPipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, downLayout,
                0, 1, &sets[setIndex], 0, nullptr);
            vkCmdPushConstants(cmd, downLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        });
    }

    // ---- 3) 上采样叠加（由粗到细）----
    // LOAD_OP_LOAD + 加法混合 => 粗级结果累加进本级，不需要额外缓冲。
    for (uint32_t i = downs; i-- > 0; )
    {
        auto& pass = graph.addGraphicsPass("Bloom.Up" + std::to_string(i));
        pass.read(levels[i + 1]);
        pass.writeColor(levels[i], VK_ATTACHMENT_LOAD_OP_LOAD);
        const VkExtent2D srcExtent = graph.getExtent(levels[i + 1]);
        UpPush push{};
        push.srcTexelSize = glm::vec2(1.0f / float(srcExtent.width), 1.0f / float(srcExtent.height));
        push.radius = 1.0f;

        const uint32_t setIndex = setBase + 1 + downs + i;
        pass.execute([this, push, setIndex](VkCommandBuffer cmd)
        {
            upPipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upLayout,
                0, 1, &sets[setIndex], 0, nullptr);
            vkCmdPushConstants(cmd, upLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        });
    }
}
