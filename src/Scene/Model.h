#pragma once

#include "RHI/Device.h"
#include "Core/MaterialSlot.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include <array>
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
// glTF 节点树（F4）：几何按「节点局部空间」存一份，节点自己的局部变换才是可改的那层。
// kNoModelNode = 这段几何不挂节点（OBJ / 地形 / 体素 / 蒙皮模型），渲染侧按单位矩阵处理。
inline constexpr uint32_t kNoModelNode = 0xFFFFFFFFu;

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

    // F4：这段索引属于 glTF 的哪个节点。
    uint32_t node = kNoModelNode;
};

// ---- 骨骼动画（CPU 蒙皮）----
//
// 引擎的普通管线只认 Vertex，不认 joints/weights。为了不改顶点输入布局和
// 所有渲染 Pass，蒙皮在 CPU 上做：每帧把绑定姿态的顶点变换到当前姿势，
// 写进 host-visible 的动态顶点缓冲，再走原有 bind/draw 路径。
// 代价是角色顶点每帧要过一遍矩阵，但一个角色只有几千个顶点，完全可接受。
enum class AnimationPath
{
    Translation = 0,
    Rotation,
    Scale,
};

struct AnimationChannel
{
    uint32_t node = 0;                 // glTF node index
    AnimationPath path = AnimationPath::Translation;
    bool step = false;                 // STEP 采样；其余按 LINEAR
    std::vector<float> times;          // 秒，升序
    std::vector<glm::vec4> values;     // translation/scale 用 xyz，rotation 用 xyzw
};

struct AnimationClip
{
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

struct SkeletonNode
{
    glm::vec3 translation{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
    int32_t parent = -1;
};

class Model
{
public:
    // glTF 节点：只存 TRS 与父子关系，不存世界矩阵（父改动后子要跟着动）。
    struct Node
    {
        std::string name;
        int32_t parent = -1;
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
        int32_t mesh = -1;

        glm::mat4 localMatrix() const;
    };
    struct Builder
    {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;
        std::vector<SubMesh>  subMeshes;

        // F4：节点树 + 每个节点下的几何在「节点局部空间」的包围盒。
        std::vector<Node> nodes;
        std::vector<glm::vec3> nodeBoundsMin;
        std::vector<glm::vec3> nodeBoundsMax;
        void loadModel(const std::string& filepath);

        // 蒙皮数据（与 vertices 一一对应）。没有蒙皮的模型留空。
        std::vector<glm::uvec4> skinJoints;
        std::vector<glm::vec4>  skinWeights;

        // 骨架：节点按 glTF node index 存，parent=-1 表示根。
        std::vector<SkeletonNode> skeleton;

        // 参与蒙皮的 node index；inverseBindMatrices 与它一一对应。
        std::vector<uint32_t> jointNodes;
        std::vector<glm::mat4> inverseBindMatrices;

        std::vector<AnimationClip> animations;
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

    // F2-f: 地形 chunk 的几何已经拷进共享 arena，Model 只保留元数据
    // （子网格、包围盒、UV 范围）供绘制与编辑器选中使用；释放后 bind() 变成空操作。
    void releaseGpuBuffers();
    bool hasGpuBuffers() const { return vertexBuffer != VK_NULL_HANDLE; }

    void bind(VkCommandBuffer commandBuffer) const;
    void draw(VkCommandBuffer commandBuffer) const;                                 // 画全部子网格
    void drawSubMesh(VkCommandBuffer commandBuffer, const SubMesh& subMesh) const;  // 只画一个子网格

    // ---- 骨骼动画 ----
    bool isSkinned() const { return !jointNodes.empty(); }
    // 动态模型不能进静态 GPU 合批：它的顶点每帧都在变。
    bool isDynamic() const { return isSkinned(); }

    const std::vector<AnimationClip>& getAnimations() const { return animations; }
    int findAnimation(const std::string& name) const;
    float getAnimationDuration(int clipIndex) const;

    // 按 clip 的时间采样骨架，CPU 蒙皮后写入动态顶点缓冲。
    // clipIndex < 0 或无效时回到绑定姿态。
    void applyAnimation(int clipIndex, float time);

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

    // 单个子网格（有一段索引范围）的求交；node 树由 intersectRay 拆开调用。
    bool intersectRayRange(const glm::mat4& worldMatrix,
                           const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                           size_t firstIndex, size_t indexCount,
                           float& outDistance) const;

    // 场景序列化需要知道这个模型来自哪个文件（编辑器保存 / 重载场景时用）。
    // 存的是 Model::createModelFromFile 收到的原始字符串（绝对路径），
    // 因为引擎本来就靠 AssetPaths.h 固化了资源根目录。
    const std::string& getSourcePath() const { return sourcePath; }

