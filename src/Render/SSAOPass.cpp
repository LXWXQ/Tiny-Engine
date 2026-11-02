#include "Render/SSAOPass.h"
#include "RHI/CommandUtils.h"
#include "AssetPaths.h"

#include <random>
#include <stdexcept>
#include <cstring>
#include <cstdlib>     // std::getenv（反面用例：SENTINEL_SIMULATE_SSAO_SKIP）
#include <array>

SSAOPass::SSAOPass(Device& device, RenderGraph& graph, RenderResources& resources,
                   VkDescriptorSetLayout globalLayout, DescriptorManager* descMgr)
    : device(device), renderGraph(graph), resources(resources),
      descriptorManager(descMgr), globalSetLayout(globalLayout)
{
}

SSAOPass::~SSAOPass()
{
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (localSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device.getDevice(), localSetLayout, nullptr);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (uboBuffers[frame] != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), uboBuffers[frame], uboAllocations[frame]);
    }

    if (noiseTexture.sampler != VK_NULL_HANDLE) vkDestroySampler(device.getDevice(), noiseTexture.sampler, nullptr);
    if (noiseTexture.view != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), noiseTexture.view, nullptr);
    if (noiseTexture.image != VK_NULL_HANDLE) vmaDestroyImage(device.getVmaAllocator(), noiseTexture.image, noiseTexture.allocation);

    blurPipeline.reset();
    if (blurPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device.getDevice(), blurPipelineLayout, nullptr);
    if (blurSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device.getDevice(), blurSetLayout, nullptr);

    if (outputSampler != VK_NULL_HANDLE) vkDestroySampler(device.getDevice(), outputSampler, nullptr);
}

VkImageView SSAOPass::getOutputImageView(uint32_t frameInFlightIndex) const
{
    return renderGraph.getView(resources.ssao[frameInFlightIndex]);
}

void SSAOPass::init()
{
    // 输出目标交给渲染图创建；D-4 起每个 frame slot 一张，避免跨帧读写冲突。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        RenderGraph::TextureDesc desc;
        desc.name = "SSAO.Raw.Frame" + std::to_string(frame);
        desc.format = VK_FORMAT_R8_UNORM;
        desc.extent = renderGraph.getExtent(resources.gbPosition[frame]);
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        resources.ssaoRaw[frame] = renderGraph.createTexture(desc);

        desc.name = "SSAO.Frame" + std::to_string(frame);
        resources.ssao[frame] = renderGraph.createTexture(desc);
    }

    // 采样器要自己建：渲染图只管图像，不管采样器
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &si, nullptr, &outputSampler) != VK_SUCCESS)
        throw std::runtime_error("SSAOPass: 输出采样器创建失败");

    generateSampleKernel();
    createNoiseTexture();
    createUniformBuffer();
    createLocalDescriptors();
    createPipeline();
    createBlurPipeline();
}

void SSAOPass::generateSampleKernel()
{
    std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    std::default_random_engine generator;

    ssaoKernel.clear();
    for (int i = 0; i < 64; ++i)
    {
        glm::vec3 sample(
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator));
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);

        // 靠近中心的样本更密集
        float scale = static_cast<float>(i) / 64.0f;
        scale = glm::mix(0.1f, 1.0f, scale * scale);
        sample *= scale;

        ssaoKernel.push_back(glm::vec4(sample, 0.0f));
    }
}

