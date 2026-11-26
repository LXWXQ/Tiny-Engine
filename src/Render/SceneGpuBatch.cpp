#include "RHI/RenderGraphBlackboard.h"
#include "Render/FramePayloads.h"
#include "Render/SceneGpuBatch.h"

#include "RHI/StateCache.h"
#include "RHI/ResourceStats.h"
#include <cmath>
#include <iomanip>

#include "AssetPaths.h"
#include "Core/MaterialSlot.h"
#include "RHI/CommandUtils.h"
#include "Scene/Model.h"

// 生成的反射头（build/shaders，CMake 在编译前生成）：布局断言直接引用 SPIR-V 里的事实，
// 改 shader 忘了改 C++ 会在编译期炸，而不是运行期写越界。
#include "scene_cull.comp.reflect.h"

#include "Core/FrameInfo.h"
#include "Scene/Components.h"
#include "Scene/RenderInterpolation.h"

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
    glm::uvec4 params{ 0u };   // x=groupCount, y/z/w = Hi-Z 宽/高/层数
    // 第二段：一次 dispatch 只做一件事。x = 这一阶段处理的实例数 / 组数，
    // y = 阶段（0 组剔除 / 1 实例视锥+名次 / 2 组前缀和 / 3 写紧凑索引 / 4 恒等 remap），
    // z = 实例容量（remap 与名次在同一块缓冲里的分界；0 表示实例级剔除关着）。
    glm::uvec4 more{ 0u };
};
static_assert(sizeof(CullPushConstants) == 32, "scene_cull.comp push constant layout mismatch");

// 与着色器共享的记录布局。任何字段变动都必须两边一起改。
static_assert(sizeof(SceneGpuBatch::InstanceRecord) == 288,
              "InstanceRecord must match scene_cull.comp / mrt.vert");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, flags) == 128,
              "InstanceRecord.flags offset mismatch");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, draw) == 208,
              "InstanceRecord.draw offset mismatch");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, previousModel) == 224,
              "InstanceRecord.previousModel offset mismatch");

static_assert(sizeof(SceneGpuBatch::GroupRecord) == 32,
              "GroupRecord must match scene_cull.comp");
static_assert(offsetof(SceneGpuBatch::GroupRecord, draw) == 0,
              "GroupRecord.draw offset mismatch");
static_assert(offsetof(SceneGpuBatch::GroupRecord, counts) == 16,
              "GroupRecord.counts offset mismatch");

static_assert(sizeof(SceneGpuBatch::FrameData) == 256,
              "FrameData must match scene_cull.comp");

// P2 第一步（决策 58）：剔除统计缓冲的 GLSL 侧与 C++ 侧必须是同一块 64 字节。
// 数字取自 SPIR-V 反射，不是手抄的常量——两边任何一处改槽位，这里编译失败。
static_assert(tiny::reflect::scene_cull_comp::kStatsBufferBlockSize == sizeof(SceneGpuBatch::Stats),
              "Stats 与 scene_cull.comp 的 StatsBuffer 块大小不一致（看 scene_cull.comp.reflect.h）");
static_assert(tiny::reflect::scene_cull_comp::kStatsBufferStatsCount == 4,
              "scene_cull.comp 的 stats 必须是 4 个 uvec4，扩/缩槽要同时改 Stats");

// P2 第二步：push constant 多了一段 more（x=实例数, y=mode），实例缓冲挂在 binding 5。
// 同样用反射出来的事实锁住，避免 shader 与 C++ 各改一半。
static_assert(tiny::reflect::scene_cull_comp::kPushBlockSize == sizeof(CullPushConstants),
              "scene_cull.comp 的 push constant 块大小与 CullPushConstants 不一致");
static_assert(tiny::reflect::scene_cull_comp::kPushMoreOffset == 16u,
              "scene_cull.comp 的 push constant 第二段必须紧跟 params（偏移 16）");
static_assert(tiny::reflect::scene_cull_comp::kInstanceBufferBinding == 5u,
              "实例缓冲在 scene_cull.comp 里必须是 binding 5");
// P2 第三步：binding 6 = 每组紧凑区间，binding 7 = remap + 名次。
static_assert(tiny::reflect::scene_cull_comp::kCullGroupBufferBinding == 6u,
              "每组紧凑区间在 scene_cull.comp 里必须是 binding 6");
