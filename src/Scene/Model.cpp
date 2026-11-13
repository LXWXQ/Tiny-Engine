#include "Core/AssetPack.h"
#include <sstream>
#include "Model.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <cctype>
#include <functional>
#include "RHI/Device.h"
#include "RHI/CommandUtils.h"
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <atomic>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

namespace
{
// 每个模型一个进程内唯一的几何 id：GPU 批的包围盒缓存用它做键。
std::atomic<uint64_t> g_nextGeometryId{ 1 };
} // namespace

glm::mat4 Model::Node::localMatrix() const
{
    glm::mat4 matrix = glm::mat4_cast(rotation);
    matrix[0] *= scale.x;
    matrix[1] *= scale.y;
    matrix[2] *= scale.z;
    matrix[3] = glm::vec4(translation, 1.0f);
    return matrix;
}

glm::mat4 modelNodeWorldMatrix(const std::vector<Model::Node>& nodes, uint32_t node)
{
    glm::mat4 world(1.0f);
    int32_t index = static_cast<int32_t>(node);
    int guard = 0;
    while (index >= 0 && index < static_cast<int32_t>(nodes.size()) && guard++ < 256)
    {
        const Model::Node& current = nodes[static_cast<size_t>(index)];
        world = current.localMatrix() * world;
        index = current.parent;
    }
    return world;
}

glm::mat4 Model::getNodeWorldMatrix(uint32_t node) const
{
    return modelNodeWorldMatrix(nodes, node);
}

bool Model::getNodeBounds(uint32_t node, glm::vec3& outMin, glm::vec3& outMax) const
{
    if (node >= nodes.size() || node >= nodeBoundsMin.size() || node >= nodeBoundsMax.size())
        return false;
    outMin = nodeBoundsMin[node];
    outMax = nodeBoundsMax[node];
    return outMin.x <= outMax.x;
}

Model::Model(Device& device, VkCommandPool commandPool, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
    : device(device)
{
    geometryId = g_nextGeometryId.fetch_add(1, std::memory_order_relaxed);
    createVertexBuffers(vertices, commandPool);
    createIndexBuffers(indices, commandPool);
    computeBounds(vertices);
    computeUvBounds(vertices);
    finalizeSubMeshes({});
    cpuVertices = vertices;
    cpuIndices = indices;
}

Model::Model(Device& device, VkCommandPool commandPool, const Model::Builder& builder)
    : device(device)
{
    geometryId = g_nextGeometryId.fetch_add(1, std::memory_order_relaxed);
    createVertexBuffers(builder.vertices, commandPool);
    createIndexBuffers(builder.indices, commandPool);
    computeUvBounds(builder.vertices);
    finalizeSubMeshes(builder.subMeshes);
    // 地形模型每块 LOD 都是独立几何，保留 CPU 副本太贵；它们不走 GPU 批。
    const bool keepCpuGeometry = std::none_of(
        builder.subMeshes.begin(), builder.subMeshes.end(),
        [](const SubMesh& subMesh) { return subMesh.isTerrainChunk; });
    if (keepCpuGeometry)
    {
        cpuVertices = builder.vertices;
        cpuIndices = builder.indices;
    }

    // 蒙皮模型：保留网格局部绑定姿态顶点，后面每帧由 CPU 重新蒙皮。
    // 注意不能用 node world 烘进顶点，否则 jointMatrix 会把变换算两遍。
    const bool skinned =
        !builder.skinJoints.empty()
        && builder.skinJoints.size() == builder.vertices.size()
        && builder.skinWeights.size() == builder.vertices.size()
        && !builder.skeleton.empty()
        && !builder.jointNodes.empty()
        && builder.jointNodes.size() == builder.inverseBindMatrices.size();
    if (skinned)
    {
        initializeSkinning(commandPool, builder);
        return;
    }

    nodes = builder.nodes;
    nodeBoundsMin = builder.nodeBoundsMin;
    nodeBoundsMax = builder.nodeBoundsMax;

    // F4：几何现在存在「节点局部空间」，模型空间 AABB 必须逐节点变换后求并，
    // 否则包围盒、相机自动取景、剔除半径全部会缩到一个节点的大小。
    // 没有节点信息的模型（OBJ / 地形 / 体素）仍走原来的顶点直算。
    bool boundsFromNodes = false;
    if (!nodes.empty() && nodeBoundsMin.size() == nodes.size() &&
        nodeBoundsMax.size() == nodes.size())
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(nodes.size()); ++i)
        {
            glm::vec3 lo;
            glm::vec3 hi;
            if (!getNodeBounds(i, lo, hi))
                continue;
            const glm::mat4 world = getNodeWorldMatrix(i);
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 point((corner & 1) ? hi.x : lo.x,
                                      (corner & 2) ? hi.y : lo.y,
                                      (corner & 4) ? hi.z : lo.z);
                const glm::vec3 transformed = glm::vec3(world * glm::vec4(point, 1.0f));
                if (!boundsFromNodes)
                {
                    boundsMin = transformed;
                    boundsMax = transformed;
                    boundsFromNodes = true;
                }
                else
                {
                    boundsMin = glm::min(boundsMin, transformed);
                    boundsMax = glm::max(boundsMax, transformed);
                }
            }
        }
    }
    if (!boundsFromNodes)
        computeBounds(builder.vertices);

    // 保留一份 CPU 侧几何用于编辑器点选。
    // 只拷位置：法线/UV 对射线求交没有意义，Sponza 那种规模能省下 2/3 内存。
    pickingPositions.reserve(builder.vertices.size());
    for (const Vertex& v : builder.vertices)
        pickingPositions.push_back(v.position);
    // Terrain stores every LOD plus skirts in one Model. Picking uses only the
    // LOD0 surface range: other LODs are duplicates and skirts are not ground.
    const bool hasTerrainLods = std::any_of(
        subMeshes.begin(), subMeshes.end(),
        [](const SubMesh& subMesh) { return subMesh.isTerrainChunk; });
    if (hasTerrainLods)
    {
        pickingIndices.clear();
        for (const SubMesh& subMesh : subMeshes)
        {
            if (!subMesh.isTerrainChunk || subMesh.terrainLod != 0u)
                continue;
            const uint32_t surfaceCount = std::min(
                subMesh.terrainSurfaceIndexCount, subMesh.indexCount);
            const uint32_t end = subMesh.firstIndex + surfaceCount;
            if (subMesh.firstIndex < builder.indices.size())
            {
                const uint32_t clampedEnd = std::min(
                    end, static_cast<uint32_t>(builder.indices.size()));
                pickingIndices.insert(pickingIndices.end(),
                                      builder.indices.begin() + subMesh.firstIndex,
                                      builder.indices.begin() + clampedEnd);
            }
        }
    }
    else
    {
        pickingIndices = builder.indices;
    }
}

