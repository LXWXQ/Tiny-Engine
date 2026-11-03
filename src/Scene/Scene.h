#pragma once

#include "Scene/Components.h"
#include "Scene/SceneDescription.h"
#include "Scene/Camera.h"
#include "Scene/CameraController.h"
#include "Scene/SpatialIndex.h"
#include "Core/MaterialSystem.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInfo.h"
#include "RHI/Device.h"
#include "RHI/Texture.h"
#include "Render/TerrainMaterial.h"
#include "Render/TerrainComputeGenerator.h"
#include "Render/TerrainGpuMeshBuilder.h"
#include "Render/TerrainMeshArena.h"
#include "Terrain/TerrainBrush.h"

#include <GLFW/glfw3.h>
#include <entt/entt.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TerrainMaterial;
class JobSystem;

class Scene
{
public:
    // F2-h：流送 chunk 的整数坐标（int64，半径再大也不溢出）。
    struct TerrainChunkCoord
    {
        int64_t x = 0;
        int64_t z = 0;
        bool operator==(const TerrainChunkCoord& other) const
        {
            return x == other.x && z == other.z;
        }
    };
    struct TerrainChunkCoordHash
    {
        size_t operator()(const TerrainChunkCoord& coord) const noexcept;
    };

public:
    // DescriptorManager is required because glTF materials must reserve
    // bindless material slots while the scene is being loaded.
    // loadDefaultContent=false 时只创建相机实体，不加载 Sponza / 天空盒。
    void init(Device& device, VkCommandPool commandPool, DescriptorManager& descriptorManager,
              bool loadDefaultContent = true, JobSystem* jobSystem = nullptr);
    ~Scene();
    // allowCameraInput=false 时相机不接受键鼠输入（编辑器在鼠标位于面板上、
    // 正在输入文字、或正在拖 gizmo 时用）。仍然会把内部状态写回 transform。
    void update(float dt, GLFWwindow* window, float aspectRatio, bool allowCameraInput = true);
    void cleanup();

    entt::entity createEntity();
    void destroyEntity(entt::entity entity);

    // Reparenting keeps HierarchyComponent::children consistent and rejects
    // cycles. Passing entt::null makes the entity a root.
    void setParent(entt::entity child, entt::entity parent);

    entt::registry& getRegistry() { return registry; }
    const entt::registry& getRegistry() const { return registry; }
    const std::vector<entt::entity>& getVisibleEntities() const { return visibleEntities; }

    entt::entity getCameraEntity() const { return cameraEntity; }
    TransformComponent& getCameraTransform() { return registry.get<TransformComponent>(cameraEntity); }
    const TransformComponent& getCameraTransform() const { return registry.get<TransformComponent>(cameraEntity); }

    VulkanCamera& getCamera() { return camera; }
    CameraController& getCameraController() { return cameraController; }
    EngineSettings& getSettings() { return engineSettings; }
    RenderTelemetry& getTelemetry() { return currentTelemetry; }
    const RenderTelemetry& getDisplayTelemetry() const { return displayTelemetry; }
    uint32_t getLastWorldTransformUpdates() const { return lastWorldTransformUpdates; }
    uint32_t getLastSpatialIndexUpdates() const { return lastSpatialIndexUpdates; }
    size_t getRetiredModelCount() const { return retiredModels.size(); }

    // Dirty notification remains public for bulk/diagnostic paths. Normal
    // Transform/Bounds mutations notify Scene automatically through the
    // component owner callback installed by attachDirtyTracking().
    void markTransformDirty(entt::entity entity);
    void markSpatialDirty(entt::entity entity);
    void attachDirtyTracking(entt::entity entity);

    // Called once at the start of a render frame, before the UI is built.
    // The previous frame's counts are complete and safe to display while the
    // current frame is still being recorded.
    void beginFrameTelemetry()
    {
        displayTelemetry = currentTelemetry;
        currentTelemetry.reset();
    }