static_assert(tiny::reflect::scene_cull_comp::kScratchBufferBinding == 7u,
              "remap/名次缓冲在 scene_cull.comp 里必须是 binding 7");

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
    : vulkanDevice(device), gpuScene(device)
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
    VkDescriptorSetLayoutBinding computeBindings[8]{};
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
    // binding 5 = 实例缓冲（只读）：实例级剔除要按实例取局部 AABB 与世界矩阵。
    computeBindings[5].binding = 5;
    computeBindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    computeBindings[5].descriptorCount = 1;
    computeBindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // binding 6 = 每组紧凑区间（可见实例数 + remap 起点），binding 7 = remap + 名次。
    // 7 与 6 分开挂是为了让「实例 → 组」和「组 → 起点」两次改写各走各的缓冲：
    // 同一块缓冲里 remap 是拓扑结果、名次是临时量，混在一个数组里会让下标算式变复杂。
    for (uint32_t i = 6; i < 8u; ++i)
    {
        computeBindings[i].binding = i;
        computeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        computeBindings[i].descriptorCount = 1;
        computeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo computeLayoutInfo{};
    computeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    computeLayoutInfo.bindingCount = 8;
    computeLayoutInfo.pBindings = computeBindings;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &computeLayoutInfo, nullptr,
                                    &computeSetLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor layout failed");

    // 图形：set 3 binding 0 = 实例缓冲（顶点/片元都读），binding 1 = 实例 remap（顶点读）。
    VkDescriptorSetLayoutBinding instanceBindings[2]{};
    instanceBindings[0].binding = 0;
    instanceBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceBindings[0].descriptorCount = 1;
    instanceBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // 地形批处理的实例表不走 remap，但同一个管线布局要能绑它的 set，所以这个 binding 必须存在；
    // GeometrySystem 会往它里面写一份有效的存储缓冲（内容不会被读）。
    instanceBindings[1].binding = 1;
    instanceBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceBindings[1].descriptorCount = 1;
    instanceBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo instanceLayoutInfo{};
    instanceLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    instanceLayoutInfo.bindingCount = 2;
    instanceLayoutInfo.pBindings = instanceBindings;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &instanceLayoutInfo, nullptr,
                                    &instanceSetLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: instance descriptor layout failed");
    VkDescriptorPoolSize computePoolSizes[2]{};
    computePoolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // A2：每个帧槽位要给「主视图 + 反射视图」各一个计算集，池按视图数放大。
    constexpr uint32_t kSetCount = kFramesInFlight * kMaxViews;
    computePoolSizes[0].descriptorCount = 7u * kSetCount;
    computePoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    computePoolSizes[1].descriptorCount = kSetCount;
    VkDescriptorPoolCreateInfo computePoolInfo{};
    computePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    computePoolInfo.maxSets = kSetCount;
    computePoolInfo.poolSizeCount = 2;
    computePoolInfo.pPoolSizes = computePoolSizes;
    if (vkCreateDescriptorPool(vulkanDevice.getDevice(), &computePoolInfo, nullptr,
                               &computePool) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor pool failed");

    VkDescriptorPoolSize graphicsPoolSize{};
    graphicsPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    graphicsPoolSize.descriptorCount = 2u * kSetCount;
    VkDescriptorPoolCreateInfo graphicsPoolInfo{};
    graphicsPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    graphicsPoolInfo.maxSets = kSetCount;
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
    // 每个帧槽位 × 每个视图各一个计算集与实例集（A2）。
    constexpr uint32_t kSetCount = kFramesInFlight * kMaxViews;
    // 容量增长时旧描述符集还挂在池上：先整体重置，否则分配会失败（池里的 set 未归还）。
    vkResetDescriptorPool(vulkanDevice.getDevice(), computePool, 0);
    vkResetDescriptorPool(vulkanDevice.getDevice(), graphicsPool, 0);
    std::array<VkDescriptorSetLayout, kSetCount> computeLayouts{};
    computeLayouts.fill(computeSetLayout);
    VkDescriptorSetAllocateInfo computeAlloc{};
    computeAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    computeAlloc.descriptorPool = computePool;
    computeAlloc.descriptorSetCount = kSetCount;
    computeAlloc.pSetLayouts = computeLayouts.data();
    std::array<VkDescriptorSet, kSetCount> computeSets{};
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &computeAlloc, computeSets.data()) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor allocation failed");

    std::array<VkDescriptorSetLayout, kSetCount> graphicsLayouts{};
    graphicsLayouts.fill(instanceSetLayout);
    VkDescriptorSetAllocateInfo graphicsAlloc{};
    graphicsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    graphicsAlloc.descriptorPool = graphicsPool;
    graphicsAlloc.descriptorSetCount = kSetCount;
    graphicsAlloc.pSetLayouts = graphicsLayouts.data();
    std::array<VkDescriptorSet, kSetCount> graphicsSets{};
    if (vkAllocateDescriptorSets(vulkanDevice.getDevice(), &graphicsAlloc, graphicsSets.data()) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: instance descriptor allocation failed");

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        FrameResources& res = frames[frame];
        createBuffer(res.instances,
                     static_cast<VkDeviceSize>(instanceCapacity) * sizeof(InstanceRecord),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(res.groups,
                     static_cast<VkDeviceSize>(groupCapacity) * sizeof(GroupRecord),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        for (uint32_t v = 0; v < kMaxViews; ++v)
        {
            ViewResources& view = res.views[v];
            createBuffer(view.commands,
                         static_cast<VkDeviceSize>(groupCapacity) * kCommandWords * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
            createBuffer(view.frameData, sizeof(FrameData),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            // 统计缓冲 = 4 个 uvec4（shader 的 stats[4]）。尺寸从 Stats 取，size 由
            // 头文件的 static_assert 锁在 64 字节，扩槽只需改这一处布局来源。
            createBuffer(view.stats, sizeof(Stats),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            std::memset(view.stats.mapped, 0, static_cast<size_t>(view.stats.size));
            vmaFlushAllocation(vulkanDevice.getVmaAllocator(), view.stats.allocation, 0, VK_WHOLE_SIZE);
            // 每组紧凑区间：一个组一个 uvec4（可见实例数 / remap 起点）。每帧由 CPU 清零。
            createBuffer(view.cullGroups,
                         static_cast<VkDeviceSize>(groupCapacity) * sizeof(glm::uvec4),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            std::memset(view.cullGroups.mapped, 0, static_cast<size_t>(view.cullGroups.size));
            vmaFlushAllocation(vulkanDevice.getVmaAllocator(), view.cullGroups.allocation, 0, VK_WHOLE_SIZE);
            // remap + 名次：前一半（容量个 uint）是紧凑索引，后一半是本帧每个实例的名次。
            createBuffer(view.instanceRemap,
                         static_cast<VkDeviceSize>(instanceCapacity) * 2u * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            view.computeSet = computeSets[frame * kMaxViews + v];
            view.instanceSet = graphicsSets[frame * kMaxViews + v];
        }
    }
}

void SceneGpuBatch::writeDescriptors()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        FrameResources& res = frames[frame];
        for (uint32_t v = 0; v < kMaxViews; ++v)
        {
        ViewResources& view = res.views[v];
        // 存储缓冲的 binding 是 0 / 1 / 2 / 3 / 5 / 6 / 7（binding 4 是 Hi-Z 采样器）。
        // 写成「binding 表 + 信息表」两张平行数组：加一个槽位只改这两行，不会漏掉某个下标。
        constexpr uint32_t kStorageBindingCount = 7u;
        constexpr uint32_t kStorageBindings[kStorageBindingCount] = { 0u, 1u, 2u, 3u, 5u, 6u, 7u };
        VkDescriptorBufferInfo infos[kStorageBindingCount]{};
        infos[0] = { res.groups.buffer, 0, VK_WHOLE_SIZE };
        infos[1] = { view.commands.buffer, 0, VK_WHOLE_SIZE };
        infos[2] = { view.frameData.buffer, 0, VK_WHOLE_SIZE };
        infos[3] = { view.stats.buffer, 0, VK_WHOLE_SIZE };
        infos[4] = { res.instances.buffer, 0, VK_WHOLE_SIZE };
        infos[5] = { view.cullGroups.buffer, 0, VK_WHOLE_SIZE };
        infos[6] = { view.instanceRemap.buffer, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[kStorageBindingCount]{};
        for (uint32_t i = 0; i < kStorageBindingCount; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = view.computeSet;
            writes[i].dstBinding = kStorageBindings[i];
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), kStorageBindingCount, writes, 0, nullptr);
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
            hizWrite.dstSet = view.computeSet;
            hizWrite.dstBinding = 4;
            hizWrite.descriptorCount = 1;
            hizWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            hizWrite.pImageInfo = &hizInfo;
            vkUpdateDescriptorSets(vulkanDevice.getDevice(), 1, &hizWrite, 0, nullptr);
        }

        VkDescriptorBufferInfo instanceInfos[2] = {
            { res.instances.buffer, 0, VK_WHOLE_SIZE },
            { view.instanceRemap.buffer, 0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet instanceWrites[2]{};
        for (uint32_t i = 0; i < 2u; ++i)
        {
            instanceWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            instanceWrites[i].dstSet = view.instanceSet;
            instanceWrites[i].dstBinding = i;
            instanceWrites[i].descriptorCount = 1;
            instanceWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            instanceWrites[i].pBufferInfo = &instanceInfos[i];
        }
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 2, instanceWrites, 0, nullptr);
        }
    }
}

void SceneGpuBatch::destroyFrameBuffers()
{
    for (FrameResources& res : frames)
    {
        destroyBuffer(res.instances);
        for (uint32_t v = 0; v < kMaxViews; ++v)
        {
            destroyBuffer(res.views[v].commands);
            destroyBuffer(res.views[v].frameData);
            destroyBuffer(res.views[v].stats);
            destroyBuffer(res.views[v].cullGroups);
            destroyBuffer(res.views[v].instanceRemap);
            res.views[v].computeSet = VK_NULL_HANDLE;
            res.views[v].instanceSet = VK_NULL_HANDLE;
        }
        destroyBuffer(res.groups);
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

void SceneGpuBatch::prewarmModelBounds(const Model& model)
{
    // 只为填充 modelBoundsCache：几何包围盒扫索引是加载期该付的一次性成本。
    (void)subMeshBoundsFor(model, 0ull);
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

void SceneGpuBatch::destroyRetiredMergedBuffers(uint32_t frameInFlightIndex)
{
    if (frameInFlightIndex >= kFramesInFlight)
        return;
    for (GpuBuffer& buffer : retiredMergedBuffers[frameInFlightIndex])
        destroyBuffer(buffer);
    retiredMergedBuffers[frameInFlightIndex].clear();
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
    mergedRebuiltThisCall = false;
    if (mergedMesh.valid && mergedMesh.signature == signature)
        return true;   // 签名命中：不重建

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

    // 别立刻销毁：本帧早些时候阴影视图已经把旧缓冲录进命令缓冲（见成员注释）。
    if (mergedMesh.vertices.buffer != VK_NULL_HANDLE)
        retiredMergedBuffers[currentFrameSlot].push_back(mergedMesh.vertices);
    if (mergedMesh.indices.buffer != VK_NULL_HANDLE)
        retiredMergedBuffers[currentFrameSlot].push_back(mergedMesh.indices);
    mergedMesh.vertices = GpuBuffer{};
    mergedMesh.indices = GpuBuffer{};
    createBuffer(mergedMesh.vertices, sizeof(Vertex) * vertices.size(),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    createBuffer(mergedMesh.indices, sizeof(uint32_t) * indices.size(),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    uploadDeviceLocal(mergedMesh.vertices, vertices.data(), sizeof(Vertex) * vertices.size());
    uploadDeviceLocal(mergedMesh.indices, indices.data(), sizeof(uint32_t) * indices.size());
    mergedMesh.signature = signature;
    mergedMesh.valid = true;
    mergedRebuiltThisCall = true;

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
        if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain || tag->isSkyGeometry))
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

void SceneGpuBatch::refreshCubeFieldInstances(FrameInfo& frameInfo)
{
if (cubeFieldCount == 0u || !cubeFieldModel)
return;
// 阵列是静态的：只有数量变了才重建。重建会改结构签名、触发一次整帧摊平，
// 那正是要量的东西，但只在第一帧发生，不能每帧都算进 flattenUs。
    // P2 度量：--cube-field-motion 打开时每帧重建一次，量「动态实例」的真实成本。
    const bool cubeFieldAnimated = cubeFieldMotion > 0u;
    if (!cubeFieldAnimated && cubeFieldBuilt == cubeFieldCount)
        return;

// 中心点在这里算而不是在 Application 里：启动期相机的 view 矩阵还没被 update 刷过。
const glm::mat4 cubeFieldInverseView = glm::inverse(frameInfo.viewMatrix);
const glm::vec3 cubeFieldEye = glm::vec3(cubeFieldInverseView[3]);
// 视图矩阵逆的第三列是世界空间里的相机 +Z（相机看 -Z）。实测本引擎这里取 +列
// 才对得上画面（取反的那次投影 w 为负，整组被视锥剔除），所以按 +列取。
const glm::vec3 cubeFieldForward = glm::normalize(glm::vec3(cubeFieldInverseView[2]));
// 只取水平方向、高度钉 8 米：相机约 20 度俯仰，沿视线直铺会把一半立方体埋到地下。
const glm::vec2 cubeFieldFlat{ cubeFieldForward.x, cubeFieldForward.z };
const glm::vec2 cubeFieldDir = glm::length(cubeFieldFlat) > 1e-4f
? glm::normalize(cubeFieldFlat) : glm::vec2(0.0f, 1.0f);
const glm::vec3 cubeFieldCenter{
cubeFieldEye.x + cubeFieldDir.x * cubeFieldDistance,
8.0f,
cubeFieldEye.z + cubeFieldDir.y * cubeFieldDistance };

externalInstances.clear();
externalInstances.reserve(cubeFieldCount);
const uint32_t side = static_cast<uint32_t>(
std::ceil(std::cbrt(static_cast<double>(cubeFieldCount))));
const float half = static_cast<float>(side - 1u) * 0.5f;

// 立方体按 1 米建模，缩到 0.35 米：格点之间留缝，剔除结果肉眼可查。
glm::mat4 unitScale(1.0f);
unitScale[0][0] = 0.35f;
unitScale[1][1] = 0.35f;
unitScale[2][2] = 0.35f;
glm::mat4 place(1.0f);
place[3] = glm::vec4(cubeFieldCenter, 1.0f);

for (uint32_t i = 0; i < cubeFieldCount; ++i)
{
const uint32_t x = i % side;
const uint32_t y = (i / side) % side;
const uint32_t z = i / (side * side);
ExternalInstance instance{};
instance.model = cubeFieldModel.get();
instance.subMeshIndex = cubeFieldSubMesh;
instance.materialSlot = cubeFieldMaterialSlot;
glm::mat4 local = unitScale;
local[3][0] = -half + static_cast<float>(x) * cubeFieldSpacing;
local[3][1] = -half + static_cast<float>(y) * cubeFieldSpacing;
local[3][2] = -half + static_cast<float>(z) * cubeFieldSpacing;
        glm::mat4 animatedLocal = local;
        if (cubeFieldAnimated)
        {
            const float angle = static_cast<float>(frameInfo.frameNumber % 6283u) * 0.001f *
                                static_cast<float>(cubeFieldMotion) * 0.001f;
            animatedLocal = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)) * local;
        }
        instance.transform = place * animatedLocal;
externalInstances.push_back(instance);
}

cubeFieldBuilt = cubeFieldCount;
std::cout << "[SceneGpu] cube field | count=" << cubeFieldCount
          << " side=" << side << " spacing=" << cubeFieldSpacing
          << " center=" << cubeFieldCenter.x << "," << cubeFieldCenter.y << "," << cubeFieldCenter.z
          << "\n";
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
        if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain || tag->isSkyGeometry))
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
    historyInstances = 0;
    historyMoved = 0;
    // P2 度量：摊平到底花在哪。分七段计时，只在重建结构的帧累加（稳定帧不跑这段）。
    // P2 度量：只做视锥测试的「实例级可见比例」上界（不含 Hi-Z，Debug 读数用）。
    glm::vec4 instancePlanes[6];
    {
        const glm::mat4 m = frameInfo.projectionMatrix * frameInfo.viewMatrix;
        const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
        const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
        const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
        const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);
        instancePlanes[0] = row3 + row0;
        instancePlanes[1] = row3 - row0;
        instancePlanes[2] = row3 + row1;
        instancePlanes[3] = row3 - row1;
        instancePlanes[4] = row2;
        instancePlanes[5] = row3 - row2;
    }
    uint32_t instanceFrustumTested = 0;
    uint32_t instanceFrustumVisible = 0;
    const auto instanceInFrustum = [&instancePlanes](const glm::vec3& center, const glm::vec3& half)
    {
        for (uint32_t planeIndex = 0; planeIndex < 6u; ++planeIndex)
        {
            const glm::vec4& plane = instancePlanes[planeIndex];
            const float distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
            const float radius = glm::abs(plane.x) * half.x + glm::abs(plane.y) * half.y + glm::abs(plane.z) * half.z;
            if (distance + radius < 0.0f)
                return false;
        }
        return true;
    };
    const auto profileStart = std::chrono::steady_clock::now();
    auto profileMark = profileStart;
    auto lap = [&profileMark]()
    {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - profileMark).count());
        profileMark = now;
        return us;
    };

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
            if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain || tag->isSkyGeometry))
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
    cullTotals.collectMicros += lap();
    mergedActive = frameInfo.settings.gpuMergedBuffers && ensureMergedMesh(batchModels);
    cullTotals.mergedMicros += lap();
    if (mergedRebuiltThisCall)
    {
        // 合并网格只建一次：把它算进 total 但单独计数，稳态平均不含这一次。
        ++cullTotals.mergedBuildSamples;
    }

    // 2) 每个实体按（模型, 子网格）归组：同一子网格被多个实体复用时实例连续排列，
    //    一条命令 instanceCount = N 就画完（实例化），而不是每实例一条命令。
    std::map<std::pair<const Model*, uint32_t>, uint32_t> groupIndex;
    auto view = frameInfo.registry.view<MeshComponent, WorldTransformComponent>();
    uint64_t entityAabbMicros = 0;
    uint64_t entityRecords = 0;
    uint64_t entityBoundsMicros = 0;
    uint64_t entityNodeMicros = 0;
    uint64_t entityNormalMicros = 0;
    uint64_t externalNormalMicros = 0;
    uint64_t externalAabbMicros = 0;
    uint64_t externalRecordMicros = 0;
    uint64_t externalPushMicros = 0;
    for (entt::entity entity : entities)
    {
        const MeshComponent& mesh = view.get<MeshComponent>(entity);
        const WorldTransformComponent& worldTransform = view.get<WorldTransformComponent>(entity);
        const Model* model = mesh.model.get();
        const std::vector<SubMesh>& subMeshes = model->getSubMeshes();
        const auto boundsStart = std::chrono::steady_clock::now();
        const std::vector<SubMeshBounds>& boundsList = subMeshBoundsFor(*model, frameInfo.frameNumber);
        entityBoundsMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - boundsStart).count());

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
            const auto nodeStart = std::chrono::steady_clock::now();
            const glm::mat4 nodeMatrix = hasNode ? model->getNodeWorldMatrix(subMesh.node)
                                                 : glm::mat4(1.0f);
            entityNodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - nodeStart).count());
            // C1c: blend the previous tick pose with the current one, then apply
            // the node matrix. renderAlpha is 0 outside the editor play mode, in
            // which case this is exactly worldTransform.matrix.
