#include "Render/SceneGpuBatch.h"

#include "Scene/Model.h"
#include "Core/MaterialSlot.h"
#include "AssetPaths.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
constexpr uint32_t kMinInstanceCapacity = 256;
constexpr uint32_t kMinModelCapacity = 32;
constexpr uint32_t kMaxCachedModels = 64;

struct CullPushConstants
{
    glm::uvec4 params{ 0u };   // x=instanceCount y=modelCount
};
static_assert(sizeof(CullPushConstants) == 16, "scene_cull.comp push constant layout mismatch");

// SceneGpuBatch 与着色器共享的实例记录布局。任何字段变动都必须两边一起改。
static_assert(sizeof(SceneGpuBatch::InstanceRecord) == 224,
              "InstanceRecord must match scene_cull.comp / mrt.vert");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, flags) == 128,
              "InstanceRecord.flags offset mismatch");
static_assert(offsetof(SceneGpuBatch::InstanceRecord, draw) == 208,
              "InstanceRecord.draw offset mismatch");
} // namespace

SceneGpuBatch::SceneGpuBatch(Device& device)
    : vulkanDevice(device)
{
    available = vulkanDevice.supportsDrawIndirectCount();
    if (!available)
    {
        std::cout << "[SceneGpu] GPU batch unavailable | drawIndirectCount=0; falling back to CPU submesh draws\n";
        return;
    }

    createLayoutsAndPipeline();
    std::cout << "[SceneGpu] GPU batch ready | instancesPerFrame=" << instanceCapacity
              << " models=" << modelCapacity << " drawIndirectCount=1\n";
}

bool SceneGpuBatch::useGpuPath(const FrameInfo& frameInfo) const
{
    if (!available || !frameInfo.settings.gpuSceneEnabled)
        return false;
    if (frameInfo.settings.gpuSceneForce)
        return true;
    return stats.submitted >= frameInfo.settings.gpuSceneMinInstances;
}

