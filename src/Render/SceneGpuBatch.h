#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "Core/FrameInFlight.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"

class Model;
struct FrameInfo;

// F1：GPU 驱动提交。
//
// CPU 只做两件事：把本帧的网格摊平成实例记录、把 (模型, 子网格) 归成组；
// 可见性判断与间接命令都由 compute 完成，图形侧一条 indirect 提交收尾。
//
// 分组键从「模型」细化到「(模型, 子网格)」：同一个子网格被多个实体复用时，
// 它们的实例在缓冲里连续排列，一条命令带 instanceCount = N 就能一次画完（实例化），
// 而不是每实例一条命令。剔除按组的并集 AABB 做，属于保守剔除（组内任一实例可见 -> 整组可见）。
class SceneGpuBatch
{
public:
    // 与 shaders/scene_cull.comp、shaders/mrt.vert 的 InstanceRecord 逐字节一致。
    struct InstanceRecord
    {
        glm::mat4 model;
        glm::mat4 normal;
        glm::uvec4 flags;      // x=materialId y=materialValid z=alphaMasked w=albedoFallback
        glm::vec4 misc;        // x=alphaCutoff y=checkerScale z=terrainMode w=保留
        glm::vec4 uvTransform; // xy=缩放 zw=偏移
        glm::vec4 boundsMin;   // xyz=局部 AABB 下界 w=1 表示有效
        glm::vec4 boundsMax;
        glm::uvec4 draw;       // 保留：图形侧不再读，命令由 compute 写
    };

    // 与 shaders/scene_cull.comp 的 GroupRecord 逐字节一致。
    struct GroupRecord
    {
        glm::vec4 boundsMin;   // xyz=世界 AABB 下界 w=1 表示有效
        glm::vec4 boundsMax;
        glm::uvec4 draw;       // x=firstIndex y=indexCount z=vertexOffset w=firstInstance
        glm::uvec4 counts;     // x=instanceCount y=materialSlot z=merged w=保留
    };

    // 与 shaders/scene_cull.comp 的 FrameData 一致。
    struct FrameData
    {
        glm::mat4 viewProjection;
        glm::mat4 view;          // Hi-Z 用：世界坐标 -> 线性视深度
        glm::vec4 planes[6];
        glm::uvec4 params;     // x=groupCount
    };

    struct Stats
    {
        uint32_t submitted = 0;
        uint32_t visibleGroups = 0;
        uint32_t culledGroups = 0;
        uint32_t visibleInstances = 0;
        uint32_t visibleTriangles = 0;
        uint32_t visibleVertices = 0;
        uint32_t groups = 0;
        uint32_t drawCalls = 0;
        uint32_t occlusionTests = 0;    // 做过 Hi-Z 测试的组数（滞后两帧）
        uint32_t occlusionCulled = 0;   // 其中被 Hi-Z 判定遮挡的组数
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
    bool useGpuPath(const FrameInfo& frameInfo) const;
    const Stats& getStats() const { return stats; }

    // 写实例记录 + 派发剔除。必须在几何 Pass 之前录制（无需附件）。
    void prepare(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);

    // 绑定实例集合并提交 indirect 绘制。调用方需已绑定管线与 push constant。
    void recordDraw(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);

    // 模型被销毁后释放缓存的子网格 AABB，避免指针复用带来的错配。
    void forgetModel(const Model* model);

    // F1-c：Hi-Z 遮挡剔除。在几何 Pass 之后调用，用本帧的 G-Buffer 位置重建金字塔；
    // 剔除用的是上一帧建好的那一份（错开一帧，相机帧间连续时几乎看不出差别）。
    void buildHiZ(VkCommandBuffer commandBuffer, const FrameInfo& frameInfo,
                  VkImageView positionView, VkExtent2D extent);

    // 登记某个帧槽位的 G-Buffer 位置视图。须在 init()/resize() 里调用：
    // Hi-Z 的构建描述符只在这时写，录制期间不再更新（否则会让在录的命令缓冲失效）。
    void setHiZSource(uint32_t frameInFlightIndex, VkImageView positionView, VkExtent2D extent);

