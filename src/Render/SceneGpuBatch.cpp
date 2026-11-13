#include "Render/SceneGpuBatch.h"

#include "AssetPaths.h"
#include "Core/MaterialSlot.h"
#include "RHI/CommandUtils.h"
#include "Scene/Model.h"

#include "Core/FrameInfo.h"
#include "Scene/Components.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace
{
constexpr uint32_t kMinInstanceCapacity = 512;
constexpr uint32_t kMinGroupCapacity = 96;
constexpr uint32_t kMaxCachedModels = 64;

struct CullPushConstants
{
    glm::uvec4 params{ 0u };   // x=groupCount
};
static_assert(sizeof(CullPushConstants) == 16, "scene_cull.comp push constant layout mismatch");

// 与着色器共享的记录布局。任何字段变动都必须两边一起改。
static_assert(sizeof(SceneGpuBatch::InstanceRecord) == 224,
              "InstanceRecord must match scene_cull.comp / mrt.vert");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, flags) == 128,
              "InstanceRecord.flags offset mismatch");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, draw) == 208,
              "InstanceRecord.draw offset mismatch");

static_assert(sizeof(SceneGpuBatch::GroupRecord) == 64,
              "GroupRecord must match scene_cull.comp");
static_assert(offsetof(SceneGpuBatch::GroupRecord, draw) == 32,
              "GroupRecord.draw offset mismatch");
static_assert(offsetof(SceneGpuBatch::GroupRecord, counts) == 48,
              "GroupRecord.counts offset mismatch");

static_assert(sizeof(SceneGpuBatch::FrameData) == 256,
              "FrameData must match scene_cull.comp");

// 组的世界 AABB 由实例的局部 AABB 变换后求并集；少于这个数的组不值得单独剔除。
constexpr uint32_t kCommandWords = 5u;
// Hi-Z 金字塔最多 32 级（8K 分辨率也只有 14 级），描述符池按上限预留。
constexpr uint32_t kMaxHiZLevels = 32u;
constexpr float kHiZOcclusionBias = 0.05f;   // 线性视深度的保守余量，单位是世界单位

// V-04：体素 chunk 的几何已经在 TerrainMeshArena 里，由 GeometrySystem 的
// 专用间接批提交。这里必须排除，否则合并缓冲会再拷一份 CPU 几何，而
// Model 的 GPU 缓冲已被释放，逐模型绑定会拿到 VK_NULL_HANDLE。
bool isArenaBackedVoxel(entt::registry& registry, entt::entity entity)
{
    const auto* voxel = registry.try_get<VoxelChunkComponent>(entity);
    return voxel != nullptr && voxel->arenaIndexBase != 0xFFFFFFFFu;
}
} // namespace