    MaterialSystem& getMaterialSystem() { return materialSystem; }

    // ---- 场景描述（编辑器 / 序列化用）----
    // captureDescription: 把当前 registry 导出成纯数据（不含任何 GPU 句柄）。
    // buildFromDescription: 清空当前场景并按描述重建（会重新加载网格、重新分配材质槽位）。
    // 两者都不抛异常，失败时通过 errorMessage 返回原因。
    scene::SceneDescription captureDescription() const;
    bool buildFromDescription(const scene::SceneDescription& description, std::string& errorMessage);

    // 清空场景（保留相机实体）。编辑器的"新建场景"走这里。
    void clearEntities();

    // 编辑器需要主动刷新世界变换 / 可见性（它改完组件后不能等下一帧）。
    void refreshTransformsAndVisibility();

    // D-7 regression test: hierarchy transform propagation + uniform-grid query.
    static bool selfTest();

private:
    // 编辑器加载场景时需要重新解析网格 / 材质，这两个指针由 init() 存下来。
    Device* device = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;

public:
    // 供编辑器按路径实例化一个网格实体（会自动分配材质槽位并设置包围盒）。

    // 返回 entt::null 表示失败。

    entt::entity createMeshEntity(const std::string& absolutePath, const std::string& name);



    // ---- Terrain (F2 / D-18) ----
    // The CPU heightfield/mesh is built first; GPU upload and ECS mutation only
    // happen after generation succeeds. spawnPosition is world-space.
    entt::entity createTerrainEntity(const terrain::TerrainConfig& config,
                                     const std::string& name,
                                     const glm::vec3& spawnPosition);

    // B-07: terrain sculpting. The brush is applied to every intersecting
    // terrain entity (streamed chunks are selected through the region index),
    // and only entities whose heightfield actually changed get a new Model.
    struct TerrainSculptResult
    {
        uint32_t modifiedVertices = 0;
        uint32_t rebuiltEntities = 0;
        std::vector<entt::entity> rebuilt;
    };
    bool sculptTerrain(const terrain::TerrainBrushSettings& brush,
                       TerrainSculptResult& outResult,
                       std::string& errorMessage);

    // B-07: lightweight kinematic character. The transform is the feet position.
    entt::entity createCharacterEntity(const std::string& name,
                                       const glm::vec3& spawnPosition);
    bool updateCharacterController(entt::entity entity,
                                   const glm::vec3& worldMoveDirection,
                                   bool jump,
                                   float dt,
                                   CharacterMoveResult& outResult);

    // B-01: camera-centred streamed terrain. The manager entity owns no mesh;
    // chunk entities are independent TerrainComponent + MeshComponent entities
    // generated asynchronously around the camera.
    // F2-h：流送半径硬上限。容量（GPU 选择槽位 / arena）跟不上时会自动退回逐 chunk
    // 路径，所以这里只限制「环的尺寸」，不再和槽位数绑定。
    static constexpr int32_t kMaxStreamingRadius = 24;
    entt::entity createStreamingTerrain(const terrain::TerrainConfig& chunkConfig,
                                        int32_t loadRadius,
                                        uint32_t maxLoadsPerFrame,
                                        const std::string& name);
    bool hasStreamingTerrain() const;
    entt::entity getStreamingTerrainEntity() const { return terrainStreamingEntity; }
    size_t getStreamingTerrainChunkCount() const { return streamingTerrainChunks.size(); }
    uint32_t getInFlightTerrainGpuRequests() const;
    entt::entity resolveTerrainChunkOwner(entt::entity entity) const;
    // B-06: query streamed terrain chunk entities through the coarse XZ region
    // index instead of scanning every entity. Entities with TerrainChunkComponent
    // are filtered out of the generic spatial result.
    void queryTerrainChunksInRegion(int32_t regionX, int32_t regionZ,
                                    std::vector<entt::entity>& outEntities) const;
    void queryTerrainChunksInRegionRange(int32_t minRegionX, int32_t minRegionZ,
                                         int32_t maxRegionX, int32_t maxRegionZ,
                                         std::vector<entt::entity>& outEntities) const;
    bool adoptStreamingTerrainManager(entt::entity entity);
    void setStreamingTerrainBudget(float milliseconds);
    double getLastStreamingUpdateMs() const { return lastStreamingUpdateMs; }
    double getMaxStreamingUpdateMs() const { return maxStreamingUpdateMs; }