    bool isHiZReady() const { return hizReady; }

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
        uint32_t subMeshIndex = 0;
        uint32_t firstInstance = 0;
        uint32_t instanceCount = 0;
        glm::vec3 boundsMin{ 0.0f };
        glm::vec3 boundsMax{ 0.0f };
        bool hasBounds = false;
        uint32_t materialSlot = 0;
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

    struct CachedBounds
    {
        std::vector<SubMeshBounds> bounds;
        uint64_t lastFrame = 0;
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

    // 每帧槽位一份 Hi-Z 金字塔：mip0 由 G-Buffer 位置生成，之后逐级 2x2 取最远。
    // 布局恒为 GENERAL，屏障全部手动管理（不进 RenderGraph 跟踪）。
    struct HiZFrame
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView fullView = VK_NULL_HANDLE;
        std::vector<VkImageView> mipViews;
        VkExtent2D extent{};
        uint32_t mipLevels = 0;
    };

    struct FrameResources
    {
        GpuBuffer instances;
        GpuBuffer commands;
        GpuBuffer groups;
        GpuBuffer frameData;
        GpuBuffer stats;
        VkDescriptorSet computeSet = VK_NULL_HANDLE;
        VkDescriptorSet graphicsSet = VK_NULL_HANDLE;
    };

    void createLayoutsAndPipeline();
    void destroyResources();

    void ensureCapacity(uint32_t instanceCapacity, uint32_t groupCapacity);
    void createFrameBuffers();
    void destroyFrameBuffers();
    void writeDescriptors();

    void createBuffer(GpuBuffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroyBuffer(GpuBuffer& buffer);

    bool ensureMergedMesh(const std::vector<const Model*>& models);
    void destroyMergedMesh();
    void uploadDeviceLocal(GpuBuffer& destination, const void* data, VkDeviceSize size);

    void ensureHiZ(VkExtent2D extent);
    void destroyHiZ();
    void writeHiZDescriptors();

    void buildInstances(FrameInfo& frameInfo);
    const std::vector<SubMeshBounds>& subMeshBoundsFor(const Model& model, uint64_t frameNumber);
    void readStats(uint32_t frameInFlightIndex);

    Device& vulkanDevice;
    bool available = false;

    VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout instanceSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool computePool = VK_NULL_HANDLE;
    VkDescriptorPool graphicsPool = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> cullPipeline;

    // Hi-Z：构建/降采样两条 compute 管线（共用一套「采样源 + 存储图像」描述符布局）。
    VkDescriptorSetLayout hizSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout hizPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool hizPool = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> hizBuildPipeline;
    std::unique_ptr<ComputePipeline> hizDownsamplePipeline;
    std::array<HiZFrame, kFramesInFlight> hiz{};
    std::array<std::vector<VkDescriptorSet>, kFramesInFlight> hizSets{};
    std::array<VkImageView, kFramesInFlight> hizPositionViews{};
    VkSampler hizSampler = VK_NULL_HANDLE;
    VkExtent2D hizExtent{};
    bool hizReady = false;

    std::array<FrameResources, kFramesInFlight> frames{};
    uint32_t instanceCapacity = 0;
    uint32_t groupCapacity = 0;

    std::vector<InstanceRecord> instanceScratch;
    std::vector<ModelGroup> modelGroups;
    std::vector<glm::vec4> planeScratch;
    std::vector<std::vector<InstanceRecord>> buckets;
    std::vector<std::vector<uint32_t>> bucketEntities;
    std::unordered_set<uint32_t> warnedFallback;

    std::unordered_map<uint64_t, CachedBounds> modelBoundsCache;

    MergedMesh mergedMesh;
    std::unordered_map<uint64_t, MergedPlacement> mergedPlacements;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    bool mergedActive = false;
    bool warnedNoMergedData = false;

    Stats stats;

    bool reportedFirstBatch = false;
};
