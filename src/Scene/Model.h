#pragma once

#include "RHI/Device.h"
#include "Core/MaterialSlot.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include <vector>
#include <memory>
#include <string>

struct Vertex
{
    glm::vec3 position;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 uv;

    bool operator==(const Vertex& other) const
    {
        return position == other.position
            && color == other.color
            && normal == other.normal
            && uv == other.uv;
    }

    static VkVertexInputBindingDescription getBindingDescription()
    {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions()
    {
        std::vector<VkVertexInputAttributeDescription> attributeDescriptions(4);

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, position);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, normal);

        attributeDescriptions[3].binding = 0;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(Vertex, uv);

        return attributeDescriptions;
    }
};

namespace std
{
    template<> struct hash<Vertex>
    {
        size_t operator()(Vertex const& vertex) const
        {
            size_t h1 = hash<glm::vec3>()(vertex.position);
            size_t h2 = hash<glm::vec3>()(vertex.color);
            size_t h3 = hash<glm::vec3>()(vertex.normal);
            size_t h4 = hash<glm::vec2>()(vertex.uv);
            return ((h1 ^ (h2 << 1)) ^ (h3 << 2)) ^ (h4 << 3);
        }
    };
}

// 子网格：索引缓冲里一段连续的索引区间，以及这段区间使用的材质槽位。
// OBJ 模型只有一个子网格；glTF 模型（Sponza 之类）会有几百个。
struct SubMesh
{
    uint32_t firstIndex = 0;      // 索引缓冲内的第一个索引（元素，不是字节）
    uint32_t indexCount = 0;
    // 该子网格使用的材质槽位。为 kInvalidMaterialSlot 表示"未指定"，
    // 渲染时回退到所属实体的 MaterialComponent::materialSetIndex。
    uint32_t materialSlot = kInvalidMaterialSlot;

    // glTF alpha-mask metadata. Foliage must discard transparent texels or
    // the opaque white background of the leaf atlas becomes a visible fringe.
    bool alphaMasked = false;
    float alphaCutoff = 0.5f;

    // 该子网格的材质没有 albedo 贴图（会采样到默认白纹理）。
    // 渲染时用程序化棋盘格代替，避免"没有纹理的网格"变成一坨纯白。
    bool albedoFallback = false;

    // Optional local-space bounds for per-submesh frustum culling. Terrain
    // chunks set these; ordinary OBJ/glTF submeshes leave hasBounds=false.
    bool hasBounds = false;
    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };

    // Terrain LOD grouping. GeometrySystem draws exactly one terrainLod per
    // (terrainChunkX, terrainChunkZ) pair. Other submeshes leave this false.
    bool isTerrainChunk = false;
    uint32_t terrainChunkX = 0;
    uint32_t terrainChunkZ = 0;
    uint32_t terrainLod = 0;
    uint32_t terrainSurfaceIndexCount = 0;
};

class Model
{
public:
    struct Builder
    {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;
        std::vector<SubMesh>  subMeshes;
        void loadModel(const std::string& filepath);
    };