SceneGpuBatch::~SceneGpuBatch()
{
    destroyResources();
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
        : 0;

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
    VkDescriptorSetLayoutBinding computeBindings[6]{};
    for (uint32_t i = 0; i < 6u; ++i)
    {
        computeBindings[i].binding = i;
        computeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        computeBindings[i].descriptorCount = 1;
        computeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo computeLayoutInfo{};
    computeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    computeLayoutInfo.bindingCount = 6;
    computeLayoutInfo.pBindings = computeBindings;
    if (vkCreateDescriptorSetLayout(vulkanDevice.getDevice(), &computeLayoutInfo, nullptr,
                                    &computeSetLayout) != VK_SUCCESS)
        throw std::runtime_error("SceneGpuBatch: compute descriptor layout failed");

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

    VkDescriptorPoolSize computePoolSize{};
    computePoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    computePoolSize.descriptorCount = 6u * kFramesInFlight;
    VkDescriptorPoolCreateInfo computePoolInfo{};
    computePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    computePoolInfo.maxSets = kFramesInFlight;
    computePoolInfo.poolSizeCount = 1;
    computePoolInfo.pPoolSizes = &computePoolSize;
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

    cullPipeline = std::make_unique<ComputePipeline>(
        vulkanDevice, TINY_SHADER_DIR "scene_cull.comp.spv", computePipelineLayout);

    ensureCapacity(kMinInstanceCapacity, kMinModelCapacity);
}

void SceneGpuBatch::ensureCapacity(uint32_t instanceNeed, uint32_t modelNeed)
{
    if (instanceCapacity >= instanceNeed && modelCapacity >= modelNeed)
        return;

    const uint32_t newInstances = std::max(instanceNeed, std::max(instanceCapacity * 2u, kMinInstanceCapacity));
    const uint32_t newModels = std::max(modelNeed, std::max(modelCapacity * 2u, kMinModelCapacity));
    if (newInstances == instanceCapacity && newModels == modelCapacity)
        return;

    // 旧缓冲上可能还有在飞的帧，重建前先等设备空闲：容量翻倍是低频事件，
    // 换来的是「不需要同时维护两代资源的生命周期」。
    vkDeviceWaitIdle(vulkanDevice.getDevice());
    destroyFrameBuffers();
    instanceCapacity = newInstances;
    modelCapacity = newModels;
    createFrameBuffers();
    writeDescriptors();
}

void SceneGpuBatch::createFrameBuffers()
{
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
                     static_cast<VkDeviceSize>(instanceCapacity) * 5u * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
        createBuffer(res.counters,
                     static_cast<VkDeviceSize>(modelCapacity) * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        createBuffer(res.ranges,
                     static_cast<VkDeviceSize>(modelCapacity) * sizeof(glm::uvec4),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(res.frameData, sizeof(FrameData),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(res.stats, 8u * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        std::memset(res.counters.mapped, 0, static_cast<size_t>(res.counters.size));
        std::memset(res.stats.mapped, 0, static_cast<size_t>(res.stats.size));
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.counters.allocation, 0, VK_WHOLE_SIZE);
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
        VkDescriptorBufferInfo infos[6]{};
        infos[0] = { res.instances.buffer, 0, VK_WHOLE_SIZE };
        infos[1] = { res.commands.buffer, 0, VK_WHOLE_SIZE };
        infos[2] = { res.counters.buffer, 0, VK_WHOLE_SIZE };
        infos[3] = { res.ranges.buffer, 0, VK_WHOLE_SIZE };
        infos[4] = { res.frameData.buffer, 0, VK_WHOLE_SIZE };
        infos[5] = { res.stats.buffer, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[6]{};
        for (uint32_t i = 0; i < 6u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = res.computeSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 6, writes, 0, nullptr);

        VkWriteDescriptorSet instanceWrite{};
        instanceWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        instanceWrite.dstSet = res.graphicsSet;
        instanceWrite.dstBinding = 0;
        instanceWrite.descriptorCount = 1;
        instanceWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instanceWrite.pBufferInfo = &infos[0];
        vkUpdateDescriptorSets(vulkanDevice.getDevice(), 1, &instanceWrite, 0, nullptr);
    }
}

void SceneGpuBatch::destroyFrameBuffers()
{
    for (FrameResources& res : frames)
    {
        destroyBuffer(res.instances);
        destroyBuffer(res.commands);
        destroyBuffer(res.counters);
        destroyBuffer(res.ranges);
        destroyBuffer(res.frameData);
        destroyBuffer(res.stats);
        res.computeSet = VK_NULL_HANDLE;
        res.graphicsSet = VK_NULL_HANDLE;
    }
}

void SceneGpuBatch::destroyResources()
{
    destroyFrameBuffers();
    cullPipeline.reset();
    if (computePipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vulkanDevice.getDevice(), computePipelineLayout, nullptr);
    computePipelineLayout = VK_NULL_HANDLE;
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

const std::vector<SceneGpuBatch::SubMeshBounds>& SceneGpuBatch::subMeshBoundsFor(const Model& model)
{
    auto it = modelBoundsCache.find(&model);
    if (it != modelBoundsCache.end())
        return it->second;

    // 缓存只按模型数量增长；模型通常只有几十个，超过上限时整表丢弃重算，
    // 避免长期运行后持有已卸载模型的指针。
    if (modelBoundsCache.size() >= kMaxCachedModels)
        modelBoundsCache.clear();

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

    auto result = modelBoundsCache.emplace(&model, std::move(bounds));
    return result.first->second;
}

void SceneGpuBatch::forgetModel(const Model* model)
{
    modelBoundsCache.erase(model);
}

void SceneGpuBatch::buildInstances(FrameInfo& frameInfo)
{
    instanceScratch.clear();
    modelGroups.clear();
    buckets.clear();

    std::unordered_map<const Model*, uint32_t> groupIndex;
    auto view = frameInfo.registry.view<MeshComponent, WorldTransformComponent>();
    for (entt::entity entity : view)
    {
        const MeshComponent& mesh = view.get<MeshComponent>(entity);
        const WorldTransformComponent& worldTransform = view.get<WorldTransformComponent>(entity);
        if (!mesh.model || !worldTransform.valid)
            continue;
        if (mesh.model->getIndexCount() == 0)
            continue;   // 非索引网格留在 CPU 路径（Model::draw 的兼容分支）。

        const RenderTagComponent* tag = frameInfo.registry.try_get<RenderTagComponent>(entity);
        if (tag && (tag->isSkybox || tag->isGrid || tag->isTerrain))
            continue;

        const Model* model = mesh.model.get();
        uint32_t group = 0;
        auto groupIt = groupIndex.find(model);
        if (groupIt == groupIndex.end())
        {
            group = static_cast<uint32_t>(modelGroups.size());
            groupIndex.emplace(model, group);
            ModelGroup newGroup{};
            newGroup.model = model;
            newGroup.modelSlot = group;
            modelGroups.push_back(newGroup);
            buckets.emplace_back();
        }
        else
        {
            group = groupIt->second;
        }

        std::vector<InstanceRecord>& bucket = buckets[group];
        const std::vector<SubMeshBounds>& boundsList = subMeshBoundsFor(*model);
        const std::vector<SubMesh>& subMeshes = model->getSubMeshes();

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

        for (size_t i = 0; i < subMeshes.size(); ++i)
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
                    std::cout << "[SceneGpu] entity " << entityId
                              << " 没有有效材质槽位，使用棋盘格占位着色（仅提示一次）" << std::endl;
                slot = 0u;
                materialValid = false;
            }

            InstanceRecord record{};
            record.model = worldTransform.matrix;
            record.normal = worldTransform.normalMatrix;
            record.flags = glm::uvec4(slot, materialValid ? 1u : 0u,
                                      subMesh.alphaMasked ? 1u : 0u,
                                      (subMesh.albedoFallback || entityAlbedoFallback) ? 1u : 0u);
            record.misc = glm::vec4(subMesh.alphaCutoff, checkerScale, 0.0f, 0.0f);
            record.uvTransform = uvTransform;
            if (i < boundsList.size() && boundsList[i].valid)
            {
                record.boundsMin = glm::vec4(boundsList[i].min, 1.0f);
                record.boundsMax = glm::vec4(boundsList[i].max, 0.0f);
            }
            else
            {
                record.boundsMin = glm::vec4(model->getBoundsMin(), 1.0f);
                record.boundsMax = glm::vec4(model->getBoundsMax(), 0.0f);
            }
            record.draw = glm::uvec4(subMesh.firstIndex, subMesh.indexCount, 0u, group);
            bucket.push_back(record);
        }
    }

    uint32_t commandCursor = 0;
    for (size_t groupValue = 0; groupValue < modelGroups.size(); ++groupValue)
    {
        ModelGroup& group = modelGroups[groupValue];
        std::vector<InstanceRecord>& bucket = buckets[groupValue];
        group.firstInstance = static_cast<uint32_t>(instanceScratch.size());
        group.instanceCount = static_cast<uint32_t>(bucket.size());
        group.commandBase = commandCursor;
        group.eligible = group.instanceCount > 0u;
        commandCursor += group.instanceCount;
        instanceScratch.insert(instanceScratch.end(), bucket.begin(), bucket.end());
        bucket.clear();
    }
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
    stats.visible = words[1];
    stats.culled = words[2];
    // models 由 CPU 侧填（着色器只统计提交/可见/剔除与可见图元数）。
    stats.visibleTriangles = words[4];
    stats.visibleVertices = words[5];
}

void SceneGpuBatch::prepare(VkCommandBuffer commandBuffer, FrameInfo& frameInfo)
{
    if (!available)
        return;
    // --no-gpu-scene：整条路径连剔除 dispatch 都不做。
    if (!frameInfo.settings.gpuSceneEnabled)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    // 同一槽位的统计是两帧前那次 dispatch 的结果（kFramesInFlight=2），GPU 早已完成；
    // 只用于日志与自检，不参与任何绘制决策。
    readStats(frame);

    buildInstances(frameInfo);
    stats.models = static_cast<uint32_t>(modelGroups.size());
    // 本帧真实提交量：GPU 回读的 submitted 是两帧前的，阈值判断与日志都用这个值。
    stats.submitted = static_cast<uint32_t>(instanceScratch.size());


    stats.drawCalls = 0;
    if (instanceScratch.empty())
        return;

    ensureCapacity(static_cast<uint32_t>(instanceScratch.size()),
                   static_cast<uint32_t>(modelGroups.size()));

    FrameResources& res = frames[frame];
    std::memcpy(res.instances.mapped, instanceScratch.data(),
                instanceScratch.size() * sizeof(InstanceRecord));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.instances.allocation, 0, VK_WHOLE_SIZE);

    std::memset(res.counters.mapped, 0, static_cast<size_t>(res.counters.size));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.counters.allocation, 0, VK_WHOLE_SIZE);

    glm::uvec4* ranges = static_cast<glm::uvec4*>(res.ranges.mapped);
    for (size_t i = 0; i < modelGroups.size(); ++i)
    {
        const ModelGroup& group = modelGroups[i];
        ranges[i] = glm::uvec4(group.commandBase, group.instanceCount, group.modelSlot, 0u);
    }
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.ranges.allocation, 0, VK_WHOLE_SIZE);

    FrameData frameData{};
    frameData.viewProjection = frameInfo.projectionMatrix * frameInfo.viewMatrix;
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
    std::memcpy(res.frameData.mapped, &frameData, sizeof(FrameData));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.frameData.allocation, 0, VK_WHOLE_SIZE);

    std::memset(res.stats.mapped, 0, static_cast<size_t>(res.stats.size));
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), res.stats.allocation, 0, VK_WHOLE_SIZE);

    cullPipeline->bind(commandBuffer);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout,
                            0, 1, &res.computeSet, 0, nullptr);

    CullPushConstants push{};
    push.params = glm::uvec4(static_cast<uint32_t>(instanceScratch.size()),
                             static_cast<uint32_t>(modelGroups.size()), 0u, 0u);
    vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CullPushConstants), &push);

    const uint32_t groupCount = (push.params.x + 63u) / 64u;
    vkCmdDispatch(commandBuffer, groupCount, 1u, 1u);

    // 命令与计数是 compute 写的，随后 indirect draw 与 vertex/fragment 要读；
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
	std::cout << "[SceneGpu] indirect batch | models=" << modelGroups.size()
	          << " instances=" << stats.submitted << std::endl;
    }

    if (!available || instanceScratch.empty())
        return;

    const FrameResources& res = frames[frameInfo.frameInFlightIndex];
    for (const ModelGroup& group : modelGroups)
    {
        if (!group.eligible || group.model == nullptr)
            continue;
        group.model->bind(commandBuffer);
        vkCmdDrawIndexedIndirectCount(commandBuffer, res.commands.buffer,
                                      static_cast<VkDeviceSize>(group.commandBase) * 5u * sizeof(uint32_t),
                                      res.counters.buffer,
                                      static_cast<VkDeviceSize>(group.modelSlot) * sizeof(uint32_t),
                                      group.instanceCount, 5u * sizeof(uint32_t));
        stats.drawCalls += 1;
    }

    // 遥测：真实绘制量只能从 GPU 回读（滞后两帧）。命令组数量不等于 draw call
    // 数——一个组内的每个可见实例都是一条间接绘制。
    frameInfo.telemetry.drawCalls += stats.visible;
    frameInfo.telemetry.triangles += stats.visibleTriangles;
    frameInfo.telemetry.vertices += stats.visibleVertices;
}