Model::Model(Device& device, GpuMeshTag,
             VkBuffer vertexBufferArg, VmaAllocation vertexAllocationArg,
             uint32_t vertexCountArg,
             VkBuffer indexBufferArg, VmaAllocation indexAllocationArg,
             uint32_t indexCountArg,
             std::vector<SubMesh> subMeshesArg,
             std::vector<glm::vec3> pickingPositionsArg,
             std::vector<uint32_t> pickingIndicesArg,
             const glm::vec3& boundsMinArg, const glm::vec3& boundsMaxArg,
             const glm::vec2& uvMinArg, const glm::vec2& uvMaxArg, bool uvValidArg)
    : device(device),
      vertexBuffer(vertexBufferArg), vertexAllocation(vertexAllocationArg),
      vertexCount(vertexCountArg),
      hasIndexBuffer(indexCountArg > 0u),
      indexBuffer(indexBufferArg), indexAllocation(indexAllocationArg),
      indexCount(indexCountArg),
      subMeshes(std::move(subMeshesArg)),
      pickingPositions(std::move(pickingPositionsArg)),
      pickingIndices(std::move(pickingIndicesArg)),
      boundsMin(boundsMinArg), boundsMax(boundsMaxArg),
      uvMin(uvMinArg), uvMax(uvMaxArg), uvValid(uvValidArg)
{
    if (subMeshes.empty() && indexCount > 0u)
        subMeshes.push_back(SubMesh{ 0, indexCount, kInvalidMaterialSlot });
}

std::shared_ptr<Model> Model::createFromGpuMesh(
    Device& device,
    VkBuffer vertexBufferArg, VmaAllocation vertexAllocationArg, uint32_t vertexCountArg,
    VkBuffer indexBufferArg, VmaAllocation indexAllocationArg, uint32_t indexCountArg,
    std::vector<SubMesh> subMeshesArg,
    std::vector<glm::vec3> pickingPositionsArg,
    std::vector<uint32_t> pickingIndicesArg,
    const glm::vec3& boundsMinArg, const glm::vec3& boundsMaxArg,
    const glm::vec2& uvMinArg, const glm::vec2& uvMaxArg, bool uvValidArg)
{
    return std::shared_ptr<Model>(new Model(
        device, GpuMeshTag{}, vertexBufferArg, vertexAllocationArg, vertexCountArg,
        indexBufferArg, indexAllocationArg, indexCountArg,
        std::move(subMeshesArg), std::move(pickingPositionsArg),
        std::move(pickingIndicesArg), boundsMinArg, boundsMaxArg,
        uvMinArg, uvMaxArg, uvValidArg));
}