    // F2-f：地形几何的共享 arena。Scene 持有生命周期，GeometrySystem 只拿裸指针绘制。
    TerrainMeshArena* getTerrainMeshArena() const { return terrainMeshArena.get(); }
    // F2-a：流送统计。loaded/unloaded 是累计值，*LastFrame 是上一帧的增量，
    // 用来在自检里断言"相机飞行时真的发生了加载与卸载"。
    uint64_t getStreamingChunksLoadedTotal() const { return streamingChunksLoadedTotal; }
    uint64_t getStreamingChunksUnloadedTotal() const { return streamingChunksUnloadedTotal; }
    uint32_t getStreamingLoadsLastFrame() const { return streamingLoadsLastFrame; }
    uint32_t getStreamingUnloadsLastFrame() const { return streamingUnloadsLastFrame; }
    // F2-d: frames where the per-frame build budget stopped issuing new
    // chunks. A steadily climbing number means the budget is the limiter.
    uint64_t getStreamingBudgetHits() const { return streamingBudgetHits; }
    // F2-c：浮动原点。worldOrigin 是当前局部原点在绝对世界里的位置（double 累计）；
    // 实体 Transform 一律存相对这个原点的坐标，所以渲染与剔除看到的数值始终很小。
    glm::dvec3 getWorldOrigin() const { return worldOrigin; }
    uint64_t getOriginRebaseCount() const { return originRebaseCount; }
    float getMaxLocalCameraDistance() const { return maxLocalCameraDistance; }
    // Test/tool entry point; Scene::update() calls this automatically after the
    // camera transform is advanced.
    void refreshStreamingTerrain();
    // F2-c：把整棵场景平移 -delta（delta 必须是 chunk 尺寸的整数倍），并累计 worldOrigin。
    void rebaseWorldOrigin(const glm::vec3& delta);
    bool maybeRebaseWorldOrigin();

    // Rebuild the entity's MeshComponent from its current config. On failure the
    // previous mesh and generatedConfig are preserved and lastError is set.
    // This synchronous entry point remains for scene loading, command tests and
    // diagnostic capture.
    bool regenerateTerrain(entt::entity entity, std::string& errorMessage);

    using TerrainGenerationCallback = std::function<void(bool, const std::string&)>;

    // Stage 7: submit only CPU heightfield/mesh generation to JobSystem.
    // Scene::update() performs the GPU upload on the main thread. A newer request
    // for the same entity supersedes older results. The callback always runs on
    // the main thread for the latest request; superseded jobs are discarded.
    bool requestRegenerateTerrain(entt::entity entity,
                                  TerrainGenerationCallback onComplete = {});
    void waitForTerrainGeneration();
    bool hasPendingTerrainGeneration() const
    {
        return !pendingTerrainBuilds.empty() || !retiringTerrainBuilds.empty();
    }

    // 自检用：任务计数（pending + 退役）暴露出来，才能断言"摘除后没有被立刻释放"。
    std::size_t getPendingTerrainBuildCount() const { return pendingTerrainBuilds.size(); }
    std::size_t getRetiringTerrainBuildCount() const { return retiringTerrainBuilds.size(); }

    // 自检用：临时禁用 GPU 高度场提交，强制走"提交即进 CPU worker"的路径。
    // 这条路径上 worker 用裸指针写任务字段，是退役队列要保护的对象。
    void setTerrainGpuHeightEnabled(bool enabled) { terrainGpuHeightEnabled = enabled; }

