#pragma once
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Core/FrameInfo.h"
#include "Render/SubMeshCulling.h"
#include "Render/TerrainMeshArena.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SceneGpuBatch;

// 几何渲染逻辑：持有 3 条管线（标准物体 / 天空盒 / 网格）。
// D-1(b) 起管线按「附件格式」创建（动态渲染），不再需要 VkRenderPass。
class GeometrySystem
{
public:
    GeometrySystem(Device& device, VkFormat depthFormat,
                   VkDescriptorSetLayout globalSetLayout,
                   VkDescriptorSetLayout materialSetLayout,
                   VkDescriptorSetLayout terrainSetLayout,
                   VkDescriptorSetLayout sceneInstanceSetLayout,
                   SceneGpuBatch* sceneGpuBatch,
                   TerrainMeshArena* terrainMeshArena);
    ~GeometrySystem();

    GeometrySystem(const GeometrySystem&) = delete;
    GeometrySystem& operator=(const GeometrySystem&) = delete;

    void prepareTerrainGpuSelection(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);
    void render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }

private:
    void createPipelineLayout(VkDescriptorSetLayout globalSetLayout,
                              VkDescriptorSetLayout materialSetLayout,
                              VkDescriptorSetLayout terrainSetLayout,
                              VkDescriptorSetLayout sceneInstanceSetLayout);
    void createPipelines();
    void createTerrainIndirectBuffers();
    void destroyTerrainIndirectBuffers();
    void createTerrainGpuSelection();
    void destroyTerrainGpuSelection();
    struct TerrainGpuBuffer;
    void createTerrainGpuBuffer(TerrainGpuBuffer& buffer, VkDeviceSize size,
                                VkBufferUsageFlags usage, bool hostVisible);
    void destroyTerrainGpuBuffer(TerrainGpuBuffer& buffer);

    Device& vulkanDevice;
    // F1：普通网格的 GPU 驱动提交。裸指针不持有所有权，生命周期由 GeometryPass 管。
    SceneGpuBatch* sceneGpuBatch = nullptr;
    VkDescriptorSetLayout sceneInstanceSetLayout = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> geometryPipeline;
    std::unique_ptr<Pipeline> skyboxPipeline;
    std::unique_ptr<Pipeline> gridPipeline;

    // 没有材质槽位的子网格会每帧走一次 fallback 分支。提示只发一次：
    // 每帧写 stderr 既刷屏，又会破坏"stderr 0 字节"的回归判据。
    std::unordered_set<uint32_t> warnedFallbackEntities;

    struct TerrainEntityLodState
    {
        const Model* model = nullptr;
        uint64_t revision = ~0ull;
        uint64_t lastFrame = 0;
        std::array<render::TerrainLodState, 16> chunks{};
    };
    std::unordered_map<uint32_t, TerrainEntityLodState> terrainLodStates;

    struct TerrainIndirectBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        uint32_t capacity = 0;
    };
    static constexpr uint32_t kTerrainIndirectCapacity = 32768;
    std::array<TerrainIndirectBuffer, kFramesInFlight> terrainIndirectBuffers{};


    // -----------------------------------------------------------------------
    // B-03: GPU-driven terrain selection.
    // -----------------------------------------------------------------------
    // F2-h: the slot count is bounded by how many chunks can be visible at once,
    // not by the streaming ring size. Radius 24 is 2401 chunks; the 60 degree
    // frustum plus per-chunk LOD keeps the visible set around a few hundred, so
    // 1024 slots cover it while the buffers stay small (records/instances only).
    static constexpr uint32_t kTerrainGpuMaxEntities = 1024;
    static constexpr uint32_t kTerrainGpuChunksPerEntity = 16;
    static constexpr uint32_t kTerrainGpuCommandsPerEntity = 16;
    static constexpr uint32_t kTerrainGpuMaxRecords =
        kTerrainGpuMaxEntities * kTerrainGpuChunksPerEntity;
    static constexpr uint32_t kTerrainGpuStatsCount = 9;
    static constexpr uint32_t kTerrainGpuDebugWords = 8;

    struct TerrainGpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct TerrainGpuChunkRecord
    {
        glm::mat4 worldMatrix{ 1.0f };
        glm::vec4 boundsMin{ 0.0f };
        glm::vec4 boundsMax{ 0.0f };
        glm::vec4 lodParams{ 1.0f, 1.0f, 0.0f, 0.0f };
        glm::uvec4 commandParams{ 0u };
        glm::uvec4 lod0{ 0u };
        glm::uvec4 lod1{ 0u };
        glm::uvec4 lod2{ 0u };
        glm::uvec4 arena{ 0u }; // x=index base y=vertex base z=adopted
    };

    struct TerrainGpuEntitySlot
    {
        uint32_t slot = 0;
        const Model* model = nullptr;
        uint64_t revision = ~0ull;
        uint64_t lastFrame = 0;
        bool resetPending = false;
        // F2-g：arena 切片由 Scene 管，实体槽只记录它出现在哪一帧的实例表里。
    };

    struct TerrainGpuFrameResources
    {
        TerrainGpuBuffer records{};
        TerrainGpuBuffer state{};
        TerrainGpuBuffer counts{};
        TerrainGpuBuffer stats{};
        TerrainGpuBuffer debug{};
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        bool debugValid = false;
        uint32_t recordCount = 0;
        std::vector<std::array<uint32_t, kTerrainGpuDebugWords>> parityExpected;
    };

    std::unique_ptr<ComputePipeline> terrainGpuPipeline;
    VkPipelineLayout terrainGpuPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout terrainGpuSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool terrainGpuDescriptorPool = VK_NULL_HANDLE;
    std::array<TerrainGpuFrameResources, kFramesInFlight> terrainGpuFrames{};
    std::unordered_map<uint32_t, TerrainGpuEntitySlot> terrainGpuEntitySlots;
    std::array<bool, kTerrainGpuMaxEntities> terrainGpuSlotUsed{};
    std::array<std::unordered_map<uint32_t, render::TerrainLodState>,
               kFramesInFlight> terrainGpuParityStates{};
    bool terrainGpuSelectionAvailable = false;
    uint32_t terrainGpuParityPasses = 0;
    bool terrainGpuParityLogged = false;
    bool terrainGpuParityFailed = false;
    bool terrainGpuTransitionLogged = false;

    struct TerrainGpuRunContext
    {
        bool enabled = false;
        TerrainGpuFrameResources* frame = nullptr;
        std::unordered_map<uint32_t, uint32_t> entitySlots;
        uint32_t entityCount = 0;
        bool batchActive = false; // all visible chunks adopted into the arena
    };

    // F2-b: terrain chunk meshes copied into one arena so a single indirect draw
    // covers every chunk; per-chunk transforms come from a set 3 instance table.
    static constexpr uint32_t kTerrainChunkInstanceStride = 224u; // SceneGpuBatch::InstanceRecord
    // F2-f：arena 由 Scene 持有并负责把几何拷进去，这里只借用指针绘制。
    TerrainMeshArena* terrainMeshArena = nullptr;
    std::array<TerrainGpuBuffer, kFramesInFlight> terrainChunkInstances{};
    std::array<VkDescriptorSet, kFramesInFlight> terrainChunkInstanceSets{};
    VkDescriptorPool terrainChunkInstancePool = VK_NULL_HANDLE;
    bool terrainChunkInstancesReady = false;
    bool terrainArenaWarned = false;
    bool terrainBatchWasActive = false;
    bool ensureTerrainChunkInstances();
    void destroyTerrainChunkInstances();
    TerrainGpuRunContext currentTerrainGpuRun;
};