void SSAOPass::createNoiseTexture()
{
    std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    std::default_random_engine generator;
    std::vector<glm::vec4> ssaoNoise;
    for (int i = 0; i < 16; i++)
        ssaoNoise.push_back(glm::vec4(randomFloats(generator) * 2.0f - 1.0f,
                                      randomFloats(generator) * 2.0f - 1.0f, 0.0f, 1.0f));

    VkDeviceSize imageSize = ssaoNoise.size() * sizeof(glm::vec4);

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &stagingAllocInfo, &stagingBuffer, &stagingAllocation, nullptr);
    vmaCopyMemoryToAllocation(device.getVmaAllocator(), ssaoNoise.data(), stagingAllocation, 0, imageSize);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { 4, 4, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vmaCreateImage(device.getVmaAllocator(), &imageInfo, &imageAllocInfo,
                   &noiseTexture.image, &noiseTexture.allocation, nullptr);


    // 用一次性命令缓冲做拷贝 + 布局转换
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        // 复用设备上已有一个通用命令池：这里临时新建一个专用池，避免和渲染用的池耦合
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = device.findPhysicalQueueFamilies().graphicsFamily.value();
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(device.getDevice(), &poolInfo, nullptr, &cmdPool);
    }

    VkCommandBuffer c = cmdBeginSingleTime(device.getDevice(), cmdPool);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = noiseTexture.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { 4, 4, 1 };
    vkCmdCopyBufferToImage(c, stagingBuffer, noiseTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    cmdEndSingleTime(device.getDevice(), device.getGraphicsQueue(), cmdPool, c);
    vkDestroyCommandPool(device.getDevice(), cmdPool, nullptr);

    vmaDestroyBuffer(device.getVmaAllocator(), stagingBuffer, stagingAllocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = noiseTexture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &noiseTexture.view);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(device.getDevice(), &samplerInfo, nullptr, &noiseTexture.sampler);
}

void SSAOPass::createUniformBuffer()
{
    // 每个 frame slot 一份 UBO。两份 buffer 不能共用一个映射地址，
    // 否则第 N+1 帧的 memcpy 会覆盖 GPU 仍在读的第 N 帧参数。
    static const char* const names[kFramesInFlight] = { "SSAOUbo.Frame0", "SSAOUbo.Frame1" };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkDeviceSize bufferSize = sizeof(SSAOUbo);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocInfo.flags |= sentinel_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>(names[frame]);

        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
            &uboBuffers[frame], &uboAllocations[frame], nullptr) != VK_SUCCESS)
            throw std::runtime_error("SSAOPass: per-frame UBO 创建失败");

        VmaAllocationInfo info;
        vmaGetAllocationInfo(device.getVmaAllocator(), uboAllocations[frame], &info);
        uboMapped[frame] = info.pMappedData;
    }
}

void SSAOPass::createLocalDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    localSetLayout = descriptorManager->createSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(), bindings.end()));

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        localSets[frame] = descriptorManager->allocateSet(localSetLayout);

        VkDescriptorBufferInfo bufferInfo{ uboBuffers[frame], 0, sizeof(SSAOUbo) };
        VkDescriptorImageInfo noiseInfo{ noiseTexture.sampler, noiseTexture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = localSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = localSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &noiseInfo;
        vkUpdateDescriptorSets(device.getDevice(), 2, writes.data(), 0, nullptr);
    }
}

void SSAOPass::updateBlurDescriptor(uint32_t frame)
{
    if (blurSets[frame] == VK_NULL_HANDLE)
        return;

    VkDescriptorImageInfo aoInfo{
        outputSampler,
        renderGraph.getView(resources.ssaoRaw[frame]),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = blurSets[frame];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &aoInfo;
    vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);
}

void SSAOPass::createBlurPipeline()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blurSetLayout = descriptorManager->createSetLayout({ binding });

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        blurSets[frame] = descriptorManager->allocateSet(blurSetLayout);
        updateBlurDescriptor(frame);
    }

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &blurSetLayout;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &blurPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("SSAOPass: blur pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.colorAttachmentFormats = { VK_FORMAT_R8_UNORM };
    config.pipelineLayout = blurPipelineLayout;
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    blurPipeline = std::make_unique<Pipeline>(device,
        SENTINEL_SHADER_DIR "fullscreen.vert.spv",
        SENTINEL_SHADER_DIR "ssao_blur.frag.spv", config);
}

void SSAOPass::createPipeline()
{
    std::array<VkDescriptorSetLayout, 2> layouts = { globalSetLayout, localSetLayout };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
    pipelineLayoutInfo.pSetLayouts = layouts.data();
    vkCreatePipelineLayout(device.getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout);

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.colorAttachmentFormats = { VK_FORMAT_R8_UNORM };   // 与渲染图里的输出格式一致
    config.pipelineLayout = pipelineLayout;
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    ssaoPipeline = std::make_unique<Pipeline>(device,
        SENTINEL_SHADER_DIR "fullscreen.vert.spv", SENTINEL_SHADER_DIR "ssao.frag.spv", config);
}