void Model::releaseGpuBuffers()
{
    if (vertexBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(device.getVmaAllocator(), vertexBuffer, vertexAllocation);
        vertexBuffer = VK_NULL_HANDLE;
        vertexAllocation = VK_NULL_HANDLE;
    }
    if (hasIndexBuffer && indexBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(device.getVmaAllocator(), indexBuffer, indexAllocation);
        indexBuffer = VK_NULL_HANDLE;
        indexAllocation = VK_NULL_HANDLE;
        hasIndexBuffer = false;
    }
}
Model::~Model()
{
    destroyDynamicVertexBuffers();
    vmaDestroyBuffer(device.getVmaAllocator(), vertexBuffer, vertexAllocation);

    if (hasIndexBuffer)
    {
        vmaDestroyBuffer(device.getVmaAllocator(), indexBuffer, indexAllocation);
    }
}

// ---------------------------------------------------------------------------
// 射线与模型求交（Möller-Trumbore）
//
// 把射线变换到模型的**局部空间**再测，而不是把几十万个顶点变换到世界空间 ——
// 后者每点一次要乘一遍矩阵，前者只乘一次。
//
// 注意：局部空间里的射线方向不再归一化（模型缩放会改变长度），
// 所以最后要把参数 t 换算回世界空间的真实距离：
//   worldDistance = t * |localDir| / |worldDir|
// 调用方传进来的 worldDir 是归一化的，因此 worldDistance = t * |localDir|。
// ---------------------------------------------------------------------------
bool Model::intersectRay(const glm::mat4& worldMatrix,
                         const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                         float& outDistance) const
{
    // 没有节点树（OBJ / 地形 / 体素）时几何本身就存在模型空间，行为与以前完全一致。
    if (!hasNodes())
        return intersectRayRange(worldMatrix, rayOrigin, rayDir, 0, pickingIndices.size(), outDistance);

    // 有节点树：每个子网格的几何存在各自的节点局部空间，先乘出该子网格的实例矩阵再求交。
    bool hit = false;
    float nearest = std::numeric_limits<float>::max();
    for (const SubMesh& subMesh : subMeshes)
    {
        const glm::mat4 instanceWorld = (subMesh.node == kNoModelNode)
            ? worldMatrix
            : worldMatrix * getNodeWorldMatrix(subMesh.node);
        float distance = 0.0f;
        if (!intersectRayRange(instanceWorld, rayOrigin, rayDir,
                               subMesh.firstIndex, subMesh.indexCount, distance))
            continue;
        if (distance < nearest)
        {
            nearest = distance;
            hit = true;
        }
    }
    if (hit)
        outDistance = nearest;
    return hit;
}

bool Model::intersectRayRange(const glm::mat4& worldMatrix,
                              const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                              size_t firstIndex, size_t indexCount,
                              float& outDistance) const
{
    if (pickingPositions.empty() || indexCount < 3)
        return false;
    const glm::mat4 inverseWorld = glm::inverse(worldMatrix);
    const glm::vec3 localOrigin = glm::vec3(inverseWorld * glm::vec4(rayOrigin, 1.0f));
    const glm::vec3 localDir = glm::vec3(inverseWorld * glm::vec4(rayDir, 0.0f));

    const float localDirLength = glm::length(localDir);
    if (localDirLength < 1e-12f)
        return false;

    float nearest = std::numeric_limits<float>::max();

    const size_t triBegin = firstIndex / 3;
    const size_t triEnd = std::min<size_t>(triBegin + indexCount / 3,
                                           pickingIndices.size() / 3);
    for (size_t tri = triBegin; tri < triEnd; ++tri)
    {
        const uint32_t i0 = pickingIndices[tri * 3 + 0];
        const uint32_t i1 = pickingIndices[tri * 3 + 1];
        const uint32_t i2 = pickingIndices[tri * 3 + 2];
        if (i0 >= pickingPositions.size() || i1 >= pickingPositions.size() || i2 >= pickingPositions.size())
            continue;

        const glm::vec3& v0 = pickingPositions[i0];
        const glm::vec3& v1 = pickingPositions[i1];
        const glm::vec3& v2 = pickingPositions[i2];

        const glm::vec3 edge1 = v1 - v0;
        const glm::vec3 edge2 = v2 - v0;
        const glm::vec3 h = glm::cross(localDir, edge2);
        const float a = glm::dot(edge1, h);
        if (std::abs(a) < 1e-12f)
            continue;      // 射线与三角形平行

        const float f = 1.0f / a;
        const glm::vec3 s = localOrigin - v0;
        const float u = f * glm::dot(s, h);
        if (u < 0.0f || u > 1.0f)
            continue;

        const glm::vec3 q = glm::cross(s, edge1);
        const float v = f * glm::dot(localDir, q);
        if (v < 0.0f || u + v > 1.0f)
            continue;

        const float t = f * glm::dot(edge2, q);
        if (t > 1e-6f && t < nearest)
            nearest = t;
    }

    if (nearest == std::numeric_limits<float>::max())
        return false;

    // 局部参数 t 乘以 |localDir| 得到世界空间距离（调用方传入的 worldDir 已归一化）
    outDistance = nearest * localDirLength;
    return true;
}