SceneGpuBatch::SceneGpuBatch(Device& device)
    : vulkanDevice(device)
{
    // 每组建模剔除 + 一条 multidraw：不再需要 vkCmdDrawIndexedIndirectCount 的计数缓冲，
    // 所以门槛降到 multiDrawIndirect。没有它就只能逐组 drawCount=1 循环（仍然只有一条命令/组）。
    available = vulkanDevice.supportsMultiDrawIndirect();
    if (!available)
    {
        std::cout << "[SceneGpu] GPU batch unavailable | multiDrawIndirect=0; falling back to CPU submesh draws" << std::endl;
        return;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = vulkanDevice.findPhysicalQueueFamilies().graphicsFamily.value();
    if (vkCreateCommandPool(vulkanDevice.getDevice(), &poolInfo, nullptr, &uploadPool) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: command pool creation failed");

    createLayoutsAndPipeline();
    std::cout << "[SceneGpu] GPU batch ready | instancesPerFrame=" << instanceCapacity
              << " groupsPerFrame=" << groupCapacity << " multiDrawIndirect=1" << std::endl;
}

SceneGpuBatch::~SceneGpuBatch()
{
    destroyResources();

    if (uploadPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(vulkanDevice.getDevice(), uploadPool, nullptr);
        uploadPool = VK_NULL_HANDLE;
    }
}

bool SceneGpuBatch::useGpuPath(const FrameInfo& frameInfo) const
{
    if (!available || !frameInfo.settings.gpuSceneEnabled)
        return false;
    // 跨模型 single draw 的前提是合并缓冲：没有它只能逐模型 bind，退回 CPU 路径更简单。
    if (!mergedActive)
        return false;
    if (frameInfo.settings.gpuSceneForce)
        return true;
    // 阈值按「实例数」算：小场景（Sponza 103 个子网格）走 CPU 逐子网格反而更省。
    return stats.submitted >= frameInfo.settings.gpuSceneMinInstances;
}

void SceneGpuBatch::createBuffer(GpuBuffer& buffer, VkDeviceSize size,
                                 VkBufferUsageFlags usage, bool hostVisible)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.flags = hostVisible
        ? (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
        : 0u;

    VmaAllocationInfo allocationInfo{};
    if (vmaCreateBuffer(vulkanDevice.getVmaAllocator(), &bufferInfo, &allocInfo,
                        &buffer.buffer, &buffer.allocation, &allocationInfo) != VK_SUCCESS)
    {
        throw std::runtime_error("SceneGpuBatch: buffer creation failed");
    }
    buffer.mapped = allocationInfo.pMappedData;
    buffer.size = size;
    if (hostVisible && !buffer.mapped)
        throw std::runtime_error("SceneGpuBatch: host-visible buffer is not mapped");
}

void SceneGpuBatch::destroyBuffer(GpuBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(vulkanDevice.getVmaAllocator(), buffer.buffer, buffer.allocation);
    buffer = {};
}
void SceneGpuBatch::createLayoutsAndPipeline()
{
    // compute：组记录 / 命令 / 帧数据 / 统计
    VkDescriptorSetLayoutBinding computeBindings[5]{};
    for (uint32_t i = 0; i < 4u; ++i)
    {
        computeBindings[i].binding = i;
        computeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        computeBindings[i].descriptorCount = 1;
        computeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // binding 4 = Hi-Z 金字塔（遮挡剔除用；图像还没建时描述符先空着）。
    computeBindings[4].binding = 4;
    computeBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    computeBindings[4].descriptorCount = 1;
    computeBindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo computeLayoutInfo{};
    computeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    computeLayoutInfo.bindingCount = 5;
    computeLayoutInfo.pBindings = computeBindings;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &computeLayoutInfo, nullptr,
                                    &computeSetLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor layout failed");

    // 图形：set 3 binding 0 = 实例缓冲（顶点/片元都读）
    VkDescriptorSetLayoutBinding instanceBinding{};
    instanceBinding.binding = 0;
    instanceBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceBinding.descriptorCount = 1;
    instanceBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo instanceLayoutInfo{};
    instanceLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    instanceLayoutInfo.bindingCount = 1;
    instanceLayoutInfo.pBindings = &instanceBinding;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &instanceLayoutInfo, nullptr,
                                    &instanceSetLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: instance descriptor layout failed");
    VkDescriptorPoolSize computePoolSizes[2]{};
    computePoolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    computePoolSizes[0].descriptorCount = 4u * kFramesInFlight;
    computePoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    computePoolSizes[1].descriptorCount = kFramesInFlight;
    VkDescriptorPoolCreateInfo computePoolInfo{};
    computePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    computePoolInfo.maxSets = kFramesInFlight;
    computePoolInfo.poolSizeCount = 2;
    computePoolInfo.pPoolSizes = computePoolSizes;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &computePoolInfo, nullptr,
                               &computePool) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor pool failed");

    VkDescriptorPoolSize graphicsPoolSize{};
    graphicsPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    graphicsPoolSize.descriptorCount = kFramesInFlight;
    VkDescriptorPoolCreateInfo graphicsPoolInfo{};
    graphicsPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    graphicsPoolInfo.maxSets = kFramesInFlight;
    graphicsPoolInfo.poolSizeCount = 1;
    graphicsPoolInfo.pPoolSizes = &graphicsPoolSize;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &graphicsPoolInfo, nullptr,
                               &graphicsPool) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: graphics descriptor pool failed");

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(CullPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &computeSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &layoutInfo, nullptr,
                               &computePipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute pipeline layout failed");

    // 显式写全限定路径：宏与字面量相邻，避免被拼成别的目录名。
    cullPipeline = std::make_unique<ComputePipeline>(
        vulkanDevice, TINY_SHADER_DIR "scene_cull.comp.spv", computePipelineLayout);

    // Hi-Z：两套 compute 共用「采样源 + 存储图像」布局。构建用 80 字节 push
    // （view 矩阵 + 尺寸），降采样只用 16 字节，范围取大的那个。
    VkDescriptorSetLayoutBinding hizBindings[2]{};
    hizBindings[0].binding = 0;
    hizBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    hizBindings[0].descriptorCount = 1;
    hizBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    hizBindings[1].binding = 1;
    hizBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    hizBindings[1].descriptorCount = 1;
    hizBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo hizLayoutInfo{};
    hizLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    hizLayoutInfo.bindingCount = 2;
    hizLayoutInfo.pBindings = hizBindings;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &hizLayoutInfo, nullptr,
                                    &hizSetLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: Hi-Z descriptor layout failed");

    VkPushConstantRange hizPushRange{};
    hizPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    hizPushRange.offset = 0;
    hizPushRange.size = sizeof(glm::mat4) + sizeof(glm::uvec4);
    VkPipelineLayoutCreateInfo hizPipelineInfo{};
    hizPipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    hizPipelineInfo.setLayoutCount = 1;
    hizPipelineInfo.pSetLayouts = &hizSetLayout;
    hizPipelineInfo.pushConstantRangeCount = 1;
    hizPipelineInfo.pPushConstantRanges = &hizPushRange;
    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &hizPipelineInfo, nullptr,
                               &hizPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: Hi-Z pipeline layout failed");

    VkDescriptorPoolSize hizPoolSizes[2]{};
    hizPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    hizPoolSizes[0].descriptorCount = kFramesInFlight * kMaxHiZLevels;
    hizPoolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    hizPoolSizes[1].descriptorCount = kFramesInFlight * kMaxHiZLevels;
    VkDescriptorPoolCreateInfo hizPoolInfo{};
    hizPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    hizPoolInfo.maxSets = kFramesInFlight * kMaxHiZLevels;
    hizPoolInfo.poolSizeCount = 2;
    hizPoolInfo.pPoolSizes = hizPoolSizes;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &hizPoolInfo, nullptr,
                               &hizPool) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: Hi-Z descriptor pool failed");

    hizBuildPipeline = std::make_unique<ComputePipeline>(
        vulkanDevice, TINY_SHADER_DIR "hiz_build.comp.spv", hizPipelineLayout);
    hizDownsamplePipeline = std::make_unique<ComputePipeline>(
        vulkanDevice, TINY_SHADER_DIR "hiz_downsample.comp.spv", hizPipelineLayout);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(vulkanDevice.getDevice(), &samplerInfo, nullptr,
                        &hizSampler) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: Hi-Z sampler failed");

    ensureCapacity(kMinInstanceCapacity, kMinGroupCapacity);
}
void SceneGpuBatch::ensureCapacity(uint32_t instanceNeed, uint32_t groupNeed)
{
    if (instanceCapacity >= instanceNeed && groupCapacity >= groupNeed)
        return;

    const uint32_t newInstances =
        std::max(instanceNeed, std::max(instanceCapacity * 2u, kMinInstanceCapacity));
    const uint32_t newGroups =
        std::max(groupNeed, std::max(groupCapacity * 2u, kMinGroupCapacity));
    if (newInstances == instanceCapacity && newGroups == groupCapacity)
        return;

    // 旧缓冲上可能还有在飞的帧，重建前先等设备空闲：容量翻倍是低频事件，
    // 换来的是「不需要同时维护两代资源的生命周期」。
    vkDeviceWaitIdle(vulkanDevice.getDevice());
    destroyFrameBuffers();
    instanceCapacity = newInstances;
    groupCapacity = newGroups;
    createFrameBuffers();
    writeDescriptors();
    // 缓冲重建过：两个槽位都得重新装一次结构数据。
    ++capacityGeneration;
}

void SceneGpuBatch::createFrameBuffers()
{
    // 容量增长时旧描述符集还挂在池上：先整体重置，否则分配会失败（池里的 set 未归还）。
    vkResetDescriptorPool(vulkanDevice.getDevice(), computePool, 0);
    vkResetDescriptorPool(vulkanDevice.getDevice(), graphicsPool, 0);
    std::array<VkDescriptorSetLayout, kFramesInFlight> computeLayouts{};
    computeLayouts.fill(computeSetLayout);
    VkDescriptorSetAllocateInfo computeAlloc{};
    computeAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    computeAlloc.descriptorPool = computePool;
    computeAlloc.descriptorSetCount = kFramesInFlight;
    computeAlloc.pSetLayouts = computeLayouts.data();
    std::array<VkDescriptorSet, kFramesInFlight> computeSets{};
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &computeAlloc, computeSets.data()) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor allocation failed");

    std::array<VkDescriptorSetLayout, kFramesInFlight> graphicsLayouts{};
    graphicsLayouts.fill(instanceSetLayout);
    VkDescriptorSetAllocateInfo graphicsAlloc{};
    graphicsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    graphicsAlloc.descriptorPool = graphicsPool;
    graphicsAlloc.descriptorSetCount = kFramesInFlight;
    graphicsAlloc.pSetLayouts = graphicsLayouts.data();
    std::array<VkDescriptorSet, kFramesInFlight> graphicsSets{};
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &graphicsAlloc, graphicsSets.data()) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: instance descriptor allocation failed");

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        FrameResources& res = frames[frame];
        createBuffer(res.instances,
                     static_cast<VkDeviceSize>(instanceCapacity) * sizeof(InstanceRecord),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(res.commands,
                     static_cast<VkDeviceSize>(groupCapacity) * kCommandWords * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
        createBuffer(res.groups,
                     static_cast<VkDeviceSize>(groupCapacity) * sizeof(GroupRecord),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(res.frameData, sizeof(FrameData),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(res.stats, 12u * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        std::memset(res.stats.mapped, 0, static_cast<size_t>(res.stats.size));
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.stats.allocation, 0, VK_WHOLE_SIZE);
        res.computeSet = computeSets[frame];
        res.graphicsSet = graphicsSets[frame];
    }
}

void SceneGpuBatch::writeDescriptors()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        FrameResources& res = frames[frame];
        VkDescriptorBufferInfo infos[4]{};
        infos[0] = { res.groups.buffer, 0, VK_WHOLE_SIZE };
        infos[1] = { res.commands.buffer, 0, VK_WHOLE_SIZE };
        infos[2] = { res.frameData.buffer, 0, VK_WHOLE_SIZE };
        infos[3] = { res.stats.buffer, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = res.computeSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 4, writes, 0, nullptr);
        // binding 4 指向「另一个帧槽」的 Hi-Z：本帧剔除读的是上一帧建好的那份。
        const HiZFrame& readHiZ = hiz[(frame + 1u) % kFramesInFlight];
        if (readHiZ.fullView != VK_NULL_HANDLE)
        {
            VkDescriptorImageInfo hizInfo{};
            hizInfo.sampler = hizSampler;
            hizInfo.imageView = readHiZ.fullView;
            hizInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet hizWrite{};
            hizWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            hizWrite.dstSet = res.computeSet;
            hizWrite.dstBinding = 4;
            hizWrite.descriptorCount = 1;
            hizWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            hizWrite.pImageInfo = &hizInfo;
            vkUpdateDescriptorSets(vulkanDevice.getDevice(), 1, &hizWrite, 0, nullptr);
        }

        VkDescriptorBufferInfo instanceInfo{ res.instances.buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet instanceWrite{};
        instanceWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        instanceWrite.dstSet = res.graphicsSet;
        instanceWrite.dstBinding = 0;
        instanceWrite.descriptorCount = 1;
        instanceWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instanceWrite.pBufferInfo = &instanceInfo;
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 1, &instanceWrite, 0, nullptr);
    }
}