void SSAOPass::resize(VkExtent2D extent)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        renderGraph.resizeOwnedTexture(resources.ssaoRaw[frame], extent);
        renderGraph.resizeOwnedTexture(resources.ssao[frame], extent);
        updateBlurDescriptor(frame);
    }
}

void SSAOPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    // 每帧上传 SSAO 参数
    const uint32_t frame = frameInfo.frameInFlightIndex;

    SSAOUbo ubo{};
    ubo.projection = frameInfo.projectionMatrix;
    ubo.view = frameInfo.viewMatrix;
    ubo.radius = frameInfo.settings.ssaoRadius;
    ubo.bias = frameInfo.settings.ssaoBias;
    ubo.kernelSize = frameInfo.settings.ssaoKernelSize;
    ubo.enabled = frameInfo.settings.ssaoEnabled ? 1.0f : 0.0f;
    memcpy(ubo.samples, ssaoKernel.data(), 64 * sizeof(glm::vec4));
    memcpy(uboMapped[frame], &ubo, sizeof(ubo));
    vmaFlushAllocation(device.getVmaAllocator(), uboAllocations[frame], 0, sizeof(ubo));

    VkClearValue clear{};
    clear.color = { { 1.0f, 1.0f, 1.0f, 1.0f } };

    auto& pass = graph.addGraphicsPass("SSAO");
    pass.read(resources.gbPosition[frame]);
    pass.read(resources.gbNormal[frame]);
    pass.writeColor(resources.ssaoRaw[frame], VK_ATTACHMENT_LOAD_OP_CLEAR, clear);

    // 注意：这里**故意不调用 pass.setEnabled(ssaoEnabled)**。
    //
    // 关掉 SSAO 如果跳过整个 Pass，ssao 这张瞬时图整帧就没人写，
    // 而 lighting.frag 的 `ambient *= ssao` 是无条件执行的 —— 环境光会被
    // 乘以未定义内容（画面忽明忽暗），而且验证层不会报（从 UNDEFINED 转出合法）。
    // 所以开关下沉到着色器：Pass 照跑，关闭时片元着色器直接输出中性白。
    // 开销是一次几乎没有纹理采样的全屏写入，换来的是"开关状态"永远有定义。

    // -----------------------------------------------------------------------
    // 反面用例（SENTINEL_SIMULATE_SSAO_SKIP=1）：
    // 故意把上面那个错误写法做出来 —— 跳过整个 Pass。
    // 用途是验证渲染图的语义校验真的能拦住它，而不是只写在注释里当传说。
    //   RenderGraph::validateWriteCoverage() 应该抛出
    //   「Pass "Lighting" 读取了瞬时纹理 "SSAO"，但没有任何 Pass 写过它」。
    // 实测（5 帧自检）确实在启动第一帧就报出来了，而且是可读的中文错误。
    // 环境变量只读一次 —— 没必要每帧走一次 getenv。
    // -----------------------------------------------------------------------
    static const bool simulateSkip = (std::getenv("SENTINEL_SIMULATE_SSAO_SKIP") != nullptr);
    if (simulateSkip)
        pass.setEnabled(false);

    pass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        ssaoPipeline->bind(cmd);
        VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
        VkDescriptorSet sets[] = { globalSet, localSets[frame] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });

    // SSAO uses a 4x4 noise texture to rotate its kernel. Without a
    // post-filter that noise becomes visible as a fine screen-space pattern
    // on distant, low-contrast surfaces. Blur the raw AO before Lighting reads it.
    auto& blurPass = graph.addGraphicsPass("SSAOBlur");
    blurPass.read(resources.ssaoRaw[frame]);
    blurPass.writeColor(resources.ssao[frame], VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    blurPass.execute([this, frame](VkCommandBuffer cmd)
    {
        blurPipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipelineLayout,
            0, 1, &blurSets[frame], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });
}