// T-06：插值权重从图级黑板取，FrameInfo 上已没有这两个字段。
const tiny::payload::RenderInterpolation* interpolation =
frameInfo.blackboard != nullptr ? frameInfo.blackboard->find<tiny::payload::RenderInterpolation>() : nullptr;
const float renderAlpha = interpolation != nullptr ? interpolation->alpha : 0.0f;
const uint64_t renderTick = interpolation != nullptr ? interpolation->tick : 0;
            const bool interpolateEntity = scene::shouldInterpolateTransform(
                worldTransform, renderAlpha, renderTick);
            const glm::mat4 entityMatrix = interpolateEntity
                ? scene::interpolateTransform(worldTransform.previousMatrix,
                                              worldTransform.matrix, renderAlpha)
                : worldTransform.matrix;
            const glm::mat4 instanceMatrix = entityMatrix * nodeMatrix;

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
            const auto normalStart = std::chrono::steady_clock::now();
            record.normal = (hasNode || interpolateEntity)
                ? glm::mat4(glm::transpose(glm::inverse(glm::mat3(instanceMatrix))))
                : worldTransform.normalMatrix;
            entityNormalMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - normalStart).count());
            record.flags = glm::uvec4(slot, materialValid ? 1u : 0u,
                                      subMesh.alphaMasked ? 1u : 0u,
                                      (subMesh.albedoFallback || entityAlbedoFallback) ? 1u : 0u);
            record.misc = glm::vec4(subMesh.alphaCutoff, checkerScale, 0.0f, 0.0f);
            record.uvTransform = uvTransform;
            record.boundsMin = glm::vec4(localMin, 1.0f);
            record.boundsMax = glm::vec4(localMax, 0.0f);
            record.draw = glm::uvec4(0u);   // 命令由 compute 写，这里不再塞绘制参数
            // 上一帧的世界矩阵直接从插值组件取：previousMatrix 就是上一次 tick 的实体变换，
            // 乘同一个 nodeMatrix 之后与 instanceMatrix 同一口径（P5-1 的 velocity 要用它）。
            record.previousModel = worldTransform.previousMatrix * nodeMatrix;
            ++historyInstances;
            if (std::memcmp(&record.model, &record.previousModel, sizeof(glm::mat4)) != 0)
                ++historyMoved;
            buckets[groupIndexValue].push_back(record);
            // 世界 AABB 用中心/半长直接算：对仿射变换是精确解，9 次乘法。
            const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
            const glm::vec3 localHalf = (localMax - localMin) * 0.5f;
            const glm::mat3 basis(instanceMatrix);
            const glm::vec3 worldCenter = glm::vec3(instanceMatrix * glm::vec4(localCenter, 1.0f));
            const glm::vec3 worldHalf = glm::abs(basis[0]) * localHalf.x +
                                        glm::abs(basis[1]) * localHalf.y +
                                        glm::abs(basis[2]) * localHalf.z;
            ModelGroup& group = modelGroups[groupIndexValue];
            if (!group.hasBounds)
            {
                group.boundsMin = worldCenter - worldHalf;
                group.boundsMax = worldCenter + worldHalf;
                group.hasBounds = true;
            }
            else
            {
                group.boundsMin = glm::min(group.boundsMin, worldCenter - worldHalf);
                group.boundsMax = glm::max(group.boundsMax, worldCenter + worldHalf);
            }
            ++entityRecords;
        }
    }

    cullTotals.entityAabbMicros += entityAabbMicros;
    cullTotals.entityBoundsMicros += entityBoundsMicros;
    cullTotals.entityNodeMicros += entityNodeMicros;
    cullTotals.entityNormalMicros += entityNormalMicros;
    cullTotals.entityCount += static_cast<uint64_t>(entities.size());
    cullTotals.entityRecords += entityRecords;
    cullTotals.entityMicros += lap();
    // 3) 排序后拼接实例：命令顺序即组顺序。合并模式只绑一次 VBO/IBO，顺序任意；
    //    非合并模式要先按模型排，保证同一模型的组连续，才能一条命令画完一组。
    // 2b) 外部实例：没有 ECS 实体，变换由调用方直接给（粒子/体素）。
    // bounds 缓存按模型复用：外部实例通常共享同一个模型，逐实例查表是纯浪费。
    const Model* cachedBoundsModel = nullptr;
    const std::vector<SubMeshBounds>* cachedBounds = nullptr;
    if (externalRecordCache.size() != externalInstances.size())
        externalRecordCache.assign(externalInstances.size(), ExternalRecordCache{});
    for (size_t externalIndex = 0; externalIndex < externalInstances.size(); ++externalIndex)
    {
        const ExternalInstance& external = externalInstances[externalIndex];
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
        if (cachedBoundsModel != model)
        {
            cachedBoundsModel = model;
            cachedBounds = &subMeshBoundsFor(*model, frameInfo.frameNumber);
        }
        const std::vector<SubMeshBounds>& boundsList = *cachedBounds;
        if (external.subMeshIndex < boundsList.size() && boundsList[external.subMeshIndex].valid)
        {
            localMin = boundsList[external.subMeshIndex].min;
            localMax = boundsList[external.subMeshIndex].max;
        }

        // 缓存命中判定：先比模型/子网格/材质槽（整型），再 memcmp 变换矩阵（64 字节）。
        // 不用逐浮点哈希：16 次 FNV 在 30k 实例上比 memcmp 贵一个数量级。
        ExternalRecordCache& cache = externalRecordCache[externalIndex];
        const bool sameBinding = cache.valid && cache.model == model &&
            cache.subMeshIndex == external.subMeshIndex && cache.materialSlot == external.materialSlot;
        InstanceRecord record{};
        glm::vec3 worldCenter{ 0.0f };
        glm::vec3 worldHalf{ 0.0f };
        if (sameBinding && std::memcmp(&cache.transform, &external.transform, sizeof(glm::mat4)) == 0)
        {
            record = cache.record;
            // 缓存命中说明这条实例的变换与上次重建完全相同：上一帧就是当前帧。
            record.previousModel = external.transform;
            ++historyInstances;
            worldCenter = cache.worldCenter;
            worldHalf = cache.worldHalf;
            ++cullTotals.externalReused;
        }
        else
        {
            const auto extRecordStart = std::chrono::steady_clock::now();
            record.model = external.transform;
            const auto extNormalStart = std::chrono::steady_clock::now();
            record.normal = glm::mat4(glm::transpose(glm::inverse(glm::mat3(external.transform))));
            externalNormalMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - extNormalStart).count());
            record.flags = glm::uvec4(external.materialSlot, 1u, subMesh.alphaMasked ? 1u : 0u,
                                      subMesh.albedoFallback ? 1u : 0u);
            record.misc = glm::vec4(subMesh.alphaCutoff, 1.0f, 0.0f, 0.0f);
            record.uvTransform = glm::vec4(0.0f);
            record.boundsMin = glm::vec4(localMin, 1.0f);
            record.boundsMax = glm::vec4(localMax, 0.0f);
            record.draw = glm::uvec4(0u);
            // 同一个外部槽位、只是变换变了：旧变换就是上一帧；换了绑定（模型/子网格/材质槽）
            // 说明是另一条实例，没有可继承的历史，直接取当前值。
            record.previousModel = (cache.valid && sameBinding) ? cache.transform : external.transform;
            ++historyInstances;
            if (std::memcmp(&record.model, &record.previousModel, sizeof(glm::mat4)) != 0)
                ++historyMoved;
            externalRecordMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - extRecordStart).count());

            // 中心/半长求世界 AABB：对仿射变换是精确解，只要 9 次乘法，
            // 比 8 个角点各做一次 mat4 * vec4 便宜一个数量级。
            const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
            const glm::vec3 localHalf = (localMax - localMin) * 0.5f;
            const glm::mat3 basis(external.transform);
            worldCenter = glm::vec3(external.transform * glm::vec4(localCenter, 1.0f));
            worldHalf = glm::abs(basis[0]) * localHalf.x +
                        glm::abs(basis[1]) * localHalf.y +
                        glm::abs(basis[2]) * localHalf.z;

            cache.transform = external.transform;
            cache.model = model;
            cache.subMeshIndex = external.subMeshIndex;
            cache.materialSlot = external.materialSlot;
            cache.record = record;
            cache.worldCenter = worldCenter;
            cache.worldHalf = worldHalf;
            cache.valid = true;
        }

        const auto extPushStart = std::chrono::steady_clock::now();
        buckets[groupIndexValue].push_back(record);
        externalPushMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - extPushStart).count());

        const auto extAabbStart = std::chrono::steady_clock::now();
        ModelGroup& group = modelGroups[groupIndexValue];
        if (!group.hasBounds)
        {
            group.boundsMin = worldCenter - worldHalf;
            group.boundsMax = worldCenter + worldHalf;
            group.hasBounds = true;
        }
        else
        {
            group.boundsMin = glm::min(group.boundsMin, worldCenter - worldHalf);
            group.boundsMax = glm::max(group.boundsMax, worldCenter + worldHalf);
        }
        externalAabbMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - extAabbStart).count());
    }

    cullTotals.externalNormalMicros += externalNormalMicros;
    cullTotals.externalAabbMicros += externalAabbMicros;
    cullTotals.externalRecordMicros += externalRecordMicros;
    cullTotals.externalPushMicros += externalPushMicros;
    cullTotals.externalMicros += lap();
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
    cullTotals.sortMicros += lap();

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
    cullTotals.assembleMicros += lap();
    for (const ExternalRecordCache& cache : externalRecordCache)
    {
        if (!cache.valid)
            continue;
        ++instanceFrustumTested;
        if (instanceInFrustum(cache.worldCenter, cache.worldHalf))
            ++instanceFrustumVisible;
    }
    cullTotals.instanceFrustumTested += instanceFrustumTested;
    cullTotals.instanceFrustumVisible += instanceFrustumVisible;
    if (gpuScene.isEnabled())
    {
    // T-08：把同一批数据镜像进持久场景缓冲（稳定编号 + 脏区间上传）。
    // 这里只是骨架：剔除仍然读每帧上传的 instances/groups，本段不改变任何渲染结果。
    for (uint32_t groupIndexValue = 0; groupIndexValue < static_cast<uint32_t>(modelGroups.size()); ++groupIndexValue)
    {
        const ModelGroup& group = modelGroups[groupIndexValue];
        const SubMesh& subMesh = group.model->getSubMeshes()[group.subMeshIndex];
        tiny::GpuScene::Primitive primitive{};
        primitive.boundsMin = glm::vec4(group.boundsMin, group.hasBounds ? 1.0f : 0.0f);
        primitive.boundsMax = glm::vec4(group.boundsMax, 0.0f);
        primitive.draw = glm::uvec4(group.merged ? group.mergedFirstIndex : subMesh.firstIndex,
                                    subMesh.indexCount,
                                    group.merged ? static_cast<uint32_t>(group.mergedBaseVertex) : 0u,
                                    group.materialSlot);
        const uint64_t primitiveKey =
            (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(group.model)) >> 4u) ^
            (static_cast<uint64_t>(group.subMeshIndex) << 48u) ^ 0x51ED2701ull;
        const uint32_t primitiveIndex = gpuScene.ensurePrimitive(primitiveKey, primitive);

        const uint32_t count = group.instanceCount;
        for (uint32_t instance = 0; instance < count; ++instance)
        {
            const InstanceRecord& source = instanceScratch[group.firstInstance + instance];
            tiny::GpuScene::Instance record{};
            record.transform = source.model;
            record.normal = source.normal;
            record.ids = glm::uvec4(primitiveIndex, instance, source.flags.x, source.flags.y);
        record.previousTransform = source.previousModel;
            const uint64_t instanceKey = primitiveKey ^ (static_cast<uint64_t>(instance + 1u) * 0x9E3779B97F4A7C15ull);
            gpuScene.ensureInstance(instanceKey, record);
        }
    }
    }
    cullTotals.mirrorMicros += lap();
    ++cullTotals.structureSamples;
    buckets.clear();
}

