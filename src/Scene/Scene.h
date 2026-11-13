#pragma once

#include "Scene/Components.h"
#include "Scene/ComponentRegistry.h"
#include "Scene/SceneDescription.h"
#include "Scene/SystemScheduler.h"
#include "Scene/Camera.h"
#include "Scene/CameraController.h"
#include "Scene/SpatialIndex.h"
#include "Core/MaterialSystem.h"
#include "Core/DescriptorManager.h"
#include "Core/JobSystem.h"
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
#include <optional>
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
    // 构造函数只注册内置组件/系统，不碰 GPU；init() 才建立 Vulkan 相关资源。
    Scene();
    void init(Device& device, VkCommandPool commandPool, DescriptorManager& descriptorManager,
              bool loadDefaultContent = true, JobSystem* jobSystem = nullptr);
    ~Scene();
    // allowCameraInput=false 时相机不接受键鼠输入（编辑器在鼠标位于面板上、
    // 正在输入文字、或正在拖 gizmo 时用）。仍然会把内部状态写回 transform。
    void update(float dt, GLFWwindow* window, float aspectRatio, bool allowCameraInput = true);

    // M1-C1b：编辑器把模拟切成固定 tick 后，Scene::update 不再自己跑系统相，
    // 改由 stepSimulation() 在 tick 回调里推进（默认 false，回归路径不变）。
    void setSimulationDrivenExternally(bool driven) { simulationDrivenExternally_ = driven; }
    bool simulationDrivenExternally() const { return simulationDrivenExternally_; }
    // 只推进一次模拟：SystemScheduler 两相 + 世界矩阵刷新；不碰相机、流送、可见性。
    void stepSimulation(float dt);

    // M1-C1c：渲染插值权重。编辑器播放态写入 SimulationClock::alpha()，
    // Application 每帧把它搬进 FrameInfo::renderAlpha；默认 0 = 不插值。
    void setRenderInterpolationAlpha(float alpha) { renderInterpolationAlpha_ = alpha; }
    float renderInterpolationAlpha() const { return renderInterpolationAlpha_; }
    void cleanup();

    entt::entity createEntity();
    void destroyEntity(entt::entity entity);

    // Reparenting keeps HierarchyComponent::children consistent and rejects
    // cycles. Passing entt::null makes the entity a root.
    void setParent(entt::entity child, entt::entity parent);

    entt::registry& getRegistry() { return registry; }
    const entt::registry& getRegistry() const { return registry; }
    const std::vector<entt::entity>& getVisibleEntities() const { return visibleEntities; }

    // ---- 通用 ECS 组件（元数据驱动）----
    // ComponentRegistry 只保存“类型/属性/反射回调”，不持有任何实体数据。
    // 因此编辑器可以遍历它画出 Add Component 与通用属性面板，序列化也能
    // 只依赖 ComponentRecord 这一层纯数据。
    scene::ComponentRegistry& getComponentRegistry() { return componentRegistry; }
    const scene::ComponentRegistry& getComponentRegistry() const { return componentRegistry; }
    scene::SystemScheduler& getSystemScheduler() { return systemScheduler; }
    const scene::SystemScheduler& getSystemScheduler() const { return systemScheduler; }

    // ---- 场景级组件（阴影 / Bloom / 雾 / 水面…）----
    // 它们同样走 ComponentRegistry 与 ComponentRecord，只是 scope = Scene，
    // 并且只能挂在 Scene 自建的隐藏实体上。UI 因此可以复用同一套组件控件。
    entt::entity getSceneSettingsEntity() const { return sceneSettingsEntity; }
    bool addSceneComponent(const std::string& type);
    bool removeSceneComponent(const std::string& type);
    bool hasSceneComponent(const std::string& type) const;
    std::vector<scene::ComponentRecord> captureSceneComponents() const;
    // 用当前 EngineSettings 重建整套场景组件（旧场景 / 引擎默认 / 新建场景）。
    void resetSceneComponentsFromSettings();
    // 移除全部场景组件：等价于关闭所有可选渲染功能。
    void clearSceneComponents();
    // 组件 -> EngineSettings 的同步。组件值变化、加载、撤销后都必须调用。
    void syncSceneSettings();

    bool addComponent(entt::entity entity, const std::string& type);
    // Add 菜单 / 粘贴前的兼容性检查。false 时 reason 给出可显示的中文原因。
    bool canAddComponent(entt::entity entity, const std::string& type) const;
    std::string componentAddBlockReason(entt::entity entity, const std::string& type) const;
    // 地形 / 流送地形 / 体素世界等由专用面板管理的对象。
    bool isTerrainManagedEntity(entt::entity entity) const;
    bool removeComponent(entt::entity entity, const std::string& type);
    bool hasComponent(entt::entity entity, const std::string& type) const;
    std::optional<scene::ComponentRecord> readComponent(entt::entity entity,
                                                       const std::string& type) const;
    std::optional<scene::ComponentRecord> defaultComponentRecord(const std::string& type) const;
    bool applyComponentRecord(entt::entity entity, const scene::ComponentRecord& record);
    std::vector<scene::ComponentRecord> captureComponentRecords(entt::entity entity) const;

    entt::entity getCameraEntity() const { return cameraEntity; }
    TransformComponent& getCameraTransform() { return registry.get<TransformComponent>(cameraEntity); }
    const TransformComponent& getCameraTransform() const { return registry.get<TransformComponent>(cameraEntity); }

    VulkanCamera& getCamera() { return camera; }
    CameraController& getCameraController() { return cameraController; }
    EngineSettings& getSettings() { return engineSettings; }
    RenderTelemetry& getTelemetry() { return currentTelemetry; }
    const RenderTelemetry& getDisplayTelemetry() const { return displayTelemetry; }
    uint32_t getLastWorldTransformUpdates() const { return lastWorldTransformUpdates; }

    // F2 收尾：每帧 CPU 分段时间。默认关闭（零开销），--update-profile 打开后
    // 每 60 帧打印一次各阶段的平均耗时与窗口内最大总耗时。
    void setUpdateProfiling(bool enabled) { updateProfiling = enabled; }
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
    // E3：材质 factor 的 GPU 侧写入（编辑器改参数后立刻生效）。
    DescriptorManager* getDescriptorManager() const { return descriptorManager; }
    // E6：编辑器选材质贴图时用。编辑器不持有 Device / commandPool，所以贴图的
    // 创建与缓存（MaterialSystem 内部按路径缓存）留在这里；srgb 由调用方按贴图
    // 语义决定：颜色类为 true，法线/粗糙度/AO 这类数据贴图必须为 false。
    std::shared_ptr<Texture> loadMaterialTexture(const std::string& filepath, bool srgb);

    // ---- 场景描述（编辑器 / 序列化用）----
    // captureDescription: 把当前 registry 导出成纯数据（不含任何 GPU 句柄）。
    // 无参版本导出整个场景；传 root 时只导出该实体的子树（用于预制体）。
    // buildFromDescription: 清空当前场景并按描述重建（会重新加载网格、重新分配材质槽位）。
    // instantiatePrefab: 不清场景，把描述里的单根子树追加到 parent 下。
    // 这些接口都不抛异常，失败时通过 errorMessage 返回原因。
    scene::SceneDescription captureDescription() const;
    scene::SceneDescription captureDescription(entt::entity root) const;
    bool buildFromDescription(const scene::SceneDescription& description, std::string& errorMessage);
    entt::entity instantiatePrefab(const scene::SceneDescription& description,
                                   entt::entity parent,
                                   std::vector<entt::entity>& createdEntities,
                                   std::string& errorMessage);

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

    // ---- Voxel world (F3) ----
    // 一个管理器实体 + 每块一个 32^3 chunk 实体。方块坐标是世界整数格，
    // 第 (x,y,z) 格占 [x,x+1] x [y,y+1] x [z,z+1]。
    // F3-g：半径上限（异步构建下可到 12，即 625 块）。
    static constexpr int32_t kMaxVoxelRadius = 12;

    entt::entity createVoxelWorld(uint64_t seed, int32_t radiusChunks, int32_t baseY);
    // 改一格并重建受影响的 chunk（跨边界时两/四块都重建）。返回是否真的改了。
    bool setVoxelBlock(entt::entity world, int32_t x, int32_t y, int32_t z, uint8_t blockId);
    uint8_t getVoxelBlock(entt::entity world, int32_t x, int32_t y, int32_t z) const;
    // V-01 目测用：在世界原点附近摆一组十字交叉玻璃 + 一片近景玻璃，
    // 一次性写入所有格子后只重建受影响的 chunk。返回写入的格子数。
    int32_t placeVoxelGlassDemo(entt::entity world);
    // DDA 体素射线：返回命中格坐标与命中面法线（用于"放在哪一面"）。
    bool raycastVoxelWorld(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                           float maxDistance, glm::ivec3& outBlock,
                           glm::ivec3& outNormal, float& outDistance,
                           entt::entity* outChunk = nullptr,
                           entt::entity worldFilter = entt::null) const;
    // 体素地面高度（该 XZ 列最高实心方块的上表面），没有则返回 false。
    bool sampleVoxelGroundHeight(int32_t x, int32_t z, int32_t& outY,
                                 entt::entity* outChunk = nullptr,
                                 entt::entity worldFilter = entt::null) const;

    // F3-g：等所有体素 chunk 的异步构建完成（诊断/自检/反序列化用）。
    void waitForVoxelBuilds();
    // 纯逻辑 + 真实场景的自检：建 3x3 chunk、射线打地、挖一格、放回去。
    bool voxelSelfTest();



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
                                   CharacterMoveResult& outResult,
                                   float speedMultiplier = 1.0f);

    // 运行时角色：把带蒙皮/动画的 glTF 挂到已有角色实体上。模型绑定姿势的
    // 脚底在原点，uniformScale 用来把 3.55m 的 Quaternius 绑定高度压到约 1.78m。
    bool attachCharacterModel(entt::entity entity,
                              const std::string& glbPath,
                              float uniformScale = 1.0f);

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
    entt::entity resolveVoxelChunkOwner(entt::entity entity) const;
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

    // tech-debt #19：异步创建。实体当帧就出现（TerrainComponent::generating = true），
    // 高度场与网格交给 requestRegenerateTerrain 的后台作业，完成时补 MeshComponent、
    // 包围盒与空间索引。返回 entt::null 表示建实体失败（配置非法/材质槽位申请失败）。
    entt::entity createTerrainEntityAsync(const terrain::TerrainConfig& config,
                                          const std::string& name,
                                          const glm::vec3& spawnPosition,
                                          TerrainGenerationCallback onComplete = {});
    bool supportsAsyncTerrainBuild() const { return jobSystem != nullptr; }

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
    // F6-07：植被只长在地形上，不爬到体素方块或建筑上。只接受命中的
    // TerrainComponent；体素地面即使更高也会返回 false。
    bool sampleTerrainOnlyGroundHeightWorld(float worldX, float worldZ,
                                            float& outHeight) const;
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
    void reportUpdateProfile(double genPollUs, double streamingUs, double worldUs,
                             double spatialUs, double visibilityUs);
    void updateTerrainStreaming();
    // F3：重建一个体素 chunk 的网格与 Model。编辑一格时只重建它自己和边界邻居。
    bool applyVoxelChunkMesh(entt::entity entity, const terrain::TerrainMeshData& mesh,
        const voxel::MeshStats& stats, const std::shared_ptr<terrain::TerrainMeshData>& glassMesh,
        const voxel::GlassStats& glassStats, const std::shared_ptr<Material>& material);
    bool rebuildVoxelChunk(entt::entity chunkEntity);

    // F3-g：异步构建中的体素 chunk（生成 + 网格化在 JobSystem 里完成）。
    struct PendingVoxelChunk
    {
        entt::entity entity = entt::null;
        std::shared_ptr<voxel::Chunk> chunk;
        int32_t chunkX = 0;
        int32_t chunkZ = 0;
        int32_t baseY = 0;
        std::shared_ptr<terrain::TerrainMeshData> mesh;
        voxel::MeshStats stats;
        // V-01：同一作业里顺带生成玻璃网格，主线程取回时一起落地。
        std::shared_ptr<terrain::TerrainMeshData> glassMesh;
        voxel::GlassStats glassStats;
        // clearEntities() 只能打标记，不能提前销毁持有裸指针的作业状态。
        bool cancelled = false;
        JobSystem::JobHandle handle;
    };
    void processVoxelChunkBuilds();
    std::vector<std::shared_ptr<PendingVoxelChunk>> pendingVoxelBuilds;
    entt::entity findVoxelChunk(entt::entity world, int32_t chunkX, int32_t chunkZ) const;
    void discardPendingTerrainCallbacks();
    void cancelPendingTerrainGenerations();
    struct RetiredModel
    {
        std::shared_ptr<Model> model;
        uint64_t releaseFrame = 0;
    };

    // 场景载入与预制体实例化共用：按描述建 Mesh/RenderTag/Bounds/Material。
    // 地形实体由 buildFromDescription 自己生成网格，这里对 e.isTerrain 直接跳过。
    bool applyMeshAndMaterialFromDescription(const scene::EntityDescription& e,
                                             entt::entity entity,
                                             std::string& errorMessage);
    // 通用 add/read/apply 的 target 校验：Scene scope 只允许隐藏设置实体，
    // Entity scope 则不允许挂到设置实体上。
    bool acceptsComponentTarget(entt::entity entity,
                                const scene::ComponentDescriptor& descriptor) const;
    void markAllTransformsDirty();
    void markAllSpatialDirty();
    void updateWorldTransforms();
    void refreshSpatialIndex();
    void retireModel(const std::shared_ptr<Model>& model);
    void collectRetiredModels();
    void updateVisibility(const glm::mat4& viewProjection);

    entt::registry registry;
    scene::ComponentRegistry componentRegistry;
    scene::SystemScheduler systemScheduler;
    entt::entity cameraEntity = entt::null;
    // 场景级组件的宿主：没有 Transform/Hierarchy/Name，因此不会出现在
    // Hierarchy、可见实体列表或普通场景实体遍历里；clearEntities() 也会保留它。
    entt::entity sceneSettingsEntity = entt::null;
    SpatialIndex spatialIndex{ 16.0f };
    std::unordered_map<entt::entity, AABB> indexedBounds;
    std::unordered_set<entt::entity> dirtyTransformEntities;
    std::unordered_set<entt::entity> dirtySpatialEntities;
    std::vector<entt::entity> visibleEntities;
    uint32_t lastWorldTransformUpdates = 0;

    // F2 收尾：流送扫描的稳态早退。三者与环内块数都没变时不需要重扫。
    int64_t lastStreamingCenterX = INT64_MIN;
    int64_t lastStreamingCenterZ = INT64_MIN;
    int32_t lastStreamingRadius = -1;

    bool simulationDrivenExternally_ = false;

    // M1-C1c：见 setRenderInterpolationAlpha()。非播放态保持 0。
    float renderInterpolationAlpha_ = 0.0f;

    // --update-profile：只累加 double，不做字符串与分配。
    bool updateProfiling = false;
    uint32_t profileFrames = 0;
    double profileGenPollUs = 0.0;
    double profileStreamingUs = 0.0;
    double profileWorldUs = 0.0;
    double profileSpatialUs = 0.0;
    double profileVisibilityUs = 0.0;
    double profileMaxTotalUs = 0.0;
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