void SceneGpuBatch::destroyFrameBuffers()
{
    for (FrameResources& res : frames)
    {
        destroyBuffer(res.instances);
        destroyBuffer(res.commands);
        destroyBuffer(res.groups);
        destroyBuffer(res.frameData);
        destroyBuffer(res.stats);
        res.computeSet = VK_NULL_HANDLE;
        res.graphicsSet = VK_NULL_HANDLE;
    }
}

void SceneGpuBatch::destroyResources()
{
    destroyHiZ();
    destroyMergedMesh();
    destroyFrameBuffers();
    cullPipeline.reset();
    if (computePipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vulkanDevice.getDevice(), computePipelineLayout, nullptr);
    computePipelineLayout = VK_NULL_HANDLE;
    hizBuildPipeline.reset();
    hizDownsamplePipeline.reset();
    if (hizPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vulkanDevice.getDevice(), hizPipelineLayout, nullptr);
    hizPipelineLayout = VK_NULL_HANDLE;
    if (hizSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vulkanDevice.getDevice(), hizSetLayout, nullptr);
    hizSetLayout = VK_NULL_HANDLE;
    if (hizPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), hizPool, nullptr);
    hizPool = VK_NULL_HANDLE;
    if (hizSampler != VK_NULL_HANDLE)
        vkDestroySampler(vulkanDevice.getDevice(), hizSampler, nullptr);
    hizSampler = VK_NULL_HANDLE;
    if (computePool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), computePool, nullptr);
    computePool = VK_NULL_HANDLE;
    if (graphicsPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vulkanDevice.getDevice(), graphicsPool, nullptr);
    graphicsPool = VK_NULL_HANDLE;
    if (computeSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vulkanDevice.getDevice(), computeSetLayout, nullptr);
    computeSetLayout = VK_NULL_HANDLE;
    if (instanceSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vulkanDevice.getDevice(), instanceSetLayout, nullptr);
    instanceSetLayout = VK_NULL_HANDLE;
}

const std::vector<SceneGpuBatch::SubMeshBounds>& SceneGpuBatch::subMeshBoundsFor(const Model& model, uint64_t frameNumber)
{
    const uint64_t geometryId = model.getGeometryId();
    auto it = modelBoundsCache.find(geometryId);
    if (it != modelBoundsCache.end())
    {
        it->second.lastFrame = frameNumber;
        return it->second.bounds;
    }

    // 缓存只按模型数量增长；模型通常只有几十个，超过上限时整表丢弃重算，
    // 避免长期运行后持有已卸载模型的指针。
    if (modelBoundsCache.size() >= kMaxCachedModels)
    {
        // 先按「最近 600 帧内出现过」裁剪，剪不动再整表丢弃：模型销毁不会有显式通知，
        // 只能靠使用痕迹回收，避免长期运行后持有已卸载模型的包围盒。
        for (auto cached = modelBoundsCache.begin(); cached != modelBoundsCache.end(); )
        {
            if (frameNumber > cached->second.lastFrame + 600u)
                cached = modelBoundsCache.erase(cached);
            else
                ++cached;
        }
        if (modelBoundsCache.size() >= kMaxCachedModels)
            modelBoundsCache.clear();
    }

    std::vector<SubMeshBounds> bounds(model.getSubMeshes().size());

    const auto& positions = model.getPickingPositions();
    const auto& indices = model.getPickingIndices();
    if (!positions.empty() && !indices.empty())
    {
        for (size_t i = 0; i < bounds.size(); ++i)
        {
            const SubMesh& subMesh = model.getSubMeshes()[i];
            const size_t first = subMesh.firstIndex;
            const size_t end = std::min<size_t>(first + subMesh.indexCount, indices.size());
            glm::vec3 mn(std::numeric_limits<float>::max());
            glm::vec3 mx(-std::numeric_limits<float>::max());
            bool any = false;
            for (size_t k = first; k < end; ++k)
            {
                const uint32_t vertexIndex = indices[k];
                if (vertexIndex >= positions.size())
                    continue;
                const glm::vec3& p = positions[vertexIndex];
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
                any = true;
            }
            if (any)
            {
                bounds[i].min = mn;
                bounds[i].max = mx;
                bounds[i].valid = true;
            }
        }
    }

    CachedBounds cached{};
    cached.bounds = std::move(bounds);
    cached.lastFrame = frameNumber;
    auto result = modelBoundsCache.emplace(geometryId, std::move(cached));
    return result.first->second.bounds;
}

void SceneGpuBatch::forgetModel(const Model* model)
{
    if (model != nullptr)
        modelBoundsCache.erase(model->getGeometryId());
}

void SceneGpuBatch::uploadDeviceLocal(GpuBuffer& destination, const void* data, VkDeviceSize size)
{
    GpuBuffer staging{};
    createBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), staging.allocation, 0, VK_WHOLE_SIZE);

    VkCommandBuffer commandBuffer = cmdBeginSingleTime(vulkanDevice.getDevice(), uploadPool);
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(commandBuffer, staging.buffer, destination.buffer, 1, &copy);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    cmdEndSingleTime(vulkanDevice.getDevice(), vulkanDevice.getGraphicsQueue(),
                    uploadPool, commandBuffer);
    destroyBuffer(staging);
}

void SceneGpuBatch::destroyMergedMesh()
{
    destroyBuffer(mergedMesh.vertices);
    destroyBuffer(mergedMesh.indices);
    mergedMesh.signature = 0;
    mergedMesh.valid = false;
    mergedPlacements.clear();
    mergedActive = false;
}