void SceneGpuBatch::readStats(uint32_t frameInFlightIndex, uint32_t viewIndex)
{
    const FrameResources& res = frames[frameInFlightIndex];
    if (res.views[viewIndex].stats.mapped == nullptr)
        return;
    // 这个帧槽上一次使用早已 fence 完成；host 可见内存不保证一致，
    // 必须先 invalidate 才能读到 GPU 写进去的计数（否则永远是 0）。
    vmaInvalidateAllocation(vulkanDevice.getVmaAllocator(), res.views[viewIndex].stats.allocation,
                            0, VK_WHOLE_SIZE);
    // 按名字逐字读：主读前 10 个 GPU 字（stats[0] 的 4 个 + stats[1] 的 4 个 + stats[2] 的前 2 个），
    // 再加 words[12..13]（stats[3].xy）的实例级视锥统计；words[10..11]、words[14..15] 没有写入者。
    const uint32_t* words = static_cast<const uint32_t*>(res.views[viewIndex].stats.mapped);
    if (viewIndex != 0u)
    {
        // 反射视图只留帧级代价：组数、可见/剔除组、提交实例数、实例可见率。
        // 回读值滞后两帧，前两帧 words[1] + words[2] 可能还是 0，那不算样本。
        if (words[1] + words[2] == 0u)
            return;
        reflectionTotals.frames += 1;
        reflectionTotals.visibleGroups += words[1];
        reflectionTotals.culledGroups += words[2];
        reflectionTotals.visibleInstances += words[3];
        reflectionTotals.instanceFrustumTested += words[12];
        reflectionTotals.instanceFrustumVisible += words[13];
        return;
    }
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
    stats.instanceTests = words[12];
    stats.instanceVisible = words[13];

    // 累计剔除量：只在 GPU 真跑过剔除的那些帧里累加（words[1]+words[2] > 0）。
    // 回读值滞后两帧，前两帧可能是 0，那不算样本。
    if (stats.visibleGroups + stats.culledGroups > 0u)
    {
        ++cullTotals.frames;
        cullTotals.submitted += stats.submitted;
        cullTotals.visibleGroups += stats.visibleGroups;
        cullTotals.culledGroups += stats.culledGroups;
        cullTotals.visibleInstances += stats.visibleInstances;
        cullTotals.occlusionCulled += stats.occlusionCulled;
        cullTotals.screenCulled += stats.screenCulled;
        cullTotals.gpuInstanceTested += stats.instanceTests;
        cullTotals.gpuInstanceVisible += stats.instanceVisible;
        cullTotals.flattenMicros += structureMicros;
    }
}