    uint32_t getVertexCount() const { return vertexCount; }

    // F2-b：地形 chunk 网格 arena 需要把整块顶点/索引缓冲拷进共享缓冲。
    VkBuffer getVertexBufferHandle() const { return vertexBuffer; }
    VkBuffer getIndexBufferHandle() const { return indexBuffer; }
    bool hasIndexBufferData() const { return hasIndexBuffer; }
    uint32_t getIndexCount()  const { return indexCount; }

    const std::vector<SubMesh>& getSubMeshes() const { return subMeshes; }

    const std::vector<Node>& getNodes() const { return nodes; }
    bool hasNodes() const { return !nodes.empty(); }
    // 节点世界矩阵 = 从根一路乘下来的局部矩阵（模型空间，不含实体变换）。
    glm::mat4 getNodeWorldMatrix(uint32_t node) const;
    // 节点局部包围盒；该节点没有几何时返回 false。
    bool getNodeBounds(uint32_t node, glm::vec3& outMin, glm::vec3& outMax) const;

    // F1：GPU 驱动提交要用 CPU 侧顶点位置算每个子网格的局部 AABB，
    // 才能在 compute 里做子网格级剔除；这里只读，不做任何修改。
    const std::vector<glm::vec3>& getPickingPositions() const { return pickingPositions; }
    const std::vector<uint32_t>& getPickingIndices() const { return pickingIndices; }

    // F1-b：合并顶点/索引缓冲要用完整顶点（法线/UV/颜色），拾取数据只有位置。
    // 地形模型不保留（LOD+裙边会成倍占内存），它们本来也不参与 GPU 批。
    const std::vector<Vertex>& getCpuVertices() const { return cpuVertices; }

    // F1：稳定的几何 id。GPU 批用 id 而不是 Model* 做缓存键——
    // 模型销毁后地址可能被新模型复用，指针键会命中过期包围盒。
    uint64_t getGeometryId() const { return geometryId; }
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

    void initializeSkinning(VkCommandPool commandPool, const Builder& builder);
    void createDynamicVertexBuffer(VkCommandPool commandPool);
    void destroyDynamicVertexBuffers();
    void buildPoseMatrices(const std::vector<SkeletonNode>& nodes,
                           std::vector<glm::mat4>& outJointMatrices) const;
    void skinVertices(const std::vector<SkeletonNode>& nodes,
                      const std::vector<glm::mat4>& jointMatrices,
                      std::vector<Vertex>& outVertices) const;
    void writeDynamicVertices(const std::vector<Vertex>& vertices);
    void updateBoundsAndPicking(const std::vector<Vertex>& vertices,
                                const std::vector<uint32_t>& indices);

    Device& device;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;

    bool hasIndexBuffer = false;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    std::vector<SubMesh> subMeshes;
    std::vector<Node> nodes;
    std::vector<glm::vec3> nodeBoundsMin;
    std::vector<glm::vec3> nodeBoundsMax;
    std::string sourcePath;

    // F1-b：合并缓冲的源数据（地形模型留空）
    std::vector<Vertex> cpuVertices;
    std::vector<uint32_t> cpuIndices;

    // 拾取用的 CPU 侧几何
    uint64_t geometryId = 0;
    std::vector<glm::vec3> pickingPositions;
    std::vector<uint32_t> pickingIndices;

    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };

    glm::vec2 uvMin{ 0.0f };
    glm::vec2 uvMax{ 0.0f };
    bool uvValid = false;

    // ---- 蒙皮 ----
    // 绑定姿态的网格局部顶点。cpuVertices 仍然保留这份数据（供序列化/调试），
    // 动态顶点缓冲初始时也由它蒙皮得到。
    std::vector<glm::uvec4> skinJoints;
    std::vector<glm::vec4>  skinWeights;
    std::vector<SkeletonNode> bindSkeletonNodes;
    std::vector<uint32_t> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
    std::vector<AnimationClip> animations;

    struct DynamicVertexBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    // 3 个环：GPU 可能还在读上一帧/上上帧的顶点缓冲。
    std::array<DynamicVertexBuffer, 3> dynamicVertexBuffers{};
    uint32_t dynamicVertexBufferIndex = 0;
};
// 节点世界矩阵 = 沿 parent 链把 localMatrix() 累乘起来（模型空间，不含实体变换）。
// 独立成自由函数是为了能脱离 Device 直接自检：构造几条 Node 就能验证父链与 TRS 合成。
glm::mat4 modelNodeWorldMatrix(const std::vector<Model::Node>& nodes, uint32_t node);

// glTF 节点树契约自检（不需要 Vulkan / 窗口）：父链合成、TRS 顺序、默认值。
bool modelNodeSelfTest();