bool SceneGpuBatch::ensureMergedMesh(const std::vector<const Model*>& models)
{
    if (models.empty())
        return false;

    // 签名覆盖模型集合与顶点/索引规模：换了模型或规模变了才重建。
    uint64_t signature = 1469598103934665603ull;
    for (const Model* model : models)
    {
        const uint64_t values[] = {
            model->getGeometryId(),
            static_cast<uint64_t>(model->getVertexCount()),
            static_cast<uint64_t>(model->getIndexCount())
        };
        for (uint64_t value : values)
        {
            signature ^= value;
            signature *= 1099511628211ull;
        }
    }
    if (mergedMesh.valid && mergedMesh.signature == signature)
        return true;

    size_t totalVertices = 0;
    size_t totalIndices = 0;
    for (const Model* model : models)
    {
        if (model->getCpuVertices().size() != model->getVertexCount() ||
            model->getCpuIndices().size() != model->getIndexCount() ||
            model->getCpuVertices().empty())
        {
            if (!warnedNoMergedData)
            {
                warnedNoMergedData = true;
                std::cout << "[SceneGpu] merged buffers unavailable | model without CPU geometry;"
                             " falling back to per-model indirect draws" << std::endl;
            }
            destroyMergedMesh();
            return false;
        }
        totalVertices += model->getCpuVertices().size();
        totalIndices += model->getCpuIndices().size();
    }

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(totalVertices);
    indices.reserve(totalIndices);
    mergedPlacements.clear();
    for (const Model* model : models)
    {
        MergedPlacement placement{};
        placement.firstIndex = static_cast<uint32_t>(indices.size());
        placement.baseVertex = static_cast<uint32_t>(vertices.size());
        mergedPlacements.emplace(model->getGeometryId(), placement);
        vertices.insert(vertices.end(), model->getCpuVertices().begin(),
                        model->getCpuVertices().end());
        indices.insert(indices.end(), model->getCpuIndices().begin(),
                       model->getCpuIndices().end());
    }

    destroyBuffer(mergedMesh.vertices);
    destroyBuffer(mergedMesh.indices);
    createBuffer(mergedMesh.vertices, sizeof(Vertex) * vertices.size(),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    createBuffer(mergedMesh.indices, sizeof(uint32_t) * indices.size(),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    uploadDeviceLocal(mergedMesh.vertices, vertices.data(), sizeof(Vertex) * vertices.size());
    uploadDeviceLocal(mergedMesh.indices, indices.data(), sizeof(uint32_t) * indices.size());
    mergedMesh.signature = signature;
    mergedMesh.valid = true;

    std::cout << "[SceneGpu] merged buffers built | vertices=" << vertices.size()
              << " indices=" << indices.size()
              << " vertexMB=" << (sizeof(Vertex) * vertices.size()) / (1024u * 1024u)
              << " indexMB=" << (sizeof(uint32_t) * indices.size()) / (1024u * 1024u)
              << std::endl;
    return true;
}

uint64_t SceneGpuBatch::computeStructureSignature(const FrameInfo& frameInfo) const
{
    // FNV-1a: 只读结构相关字段（实体、变换版本、材质、局部包围盒），不碰矩阵、不算 AABB。
    // 命中同一个签名时 buildInstances 整段跳过，稳定帧只剩 frameData 上传与剔除 dispatch。
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&signature](uint64_t value)
    {
        signature ^= value;
        signature *= 1099511628211ull;
    };

    auto view = frameInfo.registry.view<MeshComponent, WorldTransformComponent>();
    for (entt::entity entity : view)
    {
        const MeshComponent& mesh = view.get<MeshComponent>(entity);
        const WorldTransformComponent& worldTransform = view.get<WorldTransformComponent>(entity);
        if (!mesh.model || !worldTransform.valid || mesh.model->getIndexCount() == 0)
            continue;
        const RenderTagComponent* tag = frameInfo.registry.try_get<RenderTagComponent>(entity);
        if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain))
            continue;
        if (isArenaBackedVoxel(frameInfo.registry, entity))
            continue;

        const MaterialComponent* materialComp = frameInfo.registry.try_get<MaterialComponent>(entity);
        const uint64_t materialSlot = materialComp
            ? static_cast<uint64_t>(materialComp->materialSetIndex)
            : static_cast<uint64_t>(kInvalidMaterialSlot);
        const uint64_t materialPointer = materialComp && materialComp->material
            ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(materialComp->material.get()))
            : 0ull;
        mix(static_cast<uint64_t>(entity));
        mix(worldTransform.sourceRevision);
        mix(materialSlot);
        mix(materialPointer);
        mix(mesh.model->getGeometryId());
        mix(static_cast<uint64_t>(mesh.model->getSubMeshes().size()));

        const BoundsComponent* boundsComp = frameInfo.registry.try_get<BoundsComponent>(entity);
        if (boundsComp && boundsComp->getLocal().valid)
        {
            const AABB& local = boundsComp->getLocal();
            uint32_t bits[6] = {};
            std::memcpy(bits, &local.min, 3u * sizeof(float));
            std::memcpy(bits + 3, &local.max, 3u * sizeof(float));
            for (uint32_t value : bits)
                mix(static_cast<uint64_t>(value));
        }
    }

    // 外部实例每帧都可能变（粒子在动），变换本身进签名。
    mix(static_cast<uint64_t>(externalInstances.size()));
    for (const ExternalInstance& external : externalInstances)
    {
        if (external.model == nullptr)
        {
            mix(0xFFFFull);
            continue;
        }
        mix(external.model->getGeometryId());
        mix(static_cast<uint64_t>(external.subMeshIndex));
        mix(static_cast<uint64_t>(external.materialSlot));
        uint32_t bits[16] = {};
        std::memcpy(bits, &external.transform, sizeof(bits));
        for (uint32_t value : bits)
            mix(static_cast<uint64_t>(value));
    }

    // 合并/非合并会改变分组与命令写法，也属于结构。
    mix(frameInfo.settings.gpuMergedBuffers ? 1ull : 0ull);
    return signature;
}

void SceneGpuBatch::refreshDebugExternalInstances(FrameInfo& frameInfo)
{
    if (frameInfo.settings.gpuSceneExternalDebugCount == debugExternalCount)
        return;
    debugExternalCount = frameInfo.settings.gpuSceneExternalDebugCount;
    externalInstances.clear();
    if (debugExternalCount == 0u)
        return;

    // 用第一个可批子网格当模板：只验证「外部实例 -> 分组 -> 合并缓冲 -> 剔除 -> 绘制」这条链。
    const Model* templateModel = nullptr;
    uint32_t templateSubMesh = 0;
    glm::mat4 templateTransform(1.0f);
    auto view = frameInfo.registry.view<MeshComponent, WorldTransformComponent>();
    for (entt::entity entity : view)
    {
        const MeshComponent& mesh = view.get<MeshComponent>(entity);
        const WorldTransformComponent& worldTransform = view.get<WorldTransformComponent>(entity);
        if (!mesh.model || !worldTransform.valid || mesh.model->getIndexCount() == 0)
            continue;
        const RenderTagComponent* tag = frameInfo.registry.try_get<RenderTagComponent>(entity);
        if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain))
            continue;
        const std::vector<SubMesh>& subMeshes = mesh.model->getSubMeshes();
        for (uint32_t i = 0; i < static_cast<uint32_t>(subMeshes.size()); ++i)
        {
            if (subMeshes[i].isTerrainChunk || subMeshes[i].indexCount == 0)
                continue;
            templateModel = mesh.model.get();
            templateSubMesh = i;
            templateTransform = worldTransform.matrix;
            break;
        }
        if (templateModel != nullptr)
            break;
    }
    if (templateModel == nullptr)
        return;

    for (uint32_t i = 0; i < debugExternalCount; ++i)
    {
        ExternalInstance instance{};
        instance.model = templateModel;
        instance.subMeshIndex = templateSubMesh;
        instance.materialSlot = 0u;
        glm::mat4 offset(1.0f);
        offset[3][0] = 2.5f * static_cast<float>(i + 1u);
        instance.transform = templateTransform * offset;
        externalInstances.push_back(instance);
    }
    std::cout << "[SceneGpu] external instances | count=" << externalInstances.size()
              << " source=debug-injection" << std::endl;
}