void SceneGpuBatch::logCullSummary() const
{
    if (cullTotals.frames == 0)
    {
        std::cout << "[SceneGpu] cull summary | 没有采集到样本（GPU 剔除未运行）\n";
        return;
    }

    const double frames = static_cast<double>(cullTotals.frames);
    const double groupsTotal = static_cast<double>(cullTotals.visibleGroups + cullTotals.culledGroups);
    const double visibleRatio = groupsTotal > 0.0
        ? static_cast<double>(cullTotals.visibleGroups) / groupsTotal
        : 0.0;

    // visibleRatio 是 P2 的对照读数。P2-5 起组不再参与剔除，这个比值变成
    // 「至少有一个可见实例的组 / 全部组」，仍然可用于观察组粒度上的收缩。
    std::cout << "[SceneGpu] cull summary | frames=" << cullTotals.frames
              << " visibleRatio=" << std::fixed << std::setprecision(2) << visibleRatio
              << " | groups/frame=" << std::setprecision(1) << (groupsTotal / frames)
              << " visible=" << (static_cast<double>(cullTotals.visibleGroups) / frames)
              << " culled=" << (static_cast<double>(cullTotals.culledGroups) / frames)
              << " | instances/frame=" << (static_cast<double>(cullTotals.visibleInstances) / frames)
              << " submitted/frame=" << (static_cast<double>(cullTotals.submitted) / frames)
              << " | hizCulled/frame=" << (static_cast<double>(cullTotals.occlusionCulled) / frames)
              << " | flattenUs/frame=" << (static_cast<double>(cullTotals.flattenMicros) / frames)
              << " screenCulled/frame=" << (static_cast<double>(cullTotals.screenCulled) / frames)
              << "\n";

    // P2 的对照读数：GPU 侧的实例级视锥可见比例。和 CPU 的 instFrustum 用同一套算式，
    // 差别只在覆盖面 —— GPU 覆盖整条实例缓冲（含实体），CPU 只测外部记录缓存。
    // submit/frame 是紧凑之后真正提交的实例数：实例级剔除开着时它小于 visible，关着时两者相等。
    if (cullTotals.gpuInstanceTested > 0u)
    {
        const double tested = static_cast<double>(cullTotals.gpuInstanceTested);
        std::cout << "[SceneGpu] instance summary | frames=" << cullTotals.frames
                  << " gpuInst=" << std::fixed << std::setprecision(2)
                  << (static_cast<double>(cullTotals.gpuInstanceVisible) / tested)
                  << " (" << cullTotals.gpuInstanceVisible << "/" << cullTotals.gpuInstanceTested << ")"
                  << " | tested/frame=" << std::setprecision(1) << (tested / frames)
                  << " submit/frame=" << (static_cast<double>(cullTotals.visibleInstances) / frames)
                  << " cull=" << (instanceCullEnabled ? "on" : "off")
                  << "\n";
    }

    // P2-2：上一帧世界矩阵的覆盖情况。静止场景 moved 必须是 0；
    // 带 --cube-field-motion 时应接近实例总数（那时每帧都重建）。
    if (cullTotals.structureSamples > 0)
    {
        if (reflectionTotals.frames > 0u)
    {
        const double reflectionFrames = static_cast<double>(reflectionTotals.frames);
        const double reflectionGroups = static_cast<double>(reflectionTotals.visibleGroups + reflectionTotals.culledGroups);
        const double reflectionRatio = reflectionGroups > 0.0
            ? static_cast<double>(reflectionTotals.visibleGroups) / reflectionGroups : 0.0;
        const double reflectionInstRatio = reflectionTotals.instanceFrustumTested > 0u
            ? static_cast<double>(reflectionTotals.instanceFrustumVisible) / static_cast<double>(reflectionTotals.instanceFrustumTested)
            : 0.0;
        std::cout << "[SceneGpu] reflection summary | frames=" << reflectionTotals.frames
                  << " visibleRatio=" << std::fixed << std::setprecision(2) << reflectionRatio
                  << " | groups/frame=" << std::setprecision(1) << (reflectionGroups / reflectionFrames)
                  << " visible=" << (static_cast<double>(reflectionTotals.visibleGroups) / reflectionFrames)
                  << " culled=" << (static_cast<double>(reflectionTotals.culledGroups) / reflectionFrames)
                  << " | submit/frame=" << (static_cast<double>(reflectionTotals.visibleInstances) / reflectionFrames)
                  << " | gpuInst=" << std::setprecision(2) << reflectionInstRatio
                  << " (" << reflectionTotals.instanceFrustumVisible << "/" << reflectionTotals.instanceFrustumTested << ")\n";
    }
    std::cout << "[SceneGpu] instance history | instances=" << historyInstances
                  << " moved=" << historyMoved << " (last rebuild)\n";
    }

    // P2 度量：摊平花在哪。只在重建结构的帧有样本，稳定帧不计入。
    if (cullTotals.structureSamples > 0)
    {
        const double samples = static_cast<double>(cullTotals.structureSamples);
        const double total = static_cast<double>(cullTotals.collectMicros + cullTotals.mergedMicros +
            cullTotals.entityMicros + cullTotals.externalMicros + cullTotals.sortMicros +
            cullTotals.assembleMicros + cullTotals.mirrorMicros);
        auto share = [total](uint64_t us)
        {
            return total > 0.0 ? 100.0 * static_cast<double>(us) / total : 0.0;
        };
        std::cout << "[SceneGpu] flatten breakdown | samples=" << cullTotals.structureSamples
                  << " | total=" << std::setprecision(2) << (total / samples / 1000.0) << " ms/rebuild"
                  << " | collect=" << std::setprecision(1) << (static_cast<double>(cullTotals.collectMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.collectMicros) << "%)"
                  << " merged=" << (static_cast<double>(cullTotals.mergedMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.mergedMicros) << "%)"
                  << " | entities=" << (static_cast<double>(cullTotals.entityMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.entityMicros) << "%)"
                  << " external=" << (static_cast<double>(cullTotals.externalMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.externalMicros) << "%)"
                  << " | sort=" << (static_cast<double>(cullTotals.sortMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.sortMicros) << "%)"
                  << " assemble=" << (static_cast<double>(cullTotals.assembleMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.assembleMicros) << "%)"
                  << " mirror=" << (static_cast<double>(cullTotals.mirrorMicros) / samples / 1000.0)
                  << " ms (" << share(cullTotals.mirrorMicros) << "%)"
                  << " | entities=" << (cullTotals.entityCount / samples) << " records=" << (cullTotals.entityRecords / samples)
                  << " bounds=" << (static_cast<double>(cullTotals.entityBoundsMicros) / samples / 1000.0) << " ms"
                  << " node=" << (static_cast<double>(cullTotals.entityNodeMicros) / samples / 1000.0) << " ms"
                  << " normal=" << (static_cast<double>(cullTotals.entityNormalMicros) / samples / 1000.0) << " ms"
                  << " | extNormal=" << (static_cast<double>(cullTotals.externalNormalMicros) / samples / 1000.0) << " ms"
                  << " extAabb=" << (static_cast<double>(cullTotals.externalAabbMicros) / samples / 1000.0) << " ms"
                  << " extRecord=" << (static_cast<double>(cullTotals.externalRecordMicros) / samples / 1000.0) << " ms"
                  << " extPush=" << (static_cast<double>(cullTotals.externalPushMicros) / samples / 1000.0) << " ms"
                  << " mergedBuild=" << cullTotals.mergedBuildSamples
         << " instFrustum=" << cullTotals.instanceFrustumVisible << "/" << cullTotals.instanceFrustumTested
                  << " extReused=" << cullTotals.externalReused
                  << " aabbInEntity=" << (static_cast<double>(cullTotals.entityAabbMicros) / samples / 1000.0)
                  << " ms (" << (total > 0.0 ? 100.0 * static_cast<double>(cullTotals.entityAabbMicros) / total : 0.0) << "%)"
                  << "\n";
    }
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
        tiny::resourcestats::countImage();
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
    // 上一轮同一帧槽的命令缓冲这时候已经 fence 等过了，退役的合并网格可以销毁。
    currentFrameSlot = frameInfo.frameInFlightIndex;
    destroyRetiredMergedBuffers(currentFrameSlot);
    // T-08：持久场景按帧清脏范围（必须在 buildInstances 之前）。
    gpuScene.beginFrame();
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
    // P2 的尺子优先：有立方体阵列时不再走调试注入（两者都写 externalInstances）。
    if (cubeFieldCount > 0u)
        refreshCubeFieldInstances(frameInfo);
    else
        refreshDebugExternalInstances(frameInfo);
    stats.externalInstances = getExternalInstanceCount();

    const uint32_t frame = frameInfo.frameInFlightIndex;
    // 同一槽位的统计是两帧前那次 dispatch 的结果（kFramesInFlight=2），GPU 早已完成；
    // 只用于日志与自检，不参与任何绘制决策。
    readStats(frame);
    // 反射视图（view 1）每帧也跑四段 dispatch，但它的代价只有在帧级累计里才看得出来。
    readStats(frame, 1u);

    // 稳定帧快速路径：结构签名没变就不重新摊平实例，也不重算每组的并集 AABB。
    // P2 度量：按需强制重建，用来取摊平的稳态样本（首帧那次含 buckets 扩容与合并网格构建）。
    if (forceRebuildEvery > 0u)
    {
        if (forceRebuildTick == 0u)
            structureBuilt = false;
        forceRebuildTick = (forceRebuildTick + 1u) % forceRebuildEvery;
    }
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

    // 主视图：投影 * 视图。反射视图由 GeometrySystem 显式调 prepareView。
    prepareView(commandBuffer, frameInfo, 0u,
                frameInfo.projectionMatrix * frameInfo.viewMatrix, frameInfo.viewMatrix);
}

void SceneGpuBatch::prepareView(VkCommandBuffer commandBuffer, FrameInfo& frameInfo,
                                uint32_t viewIndex, const glm::mat4& viewProjection,
                                const glm::mat4& view, bool screenSpaceTest)
{
    if (!available || !frameInfo.settings.gpuSceneEnabled)
        return;
    if (instanceScratch.empty() || modelGroups.empty())
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    ViewResources& resources = frames[frame].views[viewIndex];

    ensureCapacity(static_cast<uint32_t>(instanceScratch.size()),
                   static_cast<uint32_t>(modelGroups.size()));
    FrameResources& res = frames[frame];
    // 结构没变、容量没变、这个槽位的缓冲已经装过同一批数据：跳过 instances/groups 上传。
    // 命令缓冲由 compute 每帧整组重写（模式 0 写基线、模式 2 改紧凑区间或清零），
    // 所以不需要清理旧命令。
    // 实例缓冲与组表由两个视图共享：只让主视图上传，避免反射视图重复搬 30k 条记录。
    const bool uploadStructure = viewIndex == 0u &&
        (slotSignatures[frame] != structureSignature || slotGenerations[frame] != capacityGeneration);
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
            // 组不再带宽高：剔除按实例做，组级 AABB 只有 primitive 缓冲还用（决策 63）。
            record.draw = glm::uvec4(group.merged ? group.mergedFirstIndex : subMesh.firstIndex,
                                     subMesh.indexCount,
                                     group.merged ? static_cast<uint32_t>(group.mergedBaseVertex) : 0u,
                                     group.firstInstance);
            record.counts = glm::uvec4(group.instanceCount, group.materialSlot,
                                       group.merged ? 1u : 0u, 0u);
        }
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.groups.allocation, 0, VK_WHOLE_SIZE);
        slotSignatures[frame] = structureSignature;
        slotGenerations[frame] = capacityGeneration;
    }

    FrameData frameData{};
    frameData.viewProjection = viewProjection;
    frameData.view = view;
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
    // 级联视图不做屏幕误差剔除：阈值是按主视图像素密度定的，阴影图 2048² 下没有意义。
    frameData.screen = glm::vec4(screenSpaceTest ? frameInfo.settings.gpuSceneMinScreenPixels : 0.0f,
                                 static_cast<float>(hizExtent.height),
                                 frameInfo.projectionMatrix[1][1], 0.0f);
    frameData.params = glm::uvec4(static_cast<uint32_t>(modelGroups.size()), 0u,
                                  (viewIndex == 0u && hizUsable) ? 1u : 0u, 0u);
    std::memcpy(resources.frameData.mapped, &frameData, sizeof(FrameData));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), resources.frameData.allocation, 0, VK_WHOLE_SIZE);

    std::memset(resources.stats.mapped, 0, static_cast<size_t>(resources.stats.size));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), resources.stats.allocation, 0, VK_WHOLE_SIZE);
    // 每组可见实例数靠 atomicAdd 领号，必须每帧从 0 开始。它和 stats 一样是「本帧独有」的
    // 累积器，槽位缓冲由帧栅栏保证没有被在飞的帧读着。
    std::memset(resources.cullGroups.mapped, 0, static_cast<size_t>(resources.cullGroups.size));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), resources.cullGroups.allocation, 0, VK_WHOLE_SIZE);

    // T-08：把本帧脏区间搬进持久缓冲。骨架阶段没有任何消费者，只统计上传量。
    gpuScene.flush(commandBuffer);
    gpuScene.endFrame();
    cullPipeline->bind(commandBuffer);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout,
                            0, 1, &resources.computeSet, 0, nullptr);

    CullPushConstants push{};
    push.params = glm::uvec4(static_cast<uint32_t>(modelGroups.size()),
                             hizUsable ? readHiZ.extent.width : 0u,
                             hizUsable ? readHiZ.extent.height : 0u,
                             hizUsable ? readHiZ.mipLevels : 0u);
    vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CullPushConstants), &push);

    const uint32_t instanceCount = static_cast<uint32_t>(instanceScratch.size());
    // 实例级剔除开着时，组剔除那一步不再累加实例级统计（改由前缀和按紧凑后的数量累加），
    // 所以用 more.z 把「实例容量」带过去；0 表示关着。
    const bool instanceCull = instanceCullEnabled && instanceCount > 0u;

    // 一次 dispatch 只做一件事：push 好这一段用的参数再发。
    const auto dispatchStage = [&](uint32_t mode, uint32_t count, uint32_t capacity)
    {
        push.more = glm::uvec4(count, mode, capacity, 0u);
        vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(CullPushConstants), &push);
        vkCmdDispatch(commandBuffer, (count + 63u) / 64u, 1u, 1u);
    };
    const auto computeBarrier = [&]()
    {
        VkMemoryBarrier computeToCompute{};
        computeToCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        computeToCompute.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        computeToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &computeToCompute,
                             0, nullptr, 0, nullptr);
    };

    if (viewIndex != 0u && !reportedReflectionView)
    {
        reportedReflectionView = true;
        std::cout << "[SceneGpu] reflection view | groups=" << modelGroups.size()
                  << " instances=" << instanceScratch.size()
                  << " cull=" << (instanceCull ? "on" : "off") << std::endl;
    }

    // 第一遍：一个组一个 invocation。组数远小于实例数，剔除本身几乎不花时间。
    dispatchStage(0u, static_cast<uint32_t>(modelGroups.size()),
                  instanceCull ? instanceCapacity : 0u);

    if (instanceCull)
    {
        // 第二遍：一个 invocation 一个实例，测视锥并给可见实例领名次。这段不能塞进组内循环 ——
        // cube field 就是一个组压着 30000 个实例，那样等于用一个线程跑完整个测试。
        // 组剔除写的命令在下一段才读，这里先挂一条 compute→compute 屏障，后面两段同样。
        computeBarrier();
        dispatchStage(1u, instanceCount, instanceCapacity);

        // 第三遍：组前缀和 + 把命令的实例区间改写成紧凑区间。组数几十到几百，
        // 一个 invocation 串行过完比并行扫描省事（而且每组只写一次命令）。
        computeBarrier();
        dispatchStage(2u, static_cast<uint32_t>(modelGroups.size()), instanceCapacity);

        // 第四遍：把名次搬成 remap[起点 + 名次] = 真实实例下标。
        computeBarrier();
        dispatchStage(3u, instanceCount, instanceCapacity);
    }
    else
    {
        // 回退路径：写恒等 remap，命令保持组剔除写下的原值。顶点着色器仍然走同一段代码，
        // 所以这条路径的画面与「没有 remap」逐像素一致，用来验证实例级剔除没有副作用。
        dispatchStage(4u, instanceCount, 0u);
    }

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
    recordDrawView(commandBuffer, frameInfo, 0u);
}