void Model::computeBounds(const std::vector<Vertex>& vertices)
{
    if (vertices.empty()) return;

    boundsMin = vertices[0].position;
    boundsMax = vertices[0].position;
    for (const Vertex& v : vertices)
    {
        boundsMin = glm::min(boundsMin, v.position);
        boundsMax = glm::max(boundsMax, v.position);
    }
}

void Model::computeUvBounds(const std::vector<Vertex>& vertices)
{
    uvValid = false;
    if (vertices.empty()) return;

    uvMin = vertices[0].uv;
    uvMax = vertices[0].uv;
    for (const Vertex& v : vertices)
    {
        uvMin = glm::min(uvMin, v.uv);
        uvMax = glm::max(uvMax, v.uv);
    }

    const glm::vec2 extent = uvMax - uvMin;
    uvValid = (extent.x > 1e-5f || extent.y > 1e-5f);
}

void Model::finalizeSubMeshes(const std::vector<SubMesh>& src)
{
    subMeshes = src;

    // 兜底：没有子网格信息时，把整块索引当成一个子网格。
    // 这样渲染侧统一"遍历子网格"，OBJ 与 glTF 走同一套逻辑。
    if (subMeshes.empty() && indexCount > 0)
        subMeshes.push_back(SubMesh{ 0, indexCount, kInvalidMaterialSlot });
}

void Model::createVertexBuffers(const std::vector<Vertex>& vertices, VkCommandPool commandPool)
{
    vertexCount = static_cast<uint32_t>(vertices.size());
    VkDeviceSize bufferSize = sizeof(Vertex) * vertexCount;

    // 目标缓冲放在 Device-Local（显存），GPU 读取带宽最高。
    // Sponza 这类模型顶点数很大，继续用 Host-Visible 会白白吃掉大量可映射内存。
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    // F2-g：地形 chunk 的几何会被拷进共享 arena（vkCmdCopyBuffer 的源），所以要带 TRANSFER_SRC。
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
        &vertexBuffer, &vertexAllocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create vertex buffer!");

    uploadViaStaging(vertexBuffer, vertices.data(), bufferSize, commandPool,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
}

void Model::createIndexBuffers(const std::vector<uint32_t>& indices, VkCommandPool commandPool)
{
    indexCount = static_cast<uint32_t>(indices.size());
    hasIndexBuffer = indexCount > 0;
    if (!hasIndexBuffer) return;

    VkDeviceSize bufferSize = sizeof(uint32_t) * indexCount;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
        &indexBuffer, &indexAllocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create index buffer!");

    uploadViaStaging(indexBuffer, indices.data(), bufferSize, commandPool,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT);
}

void Model::uploadViaStaging(VkBuffer dstBuffer, const void* srcData, VkDeviceSize size,
    VkCommandPool commandPool, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    // 1) staging buffer（Host-Visible）承接 CPU 数据
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = size;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    if (vmaCreateBuffer(device.getVmaAllocator(), &stagingInfo, &stagingAllocInfo,
        &stagingBuffer, &stagingAllocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create staging buffer!");

    vmaCopyMemoryToAllocation(device.getVmaAllocator(), srcData, stagingAllocation, 0, size);

    // 2) 一次性命令缓冲，在 GPU 侧完成拷贝
    VkCommandBuffer cmd = cmdBeginSingleTime(device.getDevice(), commandPool);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, stagingBuffer, dstBuffer, 1, &copyRegion);

    // 3) 让 TRANSFER 的写入对后续的顶点/索引读取可见。
    //    即使本函数末尾会 vkQueueWaitIdle，这个屏障依然必需：
    //    队列空闲只保证"执行完成"，并不建立跨阶段的"内存依赖"。
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = dstBuffer;
    barrier.offset = 0;
    barrier.size = size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage, 0,
        0, nullptr, 1, &barrier, 0, nullptr);

    cmdEndSingleTime(device.getDevice(), device.getGraphicsQueue(), commandPool, cmd);

    vmaDestroyBuffer(device.getVmaAllocator(), stagingBuffer, stagingAllocation);
}

void Model::bind(VkCommandBuffer commandBuffer) const
{
    VkBuffer boundVertexBuffer = vertexBuffer;
    if (isSkinned())
    {
        const DynamicVertexBuffer& dynamic = dynamicVertexBuffers[dynamicVertexBufferIndex];
        if (dynamic.buffer != VK_NULL_HANDLE)
            boundVertexBuffer = dynamic.buffer;
    }
    if (boundVertexBuffer == VK_NULL_HANDLE)
        return;

    VkBuffer buffers[] = { boundVertexBuffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);

    if (hasIndexBuffer)
    {
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    }
}

void Model::draw(VkCommandBuffer commandBuffer) const
{
    if (hasIndexBuffer)
    {
        vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
    }
    else
    {
        vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
    }
}

void Model::drawSubMesh(VkCommandBuffer commandBuffer, const SubMesh& subMesh) const
{
    if (!hasIndexBuffer || subMesh.indexCount == 0)
        return;
    vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, 1, subMesh.firstIndex, 0, 0);
}