    // B-05: main-thread cost of terrain generation polling only. GPU heightfield
    // and mesh assembly happen asynchronously; Model adoption still occurs on
    // the update thread and is included here.
    double getMaxTerrainPollMs() const { return maxTerrainPollMs; }
    uint64_t getTerrainPollCount() const { return terrainPollCount; }
    void resetTerrainPollStats() { maxTerrainPollMs = 0.0; terrainPollCount = 0; }
    TerrainGpuMeshBuilder::Stats getTerrainGpuMeshStats() const;

    // Compare the GPU compute heightfield against the CPU reference. Used by
    // --terrain-test / editor self-tests; not part of the render hot path.
    bool selfTestTerrainCompute(std::string& errorMessage);

    // CPU collision queries. local coordinates are terrain-grid space before
    // the entity transform; world coordinates use the current WorldTransform.
    bool sampleTerrainHeightLocal(entt::entity entity, float localX, float localZ,
                                  float& outHeight) const;
    bool sampleTerrainHeightWorld(entt::entity entity, float worldX, float worldZ,
                                  float& outHeight) const;
    // B-07: efficient world-space ground query using the CPU heightfield and
    // entity transform, not per-triangle picking. Returns the highest terrain.
    bool sampleTerrainGroundHeightWorld(float worldX, float worldZ,
                                        float& outHeight,
                                        entt::entity* outEntity = nullptr) const;
    bool raycastTerrain(entt::entity entity, const glm::vec3& rayOrigin,
                        const glm::vec3& rayDirection, glm::vec3& outHitPoint,
                        float& outDistance) const;
    // B-07: nearest terrain hit across manual and streamed terrain entities.
    // Used by the editor sculpt brush; picking still uses Model::intersectRay.
    bool raycastTerrainWorld(const glm::vec3& rayOrigin,
                             const glm::vec3& rayDirection,
                             glm::vec3& outHitPoint,
                             entt::entity& outEntity,
                             float& outDistance) const;

    // Pure build helper exposed for editor commands and scene self-tests.
    // outHeightField is optional; when supplied it receives the same CPU grid
    // used to build the mesh.
    bool buildTerrainModel(const terrain::TerrainConfig& config,
                           std::shared_ptr<Model>& outModel,
                           uint32_t& outVertexCount,
                           uint32_t& outIndexCount,
                           double& outGenerationMs,
                           std::string& errorMessage,
                           std::shared_ptr<terrain::HeightField>* outHeightField = nullptr,
                           bool* outMeshGeneratedOnGpu = nullptr);

    // B-07: rebuild a Model from an already-modified heightfield. Used by
    // sculpting; does not regenerate procedural heights.
    bool buildTerrainModelFromHeightField(
        const terrain::TerrainConfig& config,
        const std::shared_ptr<terrain::HeightField>& heightField,
        std::shared_ptr<Model>& outModel,
        uint32_t& outVertexCount,
        uint32_t& outIndexCount,
        double& outGenerationMs,
        std::string& errorMessage,
        bool* outMeshGeneratedOnGpu = nullptr);

    // B-08: bounded terrain material-set residency. Sets are loaded on demand
    // and cached; set 0 is created eagerly in Scene::init().
    bool ensureTerrainMaterialSet(uint32_t materialSetIndex, std::string& errorMessage);
    bool ensureTerrainMaterialSetFromDirectory(uint32_t materialSetIndex,
                                               const std::string& directoryOverride,
                                               std::string& errorMessage);
    TerrainMaterial::Stats getTerrainMaterialStats() const;

private:
    struct PendingTerrainBuild;

