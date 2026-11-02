#pragma once

#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Core/FrameInfo.h"
#include "Core/FrameInFlight.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

class Model;

// F1：普通网格的 GPU 驱动提交。
//
// 以前 GeometrySystem::render 每帧遍历 visibleEntities、逐个子网格做视锥剔除，
// 再逐个 vkCmdDrawIndexed —— 场景一上千级子网格时 CPU 就成了瓶颈。
//
// 现在 CPU 只做一件不需要判断的事：把每个子网格摊平成一条实例记录
// （世界矩阵 + 材质参数 + 局部 AABB + 绘制区间），可见性交给 compute，
// 命令写进 indirect buffer，几何管线按模型分批提交。
//
// 为什么按模型分批而不是一次性 multidraw：一个模型一份顶点/索引缓冲，
// 间接绘制命令不带顶点缓冲绑定，跨模型的命令没法用同一次绘制提交。
// 合并成全局缓冲是 F1 的下一刀（收益是 draw call 从「模型数」降到 1）。
class SceneGpuBatch
{
public:
    // 与 shaders/scene_cull.comp 的 InstanceRecord 严格一致。
    struct InstanceRecord
    {
        glm::mat4 model{ 1.0f };
        glm::mat4 normal{ 1.0f };
        glm::uvec4 flags{ 0u };      // x=materialId y=materialValid z=alphaMasked w=albedoFallback
        glm::vec4 misc{ 0.0f };      // x=alphaCutoff y=checkerScale z=terrainMode
        glm::vec4 uvTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
        glm::vec4 boundsMin{ 0.0f }; // xyz=局部 AABB 最小角 w=1 表示参与剔除
        glm::vec4 boundsMax{ 0.0f };
        glm::uvec4 draw{ 0u };       // x=firstIndex y=indexCount z=vertexOffset w=模型槽位
    };

    // 与着色器 FrustumBuffer 一致。
    struct FrameData
    {
        glm::mat4 viewProjection{ 1.0f };
        glm::vec4 planes[6]{};
    };

    struct Stats
    {
        uint32_t submitted = 0;
        uint32_t visible = 0;
        uint32_t culled = 0;
        uint32_t models = 0;
        uint32_t visibleTriangles = 0;
        uint32_t visibleVertices = 0;
        uint32_t drawCalls = 0;
    };

    explicit SceneGpuBatch(Device& device);
    ~SceneGpuBatch();

    SceneGpuBatch(const SceneGpuBatch&) = delete;
    SceneGpuBatch& operator=(const SceneGpuBatch&) = delete;

    // 绘制管线需要把实例缓冲当 set 3 绑定；没有可用实现时返回 VK_NULL_HANDLE。
    VkDescriptorSetLayout getInstanceSetLayout() const { return instanceSetLayout; }
    VkDescriptorSet getInstanceSet(uint32_t frameInFlightIndex) const
    {
        return frames[frameInFlightIndex].graphicsSet;
    }

    bool isAvailable() const { return available; }

    // 这一帧是否该走 GPU 批：可用 + 开关打开 + （强制 或 实例数达到阈值）。
    // prepare() 已经在本帧更早的 Pass 里跑过，stats.submitted 就是本帧的实例数。
    bool useGpuPath(const FrameInfo& frameInfo) const;
    const Stats& getStats() const { return stats; }

    // 写实例记录 + 派发剔除。必须在几何 Pass 之前录制（无需附件）。
    void prepare(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);

    // 绑定实例集合并按模型提交 indirect 绘制。调用方需已绑定管线与 push constant。
    void recordDraw(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);

    // 模型被销毁后释放缓存的子网格 AABB，避免指针复用带来的错配。
    void forgetModel(const Model* model);

private:
    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct ModelGroup
    {
        const Model* model = nullptr;
        uint32_t firstInstance = 0;
        uint32_t instanceCount = 0;
        uint32_t commandBase = 0;
        uint32_t modelSlot = 0;
        bool eligible = false;
        // F1-b：合并缓冲里的落点（该模型顶点/索引在全局缓冲中的起始位置）。
        uint32_t mergedFirstIndex = 0;
        int32_t mergedBaseVertex = 0;
        bool merged = false;
    };

    struct SubMeshBounds
    {
        glm::vec3 min{ 0.0f };
        glm::vec3 max{ 0.0f };
        bool valid = false;
    };

    struct FrameResources
    {
        GpuBuffer instances;
        GpuBuffer commands;
        GpuBuffer counters;
        GpuBuffer ranges;
        GpuBuffer frameData;
        GpuBuffer stats;
        VkDescriptorSet computeSet = VK_NULL_HANDLE;
        VkDescriptorSet graphicsSet = VK_NULL_HANDLE;
    };

    struct MergedPlacement
    {
        uint32_t firstIndex = 0;
        uint32_t baseVertex = 0;
    };

    struct MergedMesh
    {
        GpuBuffer vertices;
        GpuBuffer indices;
        uint64_t signature = 0;
        bool valid = false;
    };

    void createLayoutsAndPipeline();
    bool ensureMergedMesh(const std::vector<const Model*>& models);
    void destroyMergedMesh();
    void uploadDeviceLocal(GpuBuffer& destination, const void* data, VkDeviceSize size);
    void destroyResources();

    void ensureCapacity(uint32_t instanceCapacity, uint32_t modelCapacity);
    void createFrameBuffers();
    void destroyFrameBuffers();
    void writeDescriptors();

    void createBuffer(GpuBuffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroyBuffer(GpuBuffer& buffer);

    void buildInstances(FrameInfo& frameInfo);
    const std::vector<SubMeshBounds>& subMeshBoundsFor(const Model& model);
    void readStats(uint32_t frameInFlightIndex);

    Device& vulkanDevice;
    bool available = false;

    VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout instanceSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool computePool = VK_NULL_HANDLE;
    VkDescriptorPool graphicsPool = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> cullPipeline;

    std::array<FrameResources, kFramesInFlight> frames{};
    uint32_t instanceCapacity = 0;
    uint32_t modelCapacity = 0;

    std::vector<InstanceRecord> instanceScratch;
    std::vector<ModelGroup> modelGroups;
    std::vector<glm::vec4> planeScratch;
    std::vector<std::vector<InstanceRecord>> buckets;   // 每模型一段，组装实例缓冲时复用容量
    std::unordered_set<uint32_t> warnedFallback;        // 缺材质槽位的实体只提示一次

    std::unordered_map<const Model*, std::vector<SubMeshBounds>> modelBoundsCache;

    // F1-b：全局合并顶点/索引缓冲。签名 = 模型集合，变了才重建（低频）。
    MergedMesh mergedMesh;
    std::unordered_map<const Model*, MergedPlacement> mergedPlacements;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    bool mergedActive = false;
    bool warnedNoMergedData = false;

    Stats stats;

    // 首次真正提交间接命令时打一行日志：既方便确认路径生效，也给回归测试一个标记。
    bool reportedFirstBatch = false;
};