int Model::findAnimation(const std::string& name) const
{
    for (size_t i = 0; i < animations.size(); ++i)
    {
        if (animations[i].name == name)
            return static_cast<int>(i);
    }

    // glTF 里大小写并不统一（有的导出器写 Walk，有的写 walk）。
    auto lower = [](std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string wanted = lower(name);
    for (size_t i = 0; i < animations.size(); ++i)
    {
        if (lower(animations[i].name) == wanted)
            return static_cast<int>(i);
    }
    return -1;
}

float Model::getAnimationDuration(int clipIndex) const
{
    if (clipIndex < 0 || static_cast<size_t>(clipIndex) >= animations.size())
        return 0.0f;
    return animations[static_cast<size_t>(clipIndex)].duration;
}

void Model::initializeSkinning(VkCommandPool commandPool, const Builder& builder)
{
    skinJoints = builder.skinJoints;
    skinWeights = builder.skinWeights;
    bindSkeletonNodes = builder.skeleton;
    jointNodes = builder.jointNodes;
    inverseBindMatrices = builder.inverseBindMatrices;
    animations = builder.animations;

    // 权重归一化：有的导出器会把 4 个权重写成 0.999 之类的近似值。
    for (glm::vec4& weights : skinWeights)
    {
        const float sum = weights.x + weights.y + weights.z + weights.w;
        if (sum > 1e-6f)
            weights /= sum;
    }

    createDynamicVertexBuffer(commandPool);

    std::vector<glm::mat4> jointMatrices;
    buildPoseMatrices(bindSkeletonNodes, jointMatrices);
    std::vector<Vertex> skinnedVertices;
    skinVertices(bindSkeletonNodes, jointMatrices, skinnedVertices);
    writeDynamicVertices(skinnedVertices);
    updateBoundsAndPicking(skinnedVertices, cpuIndices);

    float normalLengthSum = 0.0f;
    for (const Vertex& vertex : skinnedVertices)
        normalLengthSum += glm::length(vertex.normal);
    const float averageNormalLength = skinnedVertices.empty()
        ? 0.0f : normalLengthSum / static_cast<float>(skinnedVertices.size());

    std::cout << "[Model] skinned | vertices=" << skinJoints.size()
              << " joints=" << jointNodes.size()
              << " animations=" << animations.size()
              << " bounds=" << (boundsMax.y - boundsMin.y) << "m"
              << " avgNormal=" << averageNormalLength << "\n";
}

void Model::createDynamicVertexBuffer(VkCommandPool commandPool)
{
    if (vertexCount == 0)
        return;

    const VkDeviceSize bufferSize = sizeof(Vertex) * static_cast<VkDeviceSize>(vertexCount);
    VmaAllocator allocator = device.getVmaAllocator();

    for (DynamicVertexBuffer& dynamic : dynamicVertexBuffers)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocationInfo{};
        if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
                &dynamic.buffer, &dynamic.allocation, &allocationInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("蒙皮动态顶点缓冲创建失败");
        }
        dynamic.mapped = allocationInfo.pMappedData;
        if (dynamic.mapped == nullptr)
            throw std::runtime_error("蒙皮动态顶点缓冲没有映射地址");
    }

    (void)commandPool;
}

void Model::destroyDynamicVertexBuffers()
{
    VmaAllocator allocator = device.getVmaAllocator();
    for (DynamicVertexBuffer& dynamic : dynamicVertexBuffers)
    {
        if (dynamic.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator, dynamic.buffer, dynamic.allocation);
        }
        dynamic.buffer = VK_NULL_HANDLE;
        dynamic.allocation = VK_NULL_HANDLE;
        dynamic.mapped = nullptr;
    }
}