void SceneGpuBatch::buildInstances(FrameInfo& frameInfo)
{
    instanceScratch.clear();
    modelGroups.clear();
    buckets.clear();

    // 1) 收集可批实体，同时得到参与合并的模型集合（集合变了才重建合并缓冲）。
    std::vector<const Model*> batchModels;
    std::vector<entt::entity> entities;
    {
        std::unordered_set<const Model*> seenModels;
        auto collectView = frameInfo.registry.view<MeshComponent, WorldTransformComponent>();
        for (entt::entity entity : collectView)
        {
            const MeshComponent& mesh = collectView.get<MeshComponent>(entity);
            const WorldTransformComponent& worldTransform =
                collectView.get<WorldTransformComponent>(entity);
            if (!mesh.model || !worldTransform.valid || mesh.model->getIndexCount() == 0)
                continue;
            // 蒙皮模型每帧改写顶点，烘焙进静态合并缓冲会永远停在某一帧。
            if (mesh.model->isDynamic())
                continue;
            const RenderTagComponent* tag =
                frameInfo.registry.try_get<RenderTagComponent>(entity);
            if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain))
                continue;
            if (isArenaBackedVoxel(frameInfo.registry, entity))
                continue;
            entities.push_back(entity);
            if (seenModels.insert(mesh.model.get()).second)
                batchModels.push_back(mesh.model.get());
        }
    }
    // 外部实例（粒子/体素）也参与同一套分组：把它们用的模型一并纳入合并缓冲。
    {
        std::unordered_set<const Model*> seenModels(batchModels.begin(), batchModels.end());
        for (const ExternalInstance& external : externalInstances)
        {
            if (external.model == nullptr || external.model->getIndexCount() == 0)
                continue;
            if (external.model->isDynamic())
                continue;
            if (external.subMeshIndex >= external.model->getSubMeshes().size())
                continue;
            if (seenModels.insert(external.model).second)
                batchModels.push_back(external.model);
        }
    }
    mergedActive = frameInfo.settings.gpuMergedBuffers && ensureMergedMesh(batchModels);

    // 2) 每个实体按（模型, 子网格）归组：同一子网格被多个实体复用时实例连续排列，
    //    一条命令 instanceCount = N 就画完（实例化），而不是每实例一条命令。
    std::map<std::pair<const Model*, uint32_t>, uint32_t> groupIndex;
    auto view = frameInfo.registry.view<MeshComponent, WorldTransformComponent>();
    for (entt::entity entity : entities)
    {
        const MeshComponent& mesh = view.get<MeshComponent>(entity);
        const WorldTransformComponent& worldTransform = view.get<WorldTransformComponent>(entity);
        const Model* model = mesh.model.get();
        const std::vector<SubMesh>& subMeshes = model->getSubMeshes();
        const std::vector<SubMeshBounds>& boundsList = subMeshBoundsFor(*model, frameInfo.frameNumber);

        const MaterialComponent* materialComp = frameInfo.registry.try_get<MaterialComponent>(entity);
        const uint32_t objectMaterialSlot = materialComp ? materialComp->materialSetIndex
                                                         : kInvalidMaterialSlot;
        const bool entityAlbedoFallback = materialComp && materialComp->material &&
                                          !materialComp->material->albedoMap;

        float checkerScale = 1.0f;
        const BoundsComponent* boundsComp = frameInfo.registry.try_get<BoundsComponent>(entity);
        if (boundsComp && boundsComp->getLocal().valid)
        {
            const AABB& local = boundsComp->getLocal();
            const glm::vec3 extent = local.max - local.min;
            const float maxExtent = std::max(extent.x, std::max(extent.y, extent.z));
            if (maxExtent > 1e-6f)
                checkerScale = 6.0f / maxExtent;
        }

        glm::vec4 uvTransform(0.0f);
        if (model->hasUv())
        {
            const glm::vec2 uvMin = model->getUvMin();
            const glm::vec2 uvMax = model->getUvMax();
            const glm::vec2 uvExtent = uvMax - uvMin;
            if (uvExtent.x > 0.8f && uvExtent.y > 0.8f)
                uvTransform = glm::vec4(1.0f / uvExtent.x, 1.0f / uvExtent.y, uvMin.x, uvMin.y);
        }

        for (uint32_t i = 0; i < static_cast<uint32_t>(subMeshes.size()); ++i)
        {
            const SubMesh& subMesh = subMeshes[i];
            if (subMesh.isTerrainChunk || subMesh.indexCount == 0)
                continue;

            uint32_t slot = (subMesh.materialSlot != kInvalidMaterialSlot)
                ? subMesh.materialSlot
                : objectMaterialSlot;
            bool materialValid = true;
            if (slot == kInvalidMaterialSlot)
            {
                const uint32_t entityId = static_cast<uint32_t>(entt::to_integral(entity));
                if (warnedFallback.insert(entityId).second)
                {
                    std::cout << "[SceneGpu] entity " << entityId
                              << " has no valid material slot; using checker fallback (reported once)"
                              << std::endl;
                }
                slot = 0u;
                materialValid = false;
            }

            const std::pair<const Model*, uint32_t> key{ model, i };
            uint32_t groupIndexValue = 0;
            auto groupIt = groupIndex.find(key);
            if (groupIt == groupIndex.end())
            {
                groupIndexValue = static_cast<uint32_t>(modelGroups.size());
                groupIndex.emplace(key, groupIndexValue);
                ModelGroup newGroup{};
                newGroup.model = model;
                newGroup.subMeshIndex = i;
                newGroup.materialSlot = slot;
                if (mergedActive)
                {
                    const auto placement = mergedPlacements.find(model->getGeometryId());
                    if (placement != mergedPlacements.end())
                    {
                        newGroup.merged = true;
                        newGroup.mergedFirstIndex = placement->second.firstIndex + subMesh.firstIndex;
                        newGroup.mergedBaseVertex = static_cast<int32_t>(placement->second.baseVertex);
                    }
                }
                modelGroups.push_back(newGroup);
                buckets.emplace_back();
            }
            else
            {
                groupIndexValue = groupIt->second;
            }

            // F4：glTF 几何存在它所属节点的局部空间，实例矩阵 = 实体世界矩阵 * 节点世界矩阵。
            // 没有节点信息的模型（OBJ / 地形 / 体素 / 蒙皮）nodeMatrix 保持单位矩阵。
            const bool hasNode = (subMesh.node != kNoModelNode);
            const glm::mat4 nodeMatrix = hasNode ? model->getNodeWorldMatrix(subMesh.node)
                                                 : glm::mat4(1.0f);
            const glm::mat4 instanceMatrix = worldTransform.matrix * nodeMatrix;

            glm::vec3 localMin = model->getBoundsMin();
            glm::vec3 localMax = model->getBoundsMax();
            if (i < boundsList.size() && boundsList[i].valid)
            {
                localMin = boundsList[i].min;
                localMax = boundsList[i].max;
            }
            else if (hasNode)
            {
                glm::vec3 nodeMin(0.0f);
                glm::vec3 nodeMax(0.0f);
                if (model->getNodeBounds(subMesh.node, nodeMin, nodeMax))
                {
                    localMin = nodeMin;
                    localMax = nodeMax;
                }
            }

            InstanceRecord record{};
            record.model = instanceMatrix;
            record.normal = hasNode
                ? glm::mat4(glm::transpose(glm::inverse(glm::mat3(instanceMatrix))))
                : worldTransform.normalMatrix;
            record.flags = glm::uvec4(slot, materialValid ? 1u : 0u,
                                      subMesh.alphaMasked ? 1u : 0u,
                                      (subMesh.albedoFallback || entityAlbedoFallback) ? 1u : 0u);
            record.misc = glm::vec4(subMesh.alphaCutoff, checkerScale, 0.0f, 0.0f);
            record.uvTransform = uvTransform;
            record.boundsMin = glm::vec4(localMin, 1.0f);
            record.boundsMax = glm::vec4(localMax, 0.0f);
            record.draw = glm::uvec4(0u);   // 命令由 compute 写，这里不再塞绘制参数
            buckets[groupIndexValue].push_back(record);

            // 组的并集世界 AABB：8 个角点变换后求并集，供剔除使用。
            ModelGroup& group = modelGroups[groupIndexValue];
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 localPoint((corner & 1) ? localMax.x : localMin.x,
                                           (corner & 2) ? localMax.y : localMin.y,
                                           (corner & 4) ? localMax.z : localMin.z);
                const glm::vec3 worldPoint = glm::vec3(instanceMatrix * glm::vec4(localPoint, 1.0f));
                if (!group.hasBounds)
                {
                    group.boundsMin = worldPoint;
                    group.boundsMax = worldPoint;
                    group.hasBounds = true;
                }
                else
                {
                    group.boundsMin = glm::min(group.boundsMin, worldPoint);
                    group.boundsMax = glm::max(group.boundsMax, worldPoint);
                }
            }
        }
    }

    // 3) 排序后拼接实例：命令顺序即组顺序。合并模式只绑一次 VBO/IBO，顺序任意；
    //    非合并模式要先按模型排，保证同一模型的组连续，才能一条命令画完一组。
    // 2b) 外部实例：没有 ECS 实体，变换由调用方直接给（粒子/体素）。
    for (const ExternalInstance& external : externalInstances)
    {
        const Model* model = external.model;
        if (model == nullptr || model->getIndexCount() == 0)
            continue;
        const std::vector<SubMesh>& subMeshes = model->getSubMeshes();
        if (external.subMeshIndex >= subMeshes.size())
            continue;
        const SubMesh& subMesh = subMeshes[external.subMeshIndex];
        if (subMesh.isTerrainChunk || subMesh.indexCount == 0)
            continue;

        const std::pair<const Model*, uint32_t> key{ model, external.subMeshIndex };
        uint32_t groupIndexValue = 0;
        auto groupIt = groupIndex.find(key);
        if (groupIt == groupIndex.end())
        {
            groupIndexValue = static_cast<uint32_t>(modelGroups.size());
            groupIndex.emplace(key, groupIndexValue);
            ModelGroup newGroup{};
            newGroup.model = model;
            newGroup.subMeshIndex = external.subMeshIndex;
            newGroup.materialSlot = external.materialSlot;
            if (mergedActive)
            {
                const auto placement = mergedPlacements.find(model->getGeometryId());
                if (placement != mergedPlacements.end())
                {
                    newGroup.merged = true;
                    newGroup.mergedFirstIndex = placement->second.firstIndex + subMesh.firstIndex;
                    newGroup.mergedBaseVertex = static_cast<int32_t>(placement->second.baseVertex);
                }
            }
            modelGroups.push_back(newGroup);
            buckets.emplace_back();
        }
        else
        {
            groupIndexValue = groupIt->second;
        }

        glm::vec3 localMin = model->getBoundsMin();
        glm::vec3 localMax = model->getBoundsMax();
        const std::vector<SubMeshBounds>& boundsList = subMeshBoundsFor(*model, frameInfo.frameNumber);
        if (external.subMeshIndex < boundsList.size() && boundsList[external.subMeshIndex].valid)
        {
            localMin = boundsList[external.subMeshIndex].min;
            localMax = boundsList[external.subMeshIndex].max;
        }

        InstanceRecord record{};
        record.model = external.transform;
        record.normal = glm::mat4(glm::transpose(glm::inverse(glm::mat3(external.transform))));
        record.flags = glm::uvec4(external.materialSlot, 1u, subMesh.alphaMasked ? 1u : 0u,
                                  subMesh.albedoFallback ? 1u : 0u);
        record.misc = glm::vec4(subMesh.alphaCutoff, 1.0f, 0.0f, 0.0f);
        record.uvTransform = glm::vec4(0.0f);
        record.boundsMin = glm::vec4(localMin, 1.0f);
        record.boundsMax = glm::vec4(localMax, 0.0f);
        record.draw = glm::uvec4(0u);
        buckets[groupIndexValue].push_back(record);

        ModelGroup& group = modelGroups[groupIndexValue];
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 localPoint((corner & 1) ? localMax.x : localMin.x,
                                       (corner & 2) ? localMax.y : localMin.y,
                                       (corner & 4) ? localMax.z : localMin.z);
            const glm::vec3 worldPoint = glm::vec3(external.transform * glm::vec4(localPoint, 1.0f));
            if (!group.hasBounds)
            {
                group.boundsMin = worldPoint;
                group.boundsMax = worldPoint;
                group.hasBounds = true;
            }
            else
            {
                group.boundsMin = glm::min(group.boundsMin, worldPoint);
                group.boundsMax = glm::max(group.boundsMax, worldPoint);
            }
        }
    }

    std::vector<uint32_t> order(modelGroups.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(order.size()); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b)
    {
        const ModelGroup& left = modelGroups[a];
        const ModelGroup& right = modelGroups[b];
        if (!mergedActive && left.model != right.model)
            return left.model < right.model;
        if (left.materialSlot != right.materialSlot)
            return left.materialSlot < right.materialSlot;
        return a < b;
    });

    std::vector<ModelGroup> sorted;
    sorted.reserve(modelGroups.size());
    for (uint32_t index : order)
    {
        ModelGroup group = modelGroups[index];
        group.firstInstance = static_cast<uint32_t>(instanceScratch.size());
        group.instanceCount = static_cast<uint32_t>(buckets[index].size());
        instanceScratch.insert(instanceScratch.end(), buckets[index].begin(), buckets[index].end());
        sorted.push_back(group);
    }
    modelGroups.swap(sorted);
    buckets.clear();
}

