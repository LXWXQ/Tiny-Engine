#include "Render/AutoExposurePass.h"

#include "AssetPaths.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInfo.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace
{
    // 亮度统计范围：log2(1e-4) 约 -13.3，log2(16) = 4。
    // 下限比夹取的更宽一点，避免"几乎全黑"的画面把均值顶到下限上。
    constexpr float kMinLog = -12.0f;
    constexpr float kMaxLog = 4.0f;

    // 采样步长：1920x1080 上约 3.2 万个采样点，统计稳定且几乎不花时间。
    constexpr uint32_t kSampleStride = 8;

    // 目标中灰与适应速度。0.18 是线性空间的常用中灰；1.5/s 意味着
    // 亮度突变后约 1 秒走完三分之二，比人眼慢一点，避免画面"呼吸"。
    constexpr float kKeyLuminance = 0.18f;
    constexpr float kAdaptSpeed = 1.5f;

    constexpr float kMinExposure = 0.05f;
    constexpr float kMaxExposure = 20.0f;

    constexpr uint32_t kQuantMax = 65535u;
}

AutoExposurePass::AutoExposurePass(Device& device, RenderGraph& graph, RenderResources& resources,
                                   DescriptorManager* descriptorManager)
    : device(device), renderGraph(graph), resources(resources), descriptorManager(descriptorManager)
{
    exposure = 1.0f;
    createBuffers();
    createDescriptors();
    createPipeline();
}

AutoExposurePass::~AutoExposurePass()
{
    pipeline.reset();

    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE)
        vkDestroySampler(device.getDevice(), sampler, nullptr);

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        if (statsBuffers[i] != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), statsBuffers[i], statsAllocations[i]);
        statsBuffers[i] = VK_NULL_HANDLE;
        statsAllocations[i] = VK_NULL_HANDLE;
        statsMapped[i] = nullptr;
    }
}

void AutoExposurePass::createBuffers()
{
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = sizeof(Stats);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                    | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &statsBuffers[i], &statsAllocations[i], &info) != VK_SUCCESS)
            throw std::runtime_error("AutoExposurePass: 统计缓冲创建失败");
        statsMapped[i] = static_cast<Stats*>(info.pMappedData);
        std::memset(statsMapped[i], 0, sizeof(Stats));
    }
}

void AutoExposurePass::createDescriptors()
{
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.getDevice(), &layoutInfo, nullptr, &setLayout) != VK_SUCCESS)
        throw std::runtime_error("AutoExposurePass: 描述符集布局创建失败");

    // 自己的 set：不碰 Set 0 的 19 个 binding，省掉"重建 Set 0 必须补写"的那类坑。
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        sets[i] = descriptorManager != nullptr
            ? descriptorManager->allocateSet(setLayout)
            : VK_NULL_HANDLE;
        if (sets[i] == VK_NULL_HANDLE)
            throw std::runtime_error("AutoExposurePass: 描述符集分配失败");
    }

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
        throw std::runtime_error("AutoExposurePass: 采样器创建失败");
}

void AutoExposurePass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("AutoExposurePass: 管线布局创建失败");

    const std::string shaderPath = std::string(TINY_SHADER_DIR) + "exposure_reduce.comp.spv";
    pipeline = std::make_unique<ComputePipeline>(device, shaderPath, pipelineLayout);
}

float AutoExposurePass::adaptExposure(float previous, float target, float deltaSeconds, float speed)
{
    if (!(previous > 0.0f) || !(target > 0.0f))
        return std::clamp(target > 0.0f ? target : 1.0f, kMinExposure, kMaxExposure);

    const float dt = std::max(deltaSeconds, 0.0f);
    const float rate = 1.0f - std::exp(-dt * std::max(speed, 0.0f));
    const float blended = std::exp((1.0f - rate) * std::log(previous) + rate * std::log(target));
    return std::clamp(blended, kMinExposure, kMaxExposure);
}

float AutoExposurePass::update(uint32_t frameInFlightIndex, float deltaTime)
{
    if (!enabled || frameInFlightIndex >= kFramesInFlight)
        return exposure;

    const Stats* stats = statsMapped[frameInFlightIndex];
    if (stats == nullptr)
        return exposure;

    sampleCount = stats->count;
    if (stats->count == 0)
        return exposure;   // 还没跑过 dispatch，保持上一次的值

    const float normalized = static_cast<float>(stats->sumQ)
                           / (static_cast<float>(kQuantMax) * static_cast<float>(stats->count));
    const float avgLog = kMinLog + std::clamp(normalized, 0.0f, 1.0f) * (kMaxLog - kMinLog);
    averageLuminance = std::exp2(avgLog);

    const float target = std::clamp(kKeyLuminance / std::max(averageLuminance, 1e-4f),
                                    kMinExposure, kMaxExposure);
    exposure = adaptExposure(exposure, target, deltaTime, kAdaptSpeed);
    return exposure;
}