void Model::buildPoseMatrices(const std::vector<SkeletonNode>& nodes,
                              std::vector<glm::mat4>& outJointMatrices) const
{
    const size_t nodeCount = nodes.size();
    std::vector<glm::mat4> world(nodeCount, glm::mat4(1.0f));
    std::vector<uint8_t> state(nodeCount, 0);

    std::function<void(size_t)> computeWorld = [&](size_t index)
    {
        if (index >= nodeCount || state[index] == 2)
            return;
        if (state[index] == 1)
        {
            // 理论上 glTF 的节点层级不会成环；真成环时按单位矩阵兜底，
            // 避免递归无限展开。
            world[index] = glm::mat4(1.0f);
            state[index] = 2;
            return;
        }

        state[index] = 1;
        const SkeletonNode& node = nodes[index];
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0f), node.translation)
            * glm::mat4_cast(node.rotation)
            * glm::scale(glm::mat4(1.0f), node.scale);

        if (node.parent >= 0 && static_cast<size_t>(node.parent) < nodeCount)
        {
            computeWorld(static_cast<size_t>(node.parent));
            world[index] = world[static_cast<size_t>(node.parent)] * local;
        }
        else
        {
            world[index] = local;
        }
        state[index] = 2;
    };

    for (size_t i = 0; i < nodeCount; ++i)
        computeWorld(i);

    outJointMatrices.resize(jointNodes.size());
    for (size_t slot = 0; slot < jointNodes.size(); ++slot)
    {
        const uint32_t nodeIndex = jointNodes[slot];
        const glm::mat4 jointWorld = nodeIndex < nodeCount
            ? world[nodeIndex]
            : glm::mat4(1.0f);
        outJointMatrices[slot] = jointWorld * inverseBindMatrices[slot];
    }
}

void Model::skinVertices(const std::vector<SkeletonNode>& nodes,
                         const std::vector<glm::mat4>& jointMatrices,
                         std::vector<Vertex>& outVertices) const
{
    (void)nodes;
    outVertices.resize(cpuVertices.size());
    for (size_t i = 0; i < cpuVertices.size(); ++i)
    {
        const Vertex& base = cpuVertices[i];
        Vertex skinned = base;

        if (i >= skinJoints.size() || i >= skinWeights.size())
        {
            outVertices[i] = skinned;
            continue;
        }

        const glm::uvec4& joints = skinJoints[i];
        const glm::vec4& weights = skinWeights[i];
        glm::vec3 position(0.0f);
        glm::vec3 normal(0.0f);
        float weightSum = 0.0f;

        for (int k = 0; k < 4; ++k)
        {
            const float weight = weights[k];
            if (weight <= 0.0f)
                continue;
            const uint32_t slot = joints[k];
            if (slot >= jointMatrices.size())
                continue;

            const glm::mat4& jointMatrix = jointMatrices[slot];
            position += weight * glm::vec3(jointMatrix * glm::vec4(base.position, 1.0f));
            normal += weight * glm::mat3(jointMatrix) * base.normal;
            weightSum += weight;
        }

        if (weightSum > 1e-6f)
        {
            skinned.position = position;
            skinned.normal = glm::length(normal) > 1e-6f ? glm::normalize(normal) : base.normal;
        }
        outVertices[i] = skinned;
    }
}

void Model::writeDynamicVertices(const std::vector<Vertex>& vertices)
{
    if (vertices.empty())
        return;
    DynamicVertexBuffer& dynamic = dynamicVertexBuffers[dynamicVertexBufferIndex];
    if (dynamic.mapped == nullptr || dynamic.allocation == VK_NULL_HANDLE)
        return;

    const size_t copyCount = std::min(vertices.size(), static_cast<size_t>(vertexCount));
    std::memcpy(dynamic.mapped, vertices.data(), sizeof(Vertex) * copyCount);
    vmaFlushAllocation(device.getVmaAllocator(), dynamic.allocation, 0, VK_WHOLE_SIZE);
}

void Model::updateBoundsAndPicking(const std::vector<Vertex>& vertices,
                                   const std::vector<uint32_t>& indices)
{
    computeBounds(vertices);
    pickingPositions.clear();
    pickingPositions.reserve(vertices.size());
    for (const Vertex& vertex : vertices)
        pickingPositions.push_back(vertex.position);
    pickingIndices = indices;
}