void SceneGpuBatch::readStats(uint32_t frameInFlightIndex)
{
    const FrameResources& res = frames[frameInFlightIndex];
    if (res.stats.mapped == nullptr)
        return;
    // 这个帧槽上一次使用早已 fence 完成；host 可见内存不保证一致，
    // 必须先 invalidate 才能读到 GPU 写进去的计数（否则永远是 0）。
    vmaInvalidateAllocation(vulkanDevice.getVmaAllocator(), res.stats.allocation,
                            0, VK_WHOLE_SIZE);
    const uint32_t* words = static_cast<const uint32_t*>(res.stats.mapped);
    stats.submitted = words[0];
    stats.visibleGroups = words[1];
    stats.culledGroups = words[2];
    stats.visibleInstances = words[3];
    stats.visibleTriangles = words[4];
    stats.visibleVertices = words[5];
    stats.occlusionTests = words[6];
    stats.occlusionCulled = words[7];
    stats.screenTests = words[8];
    stats.screenCulled = words[9];
}

void SceneGpuBatch::ensureHiZ(VkExtent2D extent)
{
    if (extent.width == 0 || extent.height == 0)
        return;
    if (hizExtent.width == extent.width && hizExtent.height == extent.height)
        return;

    // 重建意味着旧金字塔可能还在飞行中：和容量增长一样，等设备空闲换简单。
    vkDeviceWaitIdle(vulkanDevice.getDevice());
    destroyHiZ();
    hizExtent = extent;

    // 完整 mip 链长度：floor(log2(max(w, h))) + 1。多一级会直接触发
    // VUID-VkImageCreateInfo-mipLevels-00958。
    uint32_t levels = 1;
    uint32_t largest = std::max(extent.width, extent.height);
    while (largest > 1u)
    {
        largest >>= 1u;
        ++levels;
    }
    levels = std::min(levels, kMaxHiZLevels);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        HiZFrame& slot = hiz[frame];
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32_SFLOAT;
        imageInfo.extent = { extent.width, extent.height, 1u };
        imageInfo.mipLevels = levels;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vmaCreateImage(vulkanDevice.getVmaAllocator(), &imageInfo, &allocInfo, &slot.image,
                           &slot.allocation, nullptr) != VK_SUCCESS)
            throw std::runtime_error("SceneGpuBatch: Hi-Z image creation failed");
        slot.extent = extent;
        slot.mipLevels = levels;

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = slot.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = levels;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vulkanDevice.getDevice(), &viewInfo, nullptr, &slot.fullView) != VK_SUCCESS)
            throw std::runtime_error("SceneGpuBatch: Hi-Z view creation failed");

        // 描述符里声明的是 GENERAL，图像一开始就必须真的处于 GENERAL：
        // 否则即使着色器这一帧不采样，提交时也会报 InvalidImageLayout。
        {
            VkCommandBuffer cmd = cmdBeginSingleTime(vulkanDevice.getDevice(), uploadPool);
            VkImageMemoryBarrier toGeneral{};
            toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toGeneral.srcAccessMask = 0;
            toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = slot.image;
            toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &toGeneral);
            cmdEndSingleTime(vulkanDevice.getDevice(), vulkanDevice.getGraphicsQueue(), uploadPool, cmd);
        }

        slot.mipViews.resize(levels, VK_NULL_HANDLE);
        for (uint32_t level = 0; level < levels; ++level)
        {
            VkImageViewCreateInfo mipInfo = viewInfo;
            mipInfo.subresourceRange.baseMipLevel = level;
            mipInfo.subresourceRange.levelCount = 1;
            if (vkCreateImageView(vulkanDevice.getDevice(), &mipInfo, nullptr,
                                  &slot.mipViews[level]) != VK_SUCCESS)
                throw std::runtime_error("SceneGpuBatch: Hi-Z mip view creation failed");
        }
    }

    writeHiZDescriptors();
    // cull 的 binding 4 指向新视图，必须跟着重写。
    writeDescriptors();
}

