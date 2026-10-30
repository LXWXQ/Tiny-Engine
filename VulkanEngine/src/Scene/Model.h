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

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    static std::shared_ptr<Model> createModelFromFile(Device& device, VkCommandPool commandPool, const std::string& filepath);

    void bind(VkCommandBuffer commandBuffer) const;
    void draw(VkCommandBuffer commandBuffer) const;                                 // 画全部子网格
    void drawSubMesh(VkCommandBuffer commandBuffer, const SubMesh& subMesh) const;  // 只画一个子网格

    // 场景序列化需要知道这个模型来自哪个文件（编辑器保存 / 重载场景时用）。
    // 存的是 Model::createModelFromFile 收到的原始字符串（绝对路径），
    // 因为引擎本来就靠 AssetPaths.h 固化了资源根目录。
    const std::string& getSourcePath() const { return sourcePath; }

    uint32_t getVertexCount() const { return vertexCount; }
    uint32_t getIndexCount()  const { return indexCount; }

    const std::vector<SubMesh>& getSubMeshes() const { return subMeshes; }

    // 顶点包围盒（AABB）：用来给场景自动摆放相机、做粗略剔除等
    glm::vec3 getBoundsMin() const { return boundsMin; }
    glm::vec3 getBoundsMax() const { return boundsMax; }
    glm::vec3 getBoundsCenter() const { return (boundsMin + boundsMax) * 0.5f; }
    float getBoundsRadius() const { return glm::length(boundsMax - boundsMin) * 0.5f; }

private:
    void createVertexBuffers(const std::vector<Vertex>& vertices, VkCommandPool commandPool);
    void createIndexBuffers(const std::vector<uint32_t>& indices, VkCommandPool commandPool);
    void uploadViaStaging(VkBuffer dstBuffer, const void* srcData, VkDeviceSize size,
        VkCommandPool commandPool, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
    void finalizeSubMeshes(const std::vector<SubMesh>& src);
    void computeBounds(const std::vector<Vertex>& vertices);

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

    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };
};