void SceneGpuBatch::recordDrawView(VkCommandBuffer commandBuffer, FrameInfo& frameInfo,
                                   uint32_t viewIndex)
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
    const ViewResources& view = res.views[viewIndex];
    const uint32_t groupCount = static_cast<uint32_t>(modelGroups.size());
    {
        tiny::statecache::bindVertexBuffer(commandBuffer, mergedMesh.vertices.buffer, 0);
        tiny::statecache::bindIndexBuffer(commandBuffer, mergedMesh.indices.buffer, 0,
                                          VK_INDEX_TYPE_UINT32);
        // 一条 multidraw：命令里的 firstInstance 决定每个组件实例从哪条实例记录取变换。
        vkCmdDrawIndexedIndirect(commandBuffer, view.commands.buffer, 0, groupCount,
                                 kCommandWords * sizeof(uint32_t));
        // 反射视图只进 GPU 队列，不进主视角 telemetry（Validate 的 draw call 判据）。
        if (viewIndex == 0u)
        {
            stats.drawCalls += 1;
            // 与三角形/顶点同源（两帧前的回读）：这一帧真的提交了可见几何才算一次绘制。
            if (stats.visibleGroups > 0u)
                frameInfo.telemetry.drawCalls += 1;
            frameInfo.telemetry.triangles += stats.visibleTriangles;
        }
        frameInfo.telemetry.vertices += stats.visibleVertices;
    }
}

void SceneGpuBatch::drawMergedIndirect(VkCommandBuffer commandBuffer, FrameInfo& frameInfo,
                                       uint32_t viewIndex)
{
    if (!available || !mergedActive || !mergedMesh.valid || instanceScratch.empty())
        return;
    const ViewResources& view = frames[frameInfo.frameInFlightIndex].views[viewIndex];
    const uint32_t groupCount = static_cast<uint32_t>(modelGroups.size());
    tiny::statecache::bindVertexBuffer(commandBuffer, mergedMesh.vertices.buffer, 0);
    tiny::statecache::bindIndexBuffer(commandBuffer, mergedMesh.indices.buffer, 0,
                                      VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(commandBuffer, view.commands.buffer, 0, groupCount,
                             kCommandWords * sizeof(uint32_t));
}