void SceneGpuBatch::writeHiZDescriptors()
{
    if (hizPool == VK_NULL_HANDLE)
        return;
    vkResetDescriptorPool(vulkanDevice.getDevice(), hizPool, 0);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        HiZFrame& slot = hiz[frame];
        hizSets[frame].assign(slot.mipLevels, VK_NULL_HANDLE);
        if (slot.mipLevels == 0)
            continue;
        std::vector<VkDescriptorSetLayout> layouts(slot.mipLevels, hizSetLayout);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = hizPool;
        alloc.descriptorSetCount = slot.mipLevels;
        alloc.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &alloc,
                                     hizSets[frame].data()) != VK_SUCCESS)
            throw std::runtime_error("SceneGpuBatch: Hi-Z descriptor allocation failed");
        for (uint32_t level = 0; level < slot.mipLevels; ++level)
        {
            // 第 0 级读「本槽位对应的那帧 G-Buffer 位置」，其余级读金字塔本身。
            // 两者在重建时一次写好，录制期不再 vkUpdateDescriptorSets —— 否则会
            // 让还在录制的命令缓冲失效（UNASSIGNED-DrawState-InvalidCommandBuffer）。
            VkDescriptorImageInfo sourceInfo{};
            sourceInfo.sampler = hizSampler;
            if (level == 0u)
            {
                // 两个帧槽位是先后登记的：先到的这一次另一个视图还是空，先用全链视图占位
                //（那次构建会被 buildHiZ 的前置校验挡掉），登记完会整体重写。
                VkImageView positionView = hizPositionViews[(frame + 1u) % kFramesInFlight];
                sourceInfo.imageView = positionView != VK_NULL_HANDLE ? positionView : slot.fullView;
                sourceInfo.imageLayout = positionView != VK_NULL_HANDLE
                                             ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_GENERAL;
            }
            else
            {
                sourceInfo.imageView = slot.fullView;
                sourceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            VkDescriptorImageInfo destInfo{};
            destInfo.imageView = slot.mipViews[level];
            destInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = hizSets[frame][level];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &sourceInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = hizSets[frame][level];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &destInfo;
            vkUpdateDescriptorSets(vulkanDevice.getDevice(), 2, writes, 0, nullptr);
        }
    }
}

void SceneGpuBatch::destroyHiZ()
{
    for (HiZFrame& slot : hiz)
    {
        for (VkImageView view : slot.mipViews)
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(vulkanDevice.getDevice(), view, nullptr);
        }
        slot.mipViews.clear();
        if (slot.fullView != VK_NULL_HANDLE)
            vkDestroyImageView(vulkanDevice.getDevice(), slot.fullView, nullptr);
        if (slot.image != VK_NULL_HANDLE)
            vmaDestroyImage(vulkanDevice.getVmaAllocator(), slot.image, slot.allocation);
        slot = {};
    }
    for (std::vector<VkDescriptorSet>& sets : hizSets)
        sets.clear();
    // G-Buffer 重建时旧视图会被销毁：这里必须把登记过的视图一起清掉，否则
    // 重建描述符时会把已经失效的 VkImageView 写进描述符集（驱动直接崩）。
    hizPositionViews.fill(VK_NULL_HANDLE);
    hizExtent = {};
    hizReady = false;
}

void SceneGpuBatch::setHiZSource(uint32_t frameInFlightIndex, VkImageView positionView,
                              VkExtent2D extent)
{
    if (!available || frameInFlightIndex >= kFramesInFlight)
        return;
    hizPositionViews[frameInFlightIndex] = positionView;
    if (hizExtent.width != extent.width || hizExtent.height != extent.height)
    {
        ensureHiZ(extent);
        return;
    }
    // 尺寸没变但视图换了（重建 G-Buffer）：整体重写一遍描述符。写之前等设备空闲，
    // 因为池里的描述符集可能还被上一帧的命令缓冲引用。
    if (hizExtent.width != 0)
    {
        vkDeviceWaitIdle(vulkanDevice.getDevice());
        writeHiZDescriptors();
    }
}

void SceneGpuBatch::buildHiZ(VkCommandBuffer commandBuffer, const FrameInfo& frameInfo,
                             VkImageView positionView, VkExtent2D extent)
{
    if (!available || !frameInfo.settings.hizEnabled || positionView == VK_NULL_HANDLE)
        return;
    // 源视图在重建时已经写进描述符：槽位 target 的第 0 级读的是「本帧」的 G-Buffer
    // 位置（见 writeHiZDescriptors 的 (frame + 1) % kFramesInFlight 对应关系），
    // 这里兜底校验，防止 resize 之后描述符还指着旧视图。
    if (hizPositionViews[frameInfo.frameInFlightIndex] != positionView)
        return;
    ensureHiZ(extent);
    if (hizExtent.width == 0)
        return;

    // 写到「另一个槽」：本帧剔除读的是上一帧建好的那份，两份轮流当历史。
    const uint32_t target = (frameInfo.frameInFlightIndex + 1u) % kFramesInFlight;
    HiZFrame& slot = hiz[target];
    if (slot.image == VK_NULL_HANDLE || slot.mipLevels == 0 || hizSets[target].empty())
        return;

    // 这份历史上一帧被剔除 dispatch 采样过、本帧也被 SceneGpuCull 采样过；
    // 写之前必须把这些读等完，否则就是 WRITE_AFTER_READ（sync 校验会报）。
    VkImageMemoryBarrier initial{};
    initial.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    initial.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    initial.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    initial.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    initial.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    initial.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    initial.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    initial.image = slot.image;
    initial.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, slot.mipLevels, 0, 1 };
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &initial);

    uint32_t levelWidth = slot.extent.width;
    uint32_t levelHeight = slot.extent.height;
    for (uint32_t level = 0; level < slot.mipLevels; ++level)
    {
        const uint32_t nextWidth = std::max(1u, levelWidth / 2u);
        const uint32_t nextHeight = std::max(1u, levelHeight / 2u);
        const bool first = level == 0;
        const uint32_t targetWidth = first ? levelWidth : nextWidth;
        const uint32_t targetHeight = first ? levelHeight : nextHeight;

        if (first)
        {
            hizBuildPipeline->bind(commandBuffer);
            struct BuildPush { glm::mat4 view; glm::uvec4 params; } push{};
            push.view = frameInfo.viewMatrix;
            push.params = glm::uvec4(levelWidth, levelHeight, targetWidth, targetHeight);
            vkCmdPushConstants(commandBuffer, hizPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
        }
        else
        {
            hizDownsamplePipeline->bind(commandBuffer);
            const glm::uvec4 params(level - 1u, targetWidth, targetHeight, 0u);
            vkCmdPushConstants(commandBuffer, hizPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(params), &params);
        }

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipelineLayout,
                                0, 1, &hizSets[target][level], 0, nullptr);
        vkCmdDispatch(commandBuffer, (targetWidth + 7u) / 8u, (targetHeight + 7u) / 8u, 1u);

        VkImageMemoryBarrier written{};
        written.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        written.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        written.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        written.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        written.image = slot.image;
        written.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &written);

        levelWidth = nextWidth;
        levelHeight = nextHeight;
    }

    hizReady = true;
}