void Model::applyAnimation(int clipIndex, float time)
{
    if (!isSkinned() || cpuVertices.empty())
        return;

    std::vector<SkeletonNode> posed = bindSkeletonNodes;
    const bool hasClip = clipIndex >= 0 && static_cast<size_t>(clipIndex) < animations.size();
    if (hasClip)
    {
        const AnimationClip& clip = animations[static_cast<size_t>(clipIndex)];
        const float clampedTime = glm::clamp(time, 0.0f, clip.duration);
        for (const AnimationChannel& channel : clip.channels)
        {
            if (channel.node >= posed.size() || channel.times.empty() || channel.values.empty())
                continue;

            const size_t timeCount = std::min(channel.times.size(), channel.values.size());
            if (timeCount == 0)
                continue;

            size_t upper = 1;
            while (upper < timeCount && channel.times[upper] < clampedTime)
                ++upper;
            const size_t lower = upper > 0 ? upper - 1 : 0;
            upper = std::min(upper, timeCount - 1);

            glm::vec4 value = channel.values[lower];
            if (upper != lower && !channel.step)
            {
                const float t0 = channel.times[lower];
                const float t1 = channel.times[upper];
                const float span = std::max(t1 - t0, 1e-6f);
                const float alpha = glm::clamp((clampedTime - t0) / span, 0.0f, 1.0f);
                if (channel.path == AnimationPath::Rotation)
                {
                    const glm::quat a(channel.values[lower].w, channel.values[lower].x,
                                      channel.values[lower].y, channel.values[lower].z);
                    const glm::quat b(channel.values[upper].w, channel.values[upper].x,
                                      channel.values[upper].y, channel.values[upper].z);
                    const glm::quat q = glm::normalize(glm::slerp(a, b, alpha));
                    value = glm::vec4(q.x, q.y, q.z, q.w);
                }
                else
                {
                    value = glm::mix(channel.values[lower], channel.values[upper], alpha);
                }
            }

            SkeletonNode& target = posed[channel.node];
            if (channel.path == AnimationPath::Translation)
                target.translation = glm::vec3(value);
            else if (channel.path == AnimationPath::Scale)
                target.scale = glm::vec3(value);
            else
                target.rotation = glm::normalize(glm::quat(value.w, value.x, value.y, value.z));
        }
    }

    std::vector<glm::mat4> jointMatrices;
    buildPoseMatrices(posed, jointMatrices);

    std::vector<Vertex> skinned;
    skinVertices(posed, jointMatrices, skinned);
    dynamicVertexBufferIndex = (dynamicVertexBufferIndex + 1) % dynamicVertexBuffers.size();
    writeDynamicVertices(skinned);
}

std::shared_ptr<Model> Model::createModelFromFile(Device& device, VkCommandPool commandPool, const std::string& filepath)
{
    Builder builder{};
    builder.loadModel(filepath);
    std::cout << "成功加载模型: " << filepath
        << " 顶点数: " << builder.vertices.size()
        << " 索引数: " << builder.indices.size()
        << " 子网格数: " << builder.subMeshes.size() << "\n";
    std::shared_ptr<Model> model = std::make_shared<Model>(device, commandPool, builder);
    model->sourcePath = filepath;
    return model;
}