    void processTerrainGeneration();
    // 从 pendingTerrainBuilds 摘除的任务不能立刻释放：JobSystem 的 worker
    // 可能还在写它的 mesh / heightField / error（提交时用裸指针捕获）。
    // 句柄未完成时先移入 retiringTerrainBuilds，等 isComplete 后再回收。
    void retireTerrainBuild(std::shared_ptr<PendingTerrainBuild> task);
    void drainRetiringTerrainBuilds();
    bool queryGroundAtWorld(float worldX, float worldZ, float& outHeight,
                            entt::entity& outEntity, float* outSlopeDegrees = nullptr) const;
    void recordTerrainPollMs(double ms);
    void updateTerrainStreaming();
    void discardPendingTerrainCallbacks();
    void cancelPendingTerrainGenerations();
    struct RetiredModel
    {
        std::shared_ptr<Model> model;
        uint64_t releaseFrame = 0;
    };

    void markAllTransformsDirty();
    void markAllSpatialDirty();
    void updateWorldTransforms();
    void refreshSpatialIndex();
    void retireModel(const std::shared_ptr<Model>& model);
    void collectRetiredModels();
    void updateVisibility(const glm::mat4& viewProjection);

    entt::registry registry;
    entt::entity cameraEntity = entt::null;
    SpatialIndex spatialIndex{ 16.0f };
    std::unordered_map<entt::entity, AABB> indexedBounds;
    std::unordered_set<entt::entity> dirtyTransformEntities;
    std::unordered_set<entt::entity> dirtySpatialEntities;
    std::vector<entt::entity> visibleEntities;
    uint32_t lastWorldTransformUpdates = 0;
    uint32_t lastSpatialIndexUpdates = 0;
    uint64_t frameCounter = 0;
    std::vector<RetiredModel> retiredModels;

    VulkanCamera camera{};
    CameraController cameraController{};

    EngineSettings engineSettings;
    RenderTelemetry currentTelemetry;
    RenderTelemetry displayTelemetry;
    MaterialSystem materialSystem;
    std::unique_ptr<TerrainMaterial> terrainMaterial;
    std::unique_ptr<TerrainComputeGenerator> terrainComputeGenerator;
    std::unique_ptr<TerrainGpuMeshBuilder> terrainGpuMeshBuilder;

    JobSystem* jobSystem = nullptr;
    bool terrainGpuHeightEnabled = true;
    std::vector<std::shared_ptr<PendingTerrainBuild>> pendingTerrainBuilds;
    // 已摘除但仍可能有 CPU worker 写入的任务；见 retireTerrainBuild()。
    std::vector<std::shared_ptr<PendingTerrainBuild>> retiringTerrainBuilds;
    std::unordered_map<entt::entity, uint64_t> latestTerrainRequest;
    uint64_t nextTerrainRequestSerial = 0;

    entt::entity terrainStreamingEntity = entt::null;
    // F2-h：chunk 坐标用 int64（半径再大也不会溢出），键直接是坐标对而不是打包成 int64。
    std::unordered_map<TerrainChunkCoord, entt::entity, TerrainChunkCoordHash>
        streamingTerrainChunks;
    std::unordered_set<TerrainChunkCoord, TerrainChunkCoordHash> pendingStreamingTerrainChunks;
    double lastStreamingUpdateMs = 0.0;
    uint64_t streamingChunksLoadedTotal = 0;
    uint64_t streamingChunksUnloadedTotal = 0;
    uint32_t streamingLoadsLastFrame = 0;
    uint32_t streamingUnloadsLastFrame = 0;
    uint64_t streamingBudgetHits = 0;
    double maxStreamingUpdateMs = 0.0;

    // F2-f：地形 chunk 的顶点/索引统一放这里，Model 只保留元数据。
    std::unique_ptr<TerrainMeshArena> terrainMeshArena;
    // F2-c：浮动原点状态。delta 一律吸到整块 chunk，实体与流送 key 才能整体平移。
    glm::dvec3 worldOrigin{ 0.0, 0.0, 0.0 };
    uint64_t originRebaseCount = 0;
    float maxLocalCameraDistance = 0.0f;
    double maxTerrainPollMs = 0.0;
    uint64_t terrainPollCount = 0;
};