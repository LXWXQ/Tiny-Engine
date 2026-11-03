#include "Model.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <limits>
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
    computeBounds(builder.vertices);
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
    if (pickingPositions.empty() || pickingIndices.size() < 3)
        return false;

    const glm::mat4 inverseWorld = glm::inverse(worldMatrix);
    const glm::vec3 localOrigin = glm::vec3(inverseWorld * glm::vec4(rayOrigin, 1.0f));
    const glm::vec3 localDir = glm::vec3(inverseWorld * glm::vec4(rayDir, 0.0f));

    const float localDirLength = glm::length(localDir);
    if (localDirLength < 1e-12f)
        return false;

    float nearest = std::numeric_limits<float>::max();

    const size_t triangleCount = pickingIndices.size() / 3;
    for (size_t tri = 0; tri < triangleCount; ++tri)
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
    if (vertexBuffer == VK_NULL_HANDLE)
        return;

    VkBuffer buffers[] = { vertexBuffer };
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

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str()))
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