    // 顶点/索引缓冲现在是 Device-Local，需要 commandPool 做 staging 上传
    Model(Device& device, VkCommandPool commandPool, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    Model(Device& device, VkCommandPool commandPool, const Model::Builder& builder);
    ~Model();

    // B-04: adopt device-local buffers produced by TerrainGpuMeshBuilder.
    // Ownership of both VMA allocations transfers to the returned Model.
    static std::shared_ptr<Model> createFromGpuMesh(
        Device& device,
        VkBuffer vertexBuffer, VmaAllocation vertexAllocation, uint32_t vertexCount,
        VkBuffer indexBuffer, VmaAllocation indexAllocation, uint32_t indexCount,
        std::vector<SubMesh> subMeshes,
        std::vector<glm::vec3> pickingPositions,
        std::vector<uint32_t> pickingIndices,
        const glm::vec3& boundsMin, const glm::vec3& boundsMax,
        const glm::vec2& uvMin, const glm::vec2& uvMax, bool uvValid);

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    static std::shared_ptr<Model> createModelFromFile(Device& device, VkCommandPool commandPool, const std::string& filepath);

    void bind(VkCommandBuffer commandBuffer) const;
    void draw(VkCommandBuffer commandBuffer) const;                                 // 画全部子网格
    void drawSubMesh(VkCommandBuffer commandBuffer, const SubMesh& subMesh) const;  // 只画一个子网格

    // ---- CPU 侧拾取几何（编辑器点选用）----
    //
    // 只存位置 + 索引，不存法线/UV：点选只需要三角形位置。
    // Sponza 这种规模大约多占 10MB 内存，换来的是"点一下立刻知道选中谁"。
    //
    // 为什么不用 GPU 拾取（渲染一张 ID buffer）：那要多一个 Pass + 一次读回，
    // 读回还要等一帧；而点选是低频操作，CPU 侧保留一份几何反而更简单可靠。
    //
    // 为什么必须精确到三角形：实体级 AABB 对"相机在物体内部"的情况完全失效
    // —— Sponza 整栋是一个实体，它的 AABB 包住相机，任何射线都以 t≈0 命中它。
    bool intersectRay(const glm::mat4& worldMatrix,
                      const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                      float& outDistance) const;

    // 场景序列化需要知道这个模型来自哪个文件（编辑器保存 / 重载场景时用）。
    // 存的是 Model::createModelFromFile 收到的原始字符串（绝对路径），
    // 因为引擎本来就靠 AssetPaths.h 固化了资源根目录。
    const std::string& getSourcePath() const { return sourcePath; }

    uint32_t getVertexCount() const { return vertexCount; }
    uint32_t getIndexCount()  const { return indexCount; }

    const std::vector<SubMesh>& getSubMeshes() const { return subMeshes; }

    // F1：GPU 驱动提交要用 CPU 侧顶点位置算每个子网格的局部 AABB，
    // 才能在 compute 里做子网格级剔除；这里只读，不做任何修改。
    const std::vector<glm::vec3>& getPickingPositions() const { return pickingPositions; }
    const std::vector<uint32_t>& getPickingIndices() const { return pickingIndices; }

    // F1-b：合并顶点/索引缓冲要用完整顶点（法线/UV/颜色），拾取数据只有位置。
    // 地形模型不保留（LOD+裙边会成倍占内存），它们本来也不参与 GPU 批。
    const std::vector<Vertex>& getCpuVertices() const { return cpuVertices; }
    const std::vector<uint32_t>& getCpuIndices() const { return cpuIndices; }

    // 顶点包围盒（AABB）：用来给场景自动摆放相机、做粗略剔除等
    glm::vec3 getBoundsMin() const { return boundsMin; }
    glm::vec3 getBoundsMax() const { return boundsMax; }
    glm::vec3 getBoundsCenter() const { return (boundsMin + boundsMax) * 0.5f; }
    float getBoundsRadius() const { return glm::length(boundsMax - boundsMin) * 0.5f; }

    // UV 的实际取值范围。不同模型的 UV 可能只占 [0,1] 的一小块
    //（Quad-Sphere 的 UV 只在 0.05~0.43），棋盘格要用这个范围归一化，
    // 否则一张 UV 范围很小的球只会显示两三个大格子。
    bool hasUv() const { return uvValid; }
    glm::vec2 getUvMin() const { return uvMin; }
    glm::vec2 getUvMax() const { return uvMax; }

private:
    struct GpuMeshTag {};
    Model(Device& device, GpuMeshTag,
          VkBuffer vertexBuffer, VmaAllocation vertexAllocation, uint32_t vertexCount,
          VkBuffer indexBuffer, VmaAllocation indexAllocation, uint32_t indexCount,
          std::vector<SubMesh> subMeshes,
          std::vector<glm::vec3> pickingPositions,
          std::vector<uint32_t> pickingIndices,
          const glm::vec3& boundsMin, const glm::vec3& boundsMax,
          const glm::vec2& uvMin, const glm::vec2& uvMax, bool uvValid);

    void createVertexBuffers(const std::vector<Vertex>& vertices, VkCommandPool commandPool);
    void createIndexBuffers(const std::vector<uint32_t>& indices, VkCommandPool commandPool);
    void uploadViaStaging(VkBuffer dstBuffer, const void* srcData, VkDeviceSize size,
        VkCommandPool commandPool, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
    void finalizeSubMeshes(const std::vector<SubMesh>& src);
    void computeBounds(const std::vector<Vertex>& vertices);
    void computeUvBounds(const std::vector<Vertex>& vertices);

    Device& device;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;

    bool hasIndexBuffer = false;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    std::vector<SubMesh> subMeshes;
    std::string sourcePath;

    // F1-b：合并缓冲的源数据（地形模型留空）
    std::vector<Vertex> cpuVertices;
    std::vector<uint32_t> cpuIndices;

    // 拾取用的 CPU 侧几何
    std::vector<glm::vec3> pickingPositions;
    std::vector<uint32_t> pickingIndices;

    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };

    glm::vec2 uvMin{ 0.0f };
    glm::vec2 uvMax{ 0.0f };
    bool uvValid = false;
};