void SceneGpuBatch::prepare(VkCommandBuffer commandBuffer, FrameInfo& frameInfo)
{
    // 每 60 帧打一次剔除统计（回读值滞后两帧，只用于观测）。
    if (frameInfo.frameNumber % 60u == 0u && stats.visibleGroups + stats.culledGroups > 0u)
    {
        std::cout << "[SceneGpu] cull stats | groups=" << stats.visibleGroups + stats.culledGroups
                  << " visible=" << stats.visibleGroups
                  << " culled=" << stats.culledGroups
                  << " instances=" << stats.visibleInstances
                  << " hiz=" << (hizReady ? 1 : 0)
                  << " hizLevels=" << hiz[(frameInfo.frameInFlightIndex + 1u) % kFramesInFlight].mipLevels
                  << " hizEnabled=" << (frameInfo.settings.hizEnabled ? 1 : 0)
                  << " hizTests=" << stats.occlusionTests
                  << " hizCulled=" << stats.occlusionCulled
                  << " screenTests=" << stats.screenTests
                  << " screenCulled=" << stats.screenCulled
                  << " external=" << stats.externalInstances
                  << " stable=" << stableFrames << "/60"
                  << " rebuilds=" << structureRebuilds
                  << " flattenUs=" << (structureMicros / 60u)
                  << std::endl;
        stableFrames = 0u;
        structureRebuilds = 0u;
        structureMicros = 0ull;
        stableFrames = 0u;
    }

    if (!available)
        return;
    // --no-gpu-scene：整条路径连剔除 dispatch 都不做。
    if (!frameInfo.settings.gpuSceneEnabled)
        return;

    // 调试注入：把第一个可批模型复制 N 份当外部实例（验证粒子/体素那条链路）。
    refreshDebugExternalInstances(frameInfo);
    stats.externalInstances = getExternalInstanceCount();

    const uint32_t frame = frameInfo.frameInFlightIndex;
    // 同一槽位的统计是两帧前那次 dispatch 的结果（kFramesInFlight=2），GPU 早已完成；
    // 只用于日志与自检，不参与任何绘制决策。
    readStats(frame);

    // 稳定帧快速路径：结构签名没变就不重新摊平实例，也不重算每组的并集 AABB。
    const auto structureStart = std::chrono::steady_clock::now();
    const uint64_t signature = computeStructureSignature(frameInfo);
    const bool structureChanged = !structureBuilt || signature != structureSignature;
    if (structureChanged)
    {
        buildInstances(frameInfo);
        structureSignature = signature;
        structureBuilt = true;
        slotSignatures.fill(0ull);
        ++structureRebuilds;
    }
    else
    {
        ++stableFrames;
    }
    structureMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - structureStart).count());
    stats.groups = static_cast<uint32_t>(modelGroups.size());
    // 本帧真实提交量：GPU 回读的 submitted 是两帧前的，阈值判断与日志都用这个值。
    stats.submitted = static_cast<uint32_t>(instanceScratch.size());
    stats.drawCalls = 0;
    if (instanceScratch.empty() || modelGroups.empty())
        return;

    ensureCapacity(static_cast<uint32_t>(instanceScratch.size()),
                   static_cast<uint32_t>(modelGroups.size()));
    FrameResources& res = frames[frame];
    // 结构没变、容量没变、这个槽位的缓冲已经装过同一批数据：跳过 instances/groups 上传。
    // 命令缓冲由 compute 每帧整组重写（可见写命令、不可见写 0），所以不需要清理旧命令。
    const bool uploadStructure =
        slotSignatures[frame] != signature || slotGenerations[frame] != capacityGeneration;
    if (uploadStructure)
    {
        std::memcpy(res.instances.mapped, instanceScratch.data(),
                    instanceScratch.size() * sizeof(InstanceRecord));
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.instances.allocation, 0, VK_WHOLE_SIZE);

        GroupRecord* groups = static_cast<GroupRecord*>(res.groups.mapped);
        for (size_t i = 0; i < modelGroups.size(); ++i)
        {
            const ModelGroup& group = modelGroups[i];
            const SubMesh& subMesh = group.model->getSubMeshes()[group.subMeshIndex];
            GroupRecord& record = groups[i];
            record = {};
            record.boundsMin = glm::vec4(group.boundsMin, group.hasBounds ? 1.0f : 0.0f);
            record.boundsMax = glm::vec4(group.boundsMax, 0.0f);
            record.draw = glm::uvec4(group.merged ? group.mergedFirstIndex : subMesh.firstIndex,
                                     subMesh.indexCount,
                                     group.merged ? static_cast<uint32_t>(group.mergedBaseVertex) : 0u,
                                     group.firstInstance);
            record.counts = glm::uvec4(group.instanceCount, group.materialSlot,
                                       group.merged ? 1u : 0u, 0u);
        }
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.groups.allocation, 0, VK_WHOLE_SIZE);
        slotSignatures[frame] = signature;
        slotGenerations[frame] = capacityGeneration;
    }

    FrameData frameData{};
    frameData.viewProjection = frameInfo.projectionMatrix * frameInfo.viewMatrix;
    frameData.view = frameInfo.viewMatrix;
    const glm::mat4& vp = frameData.viewProjection;
    const glm::vec4 row0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const glm::vec4 row1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const glm::vec4 row2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const glm::vec4 row3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    frameData.planes[0] = row3 + row0;
    frameData.planes[1] = row3 - row0;
    frameData.planes[2] = row3 + row1;
    frameData.planes[3] = row3 - row1;
    frameData.planes[4] = row2;
    frameData.planes[5] = row3 - row2;
    // z = 1 告诉着色器「可以读 Hi-Z」：只有至少完成过一次构建、且开关没关时才为真。
    const HiZFrame& readHiZ = hiz[(frame + 1u) % kFramesInFlight];
    const bool hizUsable = frameInfo.settings.hizEnabled && hizReady &&
                           readHiZ.fullView != VK_NULL_HANDLE;
    frameData.screen = glm::vec4(frameInfo.settings.gpuSceneMinScreenPixels,
                                 static_cast<float>(hizExtent.height),
                                 frameInfo.projectionMatrix[1][1], 0.0f);
    frameData.params = glm::uvec4(static_cast<uint32_t>(modelGroups.size()), 0u,
                                  hizUsable ? 1u : 0u, 0u);
    std::memcpy(res.frameData.mapped, &frameData, sizeof(FrameData));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.frameData.allocation, 0, VK_WHOLE_SIZE);

    std::memset(res.stats.mapped, 0, static_cast<size_t>(res.stats.size));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.stats.allocation, 0, VK_WHOLE_SIZE);

    cullPipeline->bind(commandBuffer);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout,
                            0, 1, &res.computeSet, 0, nullptr);

    CullPushConstants push{};
    push.params = glm::uvec4(static_cast<uint32_t>(modelGroups.size()),
                             hizUsable ? readHiZ.extent.width : 0u,
                             hizUsable ? readHiZ.extent.height : 0u,
                             hizUsable ? readHiZ.mipLevels : 0u);
    vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CullPushConstants), &push);

    // 一个组一个 invocation：组数远小于实例数，剔除本身几乎不花时间。
    const uint32_t dispatchGroups = (push.params.x + 63u) / 64u;
    vkCmdDispatch(commandBuffer, dispatchGroups, 1u, 1u);

    // 命令是 compute 写的，随后 indirect draw 与 vertex/fragment 要读；
    // 一条全屏障同时覆盖这两个阶段。
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void SceneGpuBatch::recordDraw(VkCommandBuffer commandBuffer, FrameInfo& frameInfo)
{
    if (!reportedFirstBatch && stats.submitted > 0u)
    {
        reportedFirstBatch = true;
        // 只打本帧确定值：可见/剔除数是 GPU 回读的，滞后两帧，不放进这条日志。
        std::cout << "[SceneGpu] indirect batch | groups=" << modelGroups.size()
                  << " instances=" << stats.submitted << std::endl;
    }

    if (!available || instanceScratch.empty())
        return;
    if (!mergedActive || !mergedMesh.valid)
        return;

    const FrameResources& res = frames[frameInfo.frameInFlightIndex];
    const uint32_t groupCount = static_cast<uint32_t>(modelGroups.size());
    {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &mergedMesh.vertices.buffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, mergedMesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        // 一条 multidraw：命令里的 firstInstance 决定每个组件实例从哪条实例记录取变换。
        vkCmdDrawIndexedIndirect(commandBuffer, res.commands.buffer, 0, groupCount,
                                 kCommandWords * sizeof(uint32_t));
        stats.drawCalls += 1;
        // 与三角形/顶点同源（两帧前的回读）：这一帧真的提交了可见几何才算一次绘制。
        if (stats.visibleGroups > 0u)
            frameInfo.telemetry.drawCalls += 1;
        frameInfo.telemetry.triangles += stats.visibleTriangles;
        frameInfo.telemetry.vertices += stats.visibleVertices;
    }
}