void AutoExposurePass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    if (!enabled || pipeline == nullptr)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    if (frame >= kFramesInFlight || sets[frame] == VK_NULL_HANDLE)
        return;

    const VkExtent2D extent = graph.getExtent(resources.hdrScene[frame]);
    if (extent.width == 0 || extent.height == 0)
        return;

    // HDR 视图在 resize 后会换，所以每帧重写一次描述符。
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = graph.getView(resources.hdrScene[frame]);
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = statsBuffers[frame];
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(Stats);

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = sets[frame];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = sets[frame];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device.getDevice(), 2, writes, 0, nullptr);

    const uint32_t samplesX = (extent.width + kSampleStride - 1u) / kSampleStride;
    const uint32_t samplesY = (extent.height + kSampleStride - 1u) / kSampleStride;
    const uint32_t groupsX = (samplesX + 15u) / 16u;
    const uint32_t groupsY = (samplesY + 15u) / 16u;

    PushConstants push{};
    push.params = glm::vec4(1.0f / static_cast<float>(extent.width),
                            1.0f / static_cast<float>(extent.height), kMinLog, kMaxLog);
    push.stride = kSampleStride;

    // 走图形命令流里的 compute dispatch（与 ClusterBuildPass 同样的理由）：
    // 输入是图形队列刚写过的 HDR，丢进异步 compute 会踩 TM-09 的跨队列墙。
    auto& pass = graph.addGraphicsPass("AutoExposure.Reduce");
    pass.read(resources.hdrScene[frame], VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    pass.execute([this, frame, groupsX, groupsY, push](VkCommandBuffer cmd)
    {
        vkCmdFillBuffer(cmd, statsBuffers[frame], 0, sizeof(Stats), 0);

        VkBufferMemoryBarrier2 clearBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        clearBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        clearBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        clearBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.buffer = statsBuffers[frame];
        clearBarrier.offset = 0;
        clearBarrier.size = sizeof(Stats);

        VkDependencyInfo clearDependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        clearDependency.bufferMemoryBarrierCount = 1;
        clearDependency.pBufferMemoryBarriers = &clearBarrier;
        vkCmdPipelineBarrier2(cmd, &clearDependency);

        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                0, 1, &sets[frame], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        // compute 写 -> 主机读。显存是 HOST_COHERENT 的，屏障只负责让写完成。
        VkBufferMemoryBarrier2 hostBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer = statsBuffers[frame];
        hostBarrier.offset = 0;
        hostBarrier.size = sizeof(Stats);

        VkDependencyInfo hostDependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        hostDependency.bufferMemoryBarrierCount = 1;
        hostDependency.pBufferMemoryBarriers = &hostBarrier;
        vkCmdPipelineBarrier2(cmd, &hostDependency);
    });
}

bool AutoExposurePass::selfTest()
{
    // 1. 朝目标收敛，且永不过冲。
    float value = 1.0f;
    for (int i = 0; i < 200; ++i)
        value = adaptExposure(value, 8.0f, 1.0f / 60.0f, kAdaptSpeed);
    const bool converged = value > 7.0f && value <= 8.0f;

    // 2. 单调：时间步越大越接近目标。
    const float slow = adaptExposure(1.0f, 4.0f, 0.01f, kAdaptSpeed);
    const float fast = adaptExposure(1.0f, 4.0f, 0.2f, kAdaptSpeed);
    const bool monotonic = slow > 1.0f && slow < fast && fast <= 4.0f;

    // 3. 目标不变时不漂。
    const bool stable = std::abs(adaptExposure(2.5f, 2.5f, 0.2f, kAdaptSpeed) - 2.5f) < 1e-5f;

    // 4. 夹取：极端目标不会把曝光推到 0 或无穷。
    const bool clamped = adaptExposure(1.0f, 1e6f, 10.0f, 10.0f) <= kMaxExposure
                      && adaptExposure(1.0f, 1e-6f, 10.0f, 10.0f) >= kMinExposure;

    // 5. 坏输入（0 / 负数 / 负 dt）不产生 NaN。
    const float bad = adaptExposure(0.0f, -1.0f, -1.0f, kAdaptSpeed);
    const bool robust = bad >= kMinExposure && bad <= kMaxExposure;

    const bool ok = converged && monotonic && stable && clamped && robust;
    if (ok)
        std::printf("[Exposure] selfTest PASS | 收敛 + 单调 + 不漂 + 夹取 + 坏输入\n");
    return ok;
}