void Model::Builder::loadModel(const std::string& filepath)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    // 资产包优先：OBJ 从包内读进内存流，未命中再按路径读盘。
    std::vector<uint8_t> packedObj;
    const bool objFromPack = assets::mounts().readFile(filepath, packedObj);
    std::istringstream packedStream;
    if (objFromPack)
        packedStream.str(std::string(packedObj.begin(), packedObj.end()));

    const bool objLoaded = objFromPack
        ? tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &packedStream)
        : tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str());
    if (!objLoaded)
    {
        throw std::runtime_error("模型加载失败: " + warn + err);
    }

    vertices.clear();
    indices.clear();
    subMeshes.clear();
    std::unordered_map<Vertex, uint32_t> uniqueVertices{};   // 顶点去重

    for (const auto& shape : shapes)
    {
        // 每个 shape 记录成一段连续索引 = 一个子网格，
        // 渲染侧统一按子网格遍历，OBJ 与将来的 glTF 走同一套逻辑。
        SubMesh subMesh{};
        subMesh.firstIndex = static_cast<uint32_t>(indices.size());
        subMesh.materialSlot = kInvalidMaterialSlot;   // OBJ 暂不分材质，回退到实体的 MaterialComponent 材质

        for (const auto& index : shape.mesh.indices)
        {
            Vertex vertex{};

            // 提取顶点坐标（OBJ 的数组是铺平的一维数组，所以索引要 * 3）
            vertex.position =
            {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            if (attrib.colors.size() > 0)
            {
                vertex.color =
                {
                    attrib.colors[3 * index.vertex_index + 0],
                    attrib.colors[3 * index.vertex_index + 1],
                    attrib.colors[3 * index.vertex_index + 2]
                };
            }
            else
            {
                vertex.color = { 1.0f, 1.0f, 1.0f };    // 没有顶点色的模型给白色
            }

            if (attrib.normals.size() > 0 && index.normal_index >= 0)
            {
                vertex.normal =
                {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            else
            {
                // 没有法线时写零，而不是伪造 (0,1,0)：
                // 伪造的"朝上"会让占位棋盘格在立方体侧面只沿一个轴变化，
                // 退化成竖条纹；片元着色器看到零法线会用屏幕导数重建几何法线。
                vertex.normal = { 0.0f, 0.0f, 0.0f };
            }

            if (attrib.texcoords.size() > 0 && index.texcoord_index >= 0)
            {
                // Vulkan 的 UV 原点在左上，OBJ / 多数建模软件在左下，需要垂直翻转
                vertex.uv =
                {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
                };
            }
            else
            {
                vertex.uv = { 0.0f, 0.0f };
            }

            if (uniqueVertices.count(vertex) == 0)
            {
                uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
            }

            indices.push_back(uniqueVertices[vertex]);
        }

        subMesh.indexCount = static_cast<uint32_t>(indices.size()) - subMesh.firstIndex;
        if (subMesh.indexCount > 0)
            subMeshes.push_back(subMesh);
    }
}

// ---------------------------------------------------------------------------
// glTF 节点树契约自检（--gltf-node-test）：不需要 Vulkan，只验数学与默认值。
// 几何改成「节点局部空间」之后，世界矩阵全靠这里合成，错了就是整个场景塌到原点。
// ---------------------------------------------------------------------------
namespace
{
    bool nearValue(float a, float b, float eps = 1.0e-5f)
    {
        return std::fabs(a - b) <= eps;
    }

    bool nearVec3(const glm::vec3& a, const glm::vec3& b, float eps = 1.0e-5f)
    {
        return nearValue(a.x, b.x, eps) && nearValue(a.y, b.y, eps) && nearValue(a.z, b.z, eps);
    }
}

bool modelNodeSelfTest()
{
    int cases = 0;
    bool ok = true;
    auto expect = [&ok, &cases](bool condition, const char* what)
    {
        ++cases;
        if (!condition)
        {
            ok = false;
            std::cerr << "[Model] nodeSelfTest FAIL: " << what << '\n';
        }
    };

    // 1) 默认节点：TRS 为单位，局部矩阵是单位矩阵。
    {
        Model::Node node;
        const glm::mat4 local = node.localMatrix();
        bool identity = true;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                identity = identity && nearValue(local[c][r], (c == r) ? 1.0f : 0.0f);
        expect(identity, "default node localMatrix is identity");
    }

    // 2) 空树 / 越界下标：世界矩阵退回单位矩阵，不能崩。
    {
        const std::vector<Model::Node> empty;
        expect(nearVec3(glm::vec3(modelNodeWorldMatrix(empty, 0)[3]), glm::vec3(0.0f)),
               "empty tree world matrix is identity");
    }

    // 3) 父链平移累加：root(1,0,0) -> child(0,2,0)，child 世界位置 = (1,2,0)。
    {
        std::vector<Model::Node> nodes(2);
        nodes[0].translation = glm::vec3(1.0f, 0.0f, 0.0f);
        nodes[1].translation = glm::vec3(0.0f, 2.0f, 0.0f);
        nodes[1].parent = 0;
        expect(nearVec3(glm::vec3(modelNodeWorldMatrix(nodes, 1)[3]), glm::vec3(1.0f, 2.0f, 0.0f)),
               "child world translation accumulates parent translation");
    }

    // 4) 父节点缩放会缩放子节点位移：root scale 2 -> child(1,0,0) 世界位置 = (2,0,0)。
    {
        std::vector<Model::Node> nodes(2);
        nodes[0].scale = glm::vec3(2.0f);
        nodes[1].translation = glm::vec3(1.0f, 0.0f, 0.0f);
        nodes[1].parent = 0;
        expect(nearVec3(glm::vec3(modelNodeWorldMatrix(nodes, 1)[3]), glm::vec3(2.0f, 0.0f, 0.0f)),
               "parent scale scales child translation");
    }

    // 5) 旋转 + 缩放顺序：绕 Y 轴 90°、缩放 2 的节点把 (1,0,0) 送到 (0,0,-2)。
    {
        std::vector<Model::Node> nodes(1);
        nodes[0].rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        nodes[0].scale = glm::vec3(2.0f);
        const glm::mat4 world = modelNodeWorldMatrix(nodes, 0);
        const glm::vec3 mapped = glm::vec3(world * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        expect(nearVec3(mapped, glm::vec3(0.0f, 0.0f, -2.0f)),
               "rotation then scale maps +X to -Z with length 2");
    }

    // 6) 越界下标不能读到别的节点：树里只有一个节点时下标 7 必须是单位矩阵。
    {
        std::vector<Model::Node> nodes(1);
        nodes[0].translation = glm::vec3(5.0f, 5.0f, 5.0f);
        const glm::mat4 world = modelNodeWorldMatrix(nodes, 7);
        expect(nearVec3(glm::vec3(world[3]), glm::vec3(0.0f)), "out-of-range node is identity");
    }

    if (ok)
        std::cout << "[Model] nodeSelfTest PASS | cases=" << cases << " parent-chain=ok trs-order=ok\n";
    return ok;
}