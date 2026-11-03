#include "Scene/Scene.h"
#include "AssetPaths.h"
#include "RHI/Texture.h"
#include "RHI/CommandUtils.h"
#include "Scene/GLTFLoader.h"
#include "Render/TerrainMaterial.h"
#include "Render/TerrainComputeGenerator.h"
#include "Render/SubMeshCulling.h"
#include "Terrain/TerrainHeightField.h"
#include "Terrain/TerrainMeshBuilder.h"
#include "Terrain/TerrainBrush.h"
#include <glm/gtc/matrix_inverse.hpp>
#include "Terrain/TerrainSelfTest.h"
#include "Core/JobSystem.h"
#include <unordered_map>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>


struct Scene::PendingTerrainBuild
{
    entt::entity entity = entt::null;
    uint64_t requestId = 0;
    terrain::TerrainConfig config;
    terrain::TerrainMeshData mesh;
    std::shared_ptr<terrain::HeightField> heightField;
    TerrainGpuMeshBuildResult gpuMesh;
    bool gpuMeshReady = false;
    bool gpuMeshAttempted = false;
    JobSystem::JobHandle handle;
    Scene::TerrainGenerationCallback completion;
    std::string error;
    uint64_t gpuHeightRequest = 0;
    uint64_t gpuMeshRequest = 0;
    bool cpuHeightFallback = false;
    bool meshSubmitted = false;
    bool failed = false;
    double heightGenerationMs = 0.0;
    double cpuGenerationMs = 0.0;
    bool cpuSucceeded = false;
    std::chrono::steady_clock::time_point heightStartTime;
    std::atomic<bool> cancelRequested{ false };
};

namespace
{
    constexpr bool kLoadSponza = true;
    constexpr int32_t kMaxTerrainStreamingRadius = 12;
    constexpr uint32_t kMaxTerrainStreamingLoadsPerFrame = 16;

    int64_t terrainChunkKey(int32_t x, int32_t z)
    {
        return (static_cast<int64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

    void notifyTransformDirty(void* context, entt::entity entity)
    {
        if (context)
            static_cast<Scene*>(context)->markTransformDirty(entity);
    }

    void notifySpatialDirty(void* context, entt::entity entity)
    {
        if (context)
            static_cast<Scene*>(context)->markSpatialDirty(entity);
    }

    bool buildTerrainMeshFromHeightField(const terrain::TerrainConfig& config,
                                         const terrain::HeightField& heightField,
                                         terrain::TerrainMeshData& outMesh,
                                         std::string& errorMessage,
                                         const std::atomic<bool>* cancelFlag = nullptr)
    {
        outMesh = {};
        errorMessage.clear();
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            errorMessage = "terrain generation cancelled";
            return false;
        }
        return terrain::TerrainMeshBuilder::build(
            config, heightField, outMesh, errorMessage, cancelFlag);
    }

    bool generateTerrainHeightField(const terrain::TerrainConfig& config,
                                    std::shared_ptr<terrain::HeightField>& outHeightField,
                                    double& outGenerationMs,
                                    std::string& errorMessage,
                                    const std::atomic<bool>* cancelFlag,
                                    TerrainComputeGenerator* gpuGenerator)
    {
        outHeightField.reset();
        outGenerationMs = 0.0;
        errorMessage.clear();
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            errorMessage = "terrain generation cancelled";
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        auto heightField = std::make_shared<terrain::HeightField>();
        bool generated = false;
        if (gpuGenerator && gpuGenerator->isAvailable())
        {
            generated = gpuGenerator->generate(config, *heightField, errorMessage);
            if (!generated)
            {
                static bool reportedGpuFallback = false;
                if (!reportedGpuFallback)
                {
                    reportedGpuFallback = true;
                    std::cout << "[Terrain] GPU height generation failed; falling back to CPU | "
                              << errorMessage << "\n";
                }
            }
        }
        if (!generated &&
            !heightField->build(config, errorMessage, cancelFlag))
            return false;

        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            errorMessage = "terrain generation cancelled";
            return false;
        }

        outHeightField = std::move(heightField);
        const auto end = std::chrono::steady_clock::now();
        outGenerationMs = std::chrono::duration<double, std::milli>(end - start).count();
        return true;
    }

    bool generateTerrainMeshCpu(const terrain::TerrainConfig& config,
                                terrain::TerrainMeshData& outMesh,
                                double& outGenerationMs,
                                std::string& errorMessage,
                                const std::atomic<bool>* cancelFlag,
                                std::shared_ptr<terrain::HeightField>* outHeightField,
                                TerrainComputeGenerator* gpuGenerator)
    {
        outMesh = {};
        outGenerationMs = 0.0;
        errorMessage.clear();
        if (outHeightField)
            outHeightField->reset();

        std::shared_ptr<terrain::HeightField> heightField;
        double heightMs = 0.0;
        if (!generateTerrainHeightField(config, heightField, heightMs, errorMessage,
                                        cancelFlag, gpuGenerator))
            return false;

        const auto meshStart = std::chrono::steady_clock::now();
        if (!buildTerrainMeshFromHeightField(config, *heightField, outMesh,
                                             errorMessage, cancelFlag))
            return false;
        const auto meshEnd = std::chrono::steady_clock::now();

        outGenerationMs = heightMs +
            std::chrono::duration<double, std::milli>(meshEnd - meshStart).count();
        if (outHeightField)
            *outHeightField = std::move(heightField);
        return true;
    }

    bool uploadTerrainMesh(Device& device, VkCommandPool commandPool,
                           const terrain::TerrainMeshData& terrainMesh,
                           std::shared_ptr<Model>& outModel,
                           uint32_t& outVertexCount,
                           uint32_t& outIndexCount,
                           double& outUploadMs,
                           std::string& errorMessage)
    {
        outModel.reset();
        outVertexCount = 0;
        outIndexCount = 0;
        outUploadMs = 0.0;
        errorMessage.clear();

        const auto start = std::chrono::steady_clock::now();
        try
        {
            Model::Builder builder;
            builder.vertices.reserve(terrainMesh.vertices.size());
            for (const terrain::TerrainVertex& vertex : terrainMesh.vertices)
            {
                builder.vertices.push_back(Vertex{
                    vertex.position,
                    vertex.color,
                    vertex.normal,
                    vertex.uv
                });
            }
            builder.indices = terrainMesh.indices;
            builder.subMeshes.reserve(terrainMesh.subMeshes.size());
            for (const terrain::TerrainSubMesh& chunk : terrainMesh.subMeshes)
            {
                SubMesh subMesh;
                subMesh.firstIndex = chunk.firstIndex;
                subMesh.indexCount = chunk.indexCount;
                subMesh.hasBounds = true;
                subMesh.boundsMin = chunk.boundsMin;
                subMesh.boundsMax = chunk.boundsMax;
                subMesh.isTerrainChunk = true;
                subMesh.terrainChunkX = chunk.chunkX;
                subMesh.terrainChunkZ = chunk.chunkZ;
                subMesh.terrainLod = chunk.lod;
                subMesh.terrainSurfaceIndexCount = chunk.surfaceIndexCount;
                builder.subMeshes.push_back(subMesh);
            }

            outModel = std::make_shared<Model>(device, commandPool, builder);
            outVertexCount = static_cast<uint32_t>(builder.vertices.size());
            outIndexCount = static_cast<uint32_t>(builder.indices.size());
        }
        catch (const std::exception& ex)
        {
            outModel.reset();
            errorMessage = std::string("GPU terrain upload failed: ") + ex.what();
            return false;
        }
        catch (...)
        {
            outModel.reset();
            errorMessage = "GPU terrain upload failed with unknown exception";
            return false;
        }

        const auto end = std::chrono::steady_clock::now();
        outUploadMs = std::chrono::duration<double, std::milli>(end - start).count();
        return true;
    }

    entt::entity createTransformEntity(entt::registry& registry)
    {
        const entt::entity entity = registry.create();
        registry.emplace<TransformComponent>(entity);
        registry.emplace<HierarchyComponent>(entity);
        return entity;
    }

    void setModelBounds(entt::registry& registry, entt::entity entity,
                        const std::shared_ptr<Model>& model,
                        std::unordered_set<entt::entity>* dirtySpatial)
    {
        if (!model)
            return;

        BoundsComponent& bounds = registry.get_or_emplace<BoundsComponent>(entity);
        AABB localBounds = bounds.getLocal();
        localBounds.min = model->getBoundsMin();
        localBounds.max = model->getBoundsMax();
        localBounds.valid = true;
        bounds.setLocal(localBounds);
        if (auto* worldBounds = registry.try_get<WorldBoundsComponent>(entity))
            worldBounds->dirty = true;
        if (dirtySpatial)
            dirtySpatial->insert(entity);
    }

    void updateNodeRecursive(entt::registry& registry, entt::entity entity,
                             const glm::mat4& parentWorld, bool parentChanged,
                             uint32_t& updatedCount,
                             std::unordered_set<entt::entity>* dirtyTransforms,
                             std::unordered_set<entt::entity>* dirtySpatial)
    {
        TransformComponent& transform = registry.get<TransformComponent>(entity);
        WorldTransformComponent& worldTransform =
            registry.get_or_emplace<WorldTransformComponent>(entity);

        const bool needsUpdate = parentChanged || transform.isDirty() ||
            !worldTransform.valid ||
            worldTransform.sourceRevision != transform.getLocalRevision();

        glm::mat4 world = worldTransform.matrix;
        if (needsUpdate)
        {
            world = parentWorld * transform.localMatrix();
            worldTransform.matrix = world;
            worldTransform.normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
            worldTransform.worldPosition = glm::vec3(world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            worldTransform.sourceRevision = transform.getLocalRevision();
            worldTransform.valid = true;
            transform.clearDirty();

            if (auto* worldBounds = registry.try_get<WorldBoundsComponent>(entity))
                worldBounds->dirty = true;
            if (dirtySpatial &&
                (registry.any_of<BoundsComponent>(entity) ||
                 registry.any_of<MeshComponent>(entity)))
                dirtySpatial->insert(entity);
            if (dirtyTransforms)
                dirtyTransforms->erase(entity);
            ++updatedCount;
        }

        if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
        {
            for (entt::entity child : hierarchy->children)
            {
                if (registry.valid(child))
                {
                    updateNodeRecursive(registry, child, world, needsUpdate,
                                        updatedCount, dirtyTransforms, dirtySpatial);
                }
            }
        }
    }

    uint32_t updateAllWorldTransforms(
        entt::registry& registry,
        std::unordered_set<entt::entity>* dirtyTransforms = nullptr,
        std::unordered_set<entt::entity>* dirtySpatial = nullptr)
    {
        uint32_t updatedCount = 0;

        if (dirtyTransforms)
        {
            if (dirtyTransforms->empty())
                return 0;

            std::unordered_set<entt::entity> roots;
            for (entt::entity entity : *dirtyTransforms)
            {
                if (!registry.valid(entity) ||
                    !registry.any_of<TransformComponent>(entity))
                    continue;

                entt::entity root = entity;
                const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
                entt::entity parent = hierarchy ? hierarchy->parent : entt::null;
                while (parent != entt::null && registry.valid(parent) &&
                       registry.any_of<TransformComponent>(parent))
                {
                    const auto* parentWorld =
                        registry.try_get<WorldTransformComponent>(parent);
                    if (dirtyTransforms->find(parent) != dirtyTransforms->end() ||
                        parentWorld == nullptr || !parentWorld->valid)
                    {
                        root = parent;
                    }
                    const auto* parentHierarchy =
                        registry.try_get<HierarchyComponent>(parent);
                    parent = parentHierarchy ? parentHierarchy->parent : entt::null;
                }
                roots.insert(root);
            }

            for (entt::entity root : roots)
            {
                glm::mat4 parentWorld{ 1.0f };
                const auto* hierarchy = registry.try_get<HierarchyComponent>(root);
                if (hierarchy && hierarchy->parent != entt::null &&
                    registry.valid(hierarchy->parent))
                {
                    const auto* parentWorldComp =
                        registry.try_get<WorldTransformComponent>(hierarchy->parent);
                    if (parentWorldComp && parentWorldComp->valid)
                        parentWorld = parentWorldComp->matrix;
                }
                updateNodeRecursive(registry, root, parentWorld, false,
                                    updatedCount, dirtyTransforms, dirtySpatial);
            }

            for (auto it = dirtyTransforms->begin(); it != dirtyTransforms->end();)
            {
                if (!registry.valid(*it) ||
                    !registry.any_of<TransformComponent>(*it))
                    it = dirtyTransforms->erase(it);
                else
                    ++it;
            }
            return updatedCount;
        }

        for (entt::entity entity : registry.view<TransformComponent>())
        {
            const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
            const bool isRoot = !hierarchy || hierarchy->parent == entt::null ||
                                !registry.valid(hierarchy->parent);
            if (isRoot)
            {
                updateNodeRecursive(registry, entity, glm::mat4{ 1.0f }, false,
                                    updatedCount, nullptr, nullptr);
            }
        }
        return updatedCount;
    }

    struct Plane
    {
        glm::vec3 normal{ 0.0f };
        float distance = 0.0f;
    };

    struct Frustum
    {
        Plane planes[6];
    };

    Plane makePlane(const glm::vec4& equation)
    {
        Plane plane;
        plane.normal = glm::vec3(equation);
        plane.distance = equation.w;
        const float length = glm::length(plane.normal);
        if (length > 0.00001f)
        {
            plane.normal /= length;
            plane.distance /= length;
        }
        return plane;
    }

    Frustum extractFrustum(const glm::mat4& viewProjection)
    {
        const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
        const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
        const glm::vec4 row2(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
        const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);

        Frustum frustum;
        // Vulkan clip space: -w <= x,y <= w and 0 <= z <= w.
        frustum.planes[0] = makePlane(row3 + row0); // left
        frustum.planes[1] = makePlane(row3 - row0); // right
        frustum.planes[2] = makePlane(row3 + row1); // bottom
        frustum.planes[3] = makePlane(row3 - row1); // top
        frustum.planes[4] = makePlane(row2);        // near
        frustum.planes[5] = makePlane(row3 - row2); // far
        return frustum;
    }

    bool intersectsAABB(const Frustum& frustum, const AABB& bounds)
    {
        if (!bounds.valid)
            return false;

        for (const Plane& plane : frustum.planes)
        {
            const glm::vec3 positive{
                plane.normal.x >= 0.0f ? bounds.max.x : bounds.min.x,
                plane.normal.y >= 0.0f ? bounds.max.y : bounds.min.y,
                plane.normal.z >= 0.0f ? bounds.max.z : bounds.min.z,
            };

            if (glm::dot(plane.normal, positive) + plane.distance < 0.0f)
                return false;
        }
        return true;
    }

    AABB frustumBroadBounds(const glm::mat4& viewProjection)
    {
        const glm::mat4 inverse = glm::inverse(viewProjection);
        AABB bounds;
        bounds.valid = true;

        bool first = true;
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = -1; y <= 1; y += 2)
            {
                for (int x = -1; x <= 1; x += 2)
                {
                    glm::vec4 p = inverse * glm::vec4(static_cast<float>(x), static_cast<float>(y),
                                                      static_cast<float>(z), 1.0f);
                    p /= p.w;
                    const glm::vec3 world(p);
                    if (first)
                    {
                        bounds.min = bounds.max = world;
                        first = false;
                    }
                    else
                    {
                        bounds.min = glm::min(bounds.min, world);
                        bounds.max = glm::max(bounds.max, world);
                    }
                }
            }
        }
        return bounds;
    }

    bool isDescendant(entt::registry& registry, entt::entity candidate, entt::entity ancestor)
    {
        entt::entity current = candidate;
        while (current != entt::null && registry.valid(current))
        {
            if (current == ancestor)
                return true;
            const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
            current = hierarchy ? hierarchy->parent : entt::null;
        }
        return false;
    }
}

Scene::~Scene() = default;

void Scene::init(Device& deviceArg, VkCommandPool commandPoolArg, DescriptorManager& descriptorManagerArg,
                 bool loadDefaultContent, JobSystem* jobSystemArg)
{
    // 编辑器 / 场景重建需要这些依赖（创建网格实体、分配材质槽位）。
    // 存指针而不是让每个调用点都传参，是因为 Scene 已经是"场景所有者"，
    // 它天然应该记得自己是用哪个设备创建出来的。
    device = &deviceArg;
    commandPool = commandPoolArg;
    // F2-f：地形几何的共享 arena 由 Scene 持有，生成完成时直接拷进来。
    terrainMeshArena = std::make_unique<TerrainMeshArena>(deviceArg);
    descriptorManager = &descriptorManagerArg;
    jobSystem = jobSystemArg;

    Device& device = deviceArg;
    VkCommandPool commandPool = commandPoolArg;
    DescriptorManager& descriptorManager = descriptorManagerArg;

    const std::string basePath = TINY_RESOURCE_DIR;

    materialSystem.initDefaultTextures(device, commandPool);
    terrainMaterial = std::make_unique<TerrainMaterial>(
        device, commandPool, descriptorManager, basePath + "Texture/Terrain/");

    try
    {
        terrainComputeGenerator = std::make_unique<TerrainComputeGenerator>(
            device, descriptorManager);
    }
    catch (const std::exception& ex)
    {
        terrainComputeGenerator.reset();
        std::cout << "[Terrain] GPU compute heightfield unavailable; using CPU fallback | "
                  << ex.what() << "\n";
    }

    try
    {
        terrainGpuMeshBuilder = std::make_unique<TerrainGpuMeshBuilder>(
            device, descriptorManager);
    }
    catch (const std::exception& ex)
    {
        terrainGpuMeshBuilder.reset();
        std::cout << "[Terrain] GPU mesh assembly unavailable; using CPU mesh fallback | "
                  << ex.what() << "\n";
    }

    cameraEntity = createTransformEntity(registry);
    registry.emplace<CameraComponent>(cameraEntity);
    // 编辑器层级面板需要一个可读名字。没有 NameComponent 时面板只能显示
    // "Entity 2" 这样的数字 ID，完全看不出哪个是相机、哪个是天空盒。
    registry.emplace<NameComponent>(cameraEntity, NameComponent{ "Camera" });
    attachDirtyTracking(cameraEntity);

    // loadDefaultContent=false 时跳过 Sponza / 兜底模型，只保留相机 + 天空盒。
    // 编辑器默认走这条路径：Sponza 整栋是一个实体、包围盒包住整个场景，
    // 会干扰"点选 / 包围盒 / 手柄"这类交互的观察。
    //
    // 天空盒仍然创建 —— 纯黑背景会让视口看起来像"没渲染出来"。
    std::shared_ptr<Model> heroModel;
    if (loadDefaultContent && kLoadSponza)
    {
        tiny::GLTFLoadContext ctx;
        ctx.device = &device;
        ctx.commandPool = commandPool;
        ctx.materialSystem = &materialSystem;
        ctx.descriptorManager = &descriptorManager;

        heroModel = tiny::loadGLTFModel(ctx, basePath + "Models/Sponza/glTF/Sponza.gltf");
        if (!heroModel)
            std::cerr << "[Scene] Sponza load failed, falling back to DamagedHelmet\n";
    }

    if (heroModel)
    {
        const entt::entity hero = createTransformEntity(registry);
        registry.emplace<MeshComponent>(hero, MeshComponent{ heroModel });
        registry.emplace<NameComponent>(hero, NameComponent{ "Sponza" });
        registry.emplace<RenderTagComponent>(hero);
        setModelBounds(registry, hero, heroModel, &dirtySpatialEntities);
        attachDirtyTracking(hero);

        const glm::vec3 center = heroModel->getBoundsCenter();
        const float radius = heroModel->getBoundsRadius();
        std::cout << "[Scene] Sponza bounds min(" << heroModel->getBoundsMin().x << ", "
                  << heroModel->getBoundsMin().y << ", " << heroModel->getBoundsMin().z
                  << ") max(" << heroModel->getBoundsMax().x << ", "
                  << heroModel->getBoundsMax().y << ", " << heroModel->getBoundsMax().z
                  << ") center(" << center.x << ", " << center.y << ", " << center.z
                  << ") radius " << radius << "\n";

        // Default establishing shot for Sponza. The camera sits inside the
        // central aisle and looks down its long axis; config/engine.json can
        // still override this pose at runtime.
        TransformComponent& cameraTransform = getCameraTransform();
        cameraTransform.setTranslation({ -4.6485233f, 5.2119632f, 0.0523994f });
        cameraTransform.setEuler({ 0.3507974f, -17.3302765f, 0.0f });
    }
    else if (loadDefaultContent)
    {
        const entt::entity helmet = createTransformEntity(registry);
        auto helmetModel = Model::createModelFromFile(device, commandPool,
            basePath + "Models/DamagedHelmet/DamagedHelmet.obj");
        registry.emplace<MeshComponent>(helmet, MeshComponent{ helmetModel });
        registry.emplace<NameComponent>(helmet, NameComponent{ "DamagedHelmet" });
        registry.emplace<RenderTagComponent>(helmet);
        setModelBounds(registry, helmet, helmetModel, &dirtySpatialEntities);
        attachDirtyTracking(helmet);
        getCameraTransform().setTranslation({ 0.0f, 0.0f, -5.0f });
    }

    // Skybox: small sphere; the vertex shader performs depth/w translation.
    {
        const entt::entity skybox = createTransformEntity(registry);
        auto skyboxModel = Model::createModelFromFile(device, commandPool,
            basePath + "Models/sphere.obj");
        registry.emplace<MeshComponent>(skybox, MeshComponent{ skyboxModel });
        registry.emplace<NameComponent>(skybox, NameComponent{ "Skybox" });
        RenderTagComponent tag;
        tag.isSkybox = true;
        registry.emplace<RenderTagComponent>(skybox, tag);
        registry.get<TransformComponent>(skybox).setScale({ 0.1f, 0.1f, 0.1f });
        setModelBounds(registry, skybox, skyboxModel, &dirtySpatialEntities);
        attachDirtyTracking(skybox);
    }

    markAllTransformsDirty();
    markAllSpatialDirty();
    updateWorldTransforms();
    refreshSpatialIndex();
}

entt::entity Scene::createEntity()
{
    const entt::entity entity = createTransformEntity(registry);
    attachDirtyTracking(entity);
    markTransformDirty(entity);
    return entity;
}

void Scene::destroyEntity(entt::entity entity)
{
    if (!registry.valid(entity))
        return;

    if (entity == terrainStreamingEntity)
    {
        std::vector<entt::entity> chunks;
        chunks.reserve(streamingTerrainChunks.size());
        for (const auto& [key, chunk] : streamingTerrainChunks)
        {
            (void)key;
            if (registry.valid(chunk))
                chunks.push_back(chunk);
        }
        terrainStreamingEntity = entt::null;
        streamingTerrainChunks.clear();
        pendingStreamingTerrainChunks.clear();
        for (entt::entity chunk : chunks)
            destroyEntity(chunk);
    }
    else if (const auto* chunk = registry.try_get<TerrainChunkComponent>(entity))
    {
        const int64_t key = terrainChunkKey(chunk->chunkX, chunk->chunkZ);
        streamingTerrainChunks.erase(key);
        pendingStreamingTerrainChunks.erase(key);
    }

    latestTerrainRequest.erase(entity);

    if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
    {
        if (hierarchy->parent != entt::null && registry.valid(hierarchy->parent))
        {
            auto& siblings = registry.get<HierarchyComponent>(hierarchy->parent).children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        }

        for (entt::entity child : hierarchy->children)
        {
            if (registry.valid(child))
            {
                auto* childHierarchy = registry.try_get<HierarchyComponent>(child);
                if (childHierarchy)
                    childHierarchy->parent = entt::null;
            }
        }
    }

    // F2-g：销毁 chunk 时把 arena 切片交还，否则流送来回跑会把显存吃满。
    if (auto* terrain = registry.try_get<TerrainComponent>(entity))
    {
        if (terrainMeshArena && terrain->arenaIndexBase != TerrainMeshArena::kInvalidOffset)
        {
            TerrainMeshArena::Slice slice{};
            slice.vertexOffset = terrain->arenaVertexBase;
            slice.vertexCount = terrain->arenaVertexCount;
            slice.indexOffset = terrain->arenaIndexBase;
            slice.indexCount = terrain->arenaIndexCount;
            terrainMeshArena->release(slice);
        }
        terrain->arenaVertexBase = TerrainMeshArena::kInvalidOffset;
        terrain->arenaIndexBase = TerrainMeshArena::kInvalidOffset;
        terrain->arenaVertexCount = 0u;
        terrain->arenaIndexCount = 0u;
    }

    if (const auto* mesh = registry.try_get<MeshComponent>(entity))
        retireModel(mesh->model);

    spatialIndex.remove(entity);
    indexedBounds.erase(entity);
    dirtyTransformEntities.erase(entity);
    dirtySpatialEntities.erase(entity);
    registry.destroy(entity);
}

void Scene::setParent(entt::entity child, entt::entity parent)
{
    if (!registry.valid(child))
        throw std::runtime_error("Scene::setParent: invalid child entity");
    if (parent != entt::null && !registry.valid(parent))
        throw std::runtime_error("Scene::setParent: invalid parent entity");
    if (child == parent)
        throw std::runtime_error("Scene::setParent: entity cannot parent itself");
    if (parent != entt::null && isDescendant(registry, parent, child))
        throw std::runtime_error("Scene::setParent: cycle detected");

    auto& childHierarchy = registry.get_or_emplace<HierarchyComponent>(child);
    if (childHierarchy.parent != entt::null && registry.valid(childHierarchy.parent))
    {
        auto& oldChildren = registry.get<HierarchyComponent>(childHierarchy.parent).children;
        oldChildren.erase(std::remove(oldChildren.begin(), oldChildren.end(), child), oldChildren.end());
    }

    childHierarchy.parent = parent;
    if (parent != entt::null)
        registry.get_or_emplace<HierarchyComponent>(parent).children.push_back(child);
    markTransformDirty(child);
}

void Scene::update(float dt, GLFWwindow* window, float aspectRatio, bool allowCameraInput)
{
    ++frameCounter;
    collectRetiredModels();
    const auto terrainPollStart = std::chrono::steady_clock::now();
    processTerrainGeneration();
    recordTerrainPollMs(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - terrainPollStart).count());

    TransformComponent& cameraTransform = getCameraTransform();
    cameraController.inputEnabled = allowCameraInput;
    cameraController.processPCInput(window, dt, cameraTransform);
    if (cameraTransform.isDirty())
        markTransformDirty(cameraEntity);

    maybeRebaseWorldOrigin();
    camera.setViewYXZ(cameraTransform.getTranslation(), cameraTransform.getEuler());
    camera.setPerspectiveProjection(glm::radians(60.0f), aspectRatio, 0.1f, 5000.0f);

    refreshStreamingTerrain();

    updateWorldTransforms();
    refreshSpatialIndex();
    updateVisibility(camera.getProjection() * camera.getView());
}

void Scene::markTransformDirty(entt::entity entity)
{
    if (!registry.valid(entity))
        return;
    const auto* transform = registry.try_get<TransformComponent>(entity);
    if (!transform)
        return;
    // Explicit calls always enqueue: callers may have written public fields
    // directly and then notified manually, so component dirty/revision alone
    // cannot prove the data is unchanged.
    dirtyTransformEntities.insert(entity);
}

void Scene::markSpatialDirty(entt::entity entity)
{
    if (registry.valid(entity))
        dirtySpatialEntities.insert(entity);
}

void Scene::attachDirtyTracking(entt::entity entity)
{
    if (!registry.valid(entity))
        return;
    if (auto* transform = registry.try_get<TransformComponent>(entity))
        transform->setDirtyOwner(this, &notifyTransformDirty, entity);
    if (auto* bounds = registry.try_get<BoundsComponent>(entity))
        bounds->setDirtyOwner(this, &notifySpatialDirty, entity);
}

void Scene::markAllTransformsDirty()
{
    for (entt::entity entity : registry.view<TransformComponent>())
        dirtyTransformEntities.insert(entity);
}

void Scene::markAllSpatialDirty()
{
    for (entt::entity entity : registry.view<MeshComponent>())
        dirtySpatialEntities.insert(entity);
}

void Scene::retireModel(const std::shared_ptr<Model>& model)
{
    if (!model)
        return;
    retiredModels.push_back(RetiredModel{ model, frameCounter + 4u });
}

void Scene::collectRetiredModels()
{
    retiredModels.erase(
        std::remove_if(retiredModels.begin(), retiredModels.end(),
                       [this](const RetiredModel& retired)
                       {
                           return retired.releaseFrame <= frameCounter;
                       }),
        retiredModels.end());
}

void Scene::updateWorldTransforms()
{
    lastWorldTransformUpdates = updateAllWorldTransforms(
        registry, &dirtyTransformEntities, &dirtySpatialEntities);
}

void Scene::refreshSpatialIndex()
{
    lastSpatialIndexUpdates = 0;

    const auto sameBounds = [](const AABB& a, const AABB& b)
    {
        if (a.valid != b.valid)
            return false;
        if (!a.valid)
            return true;
        const glm::vec3 dmin = glm::abs(a.min - b.min);
        const glm::vec3 dmax = glm::abs(a.max - b.max);
        constexpr float kEpsilon = 1e-5f;
        return dmin.x <= kEpsilon && dmin.y <= kEpsilon && dmin.z <= kEpsilon &&
               dmax.x <= kEpsilon && dmax.y <= kEpsilon && dmax.z <= kEpsilon;
    };

    for (auto it = dirtySpatialEntities.begin(); it != dirtySpatialEntities.end();)
    {
        const entt::entity entity = *it;
        if (!registry.valid(entity))
        {
            spatialIndex.remove(entity);
            indexedBounds.erase(entity);
            it = dirtySpatialEntities.erase(it);
            continue;
        }

        const MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        const RenderTagComponent* tag = registry.try_get<RenderTagComponent>(entity);
        if (!mesh || !mesh->model || (tag && tag->isSkybox))
        {
            spatialIndex.remove(entity);
            indexedBounds.erase(entity);
            it = dirtySpatialEntities.erase(it);
            continue;
        }

        BoundsComponent& bounds = registry.get_or_emplace<BoundsComponent>(entity);
        if (!bounds.getLocal().valid)
        {
            AABB localBounds = bounds.getLocal();
            localBounds.min = mesh->model->getBoundsMin();
            localBounds.max = mesh->model->getBoundsMax();
            localBounds.valid = true;
            bounds.setLocal(localBounds);
        }

        const WorldTransformComponent& world =
            registry.get_or_emplace<WorldTransformComponent>(entity);
        if (!world.valid)
        {
            // The entity was inserted between updateWorldTransforms() and this
            // pass. Keep it queued; the next update will make the world matrix.
            markTransformDirty(entity);
            ++it;
            continue;
        }

        WorldBoundsComponent& worldBounds =
            registry.get_or_emplace<WorldBoundsComponent>(entity);
        worldBounds.world = transformAABB(bounds.getLocal(), world.matrix);
        worldBounds.dirty = false;
        bounds.clearDirty();
        ++lastSpatialIndexUpdates;

        auto indexed = indexedBounds.find(entity);
        if (indexed == indexedBounds.end() ||
            !sameBounds(indexed->second, worldBounds.world))
        {
            spatialIndex.insert(entity, worldBounds.world);
            indexedBounds[entity] = worldBounds.world;
        }
        it = dirtySpatialEntities.erase(it);
    }
}

void Scene::updateVisibility(const glm::mat4& viewProjection)
{
    visibleEntities.clear();

    const Frustum frustum = extractFrustum(viewProjection);
    const AABB broadBounds = frustumBroadBounds(viewProjection);

    for (auto [entity, mesh, tag] : registry.view<MeshComponent, RenderTagComponent>().each())
    {
        if (tag.isSkybox)
            visibleEntities.push_back(entity);
    }

    std::vector<entt::entity> candidates;
    spatialIndex.query(broadBounds, candidates);

    for (entt::entity entity : candidates)
    {
        const auto* tag = registry.try_get<RenderTagComponent>(entity);
        const auto* worldBounds = registry.try_get<WorldBoundsComponent>(entity);
        if (!worldBounds)
            continue;

        // Skybox is camera-following and always needed as the background.
        if ((tag && tag->isSkybox) || intersectsAABB(frustum, worldBounds->world))
            visibleEntities.push_back(entity);
    }
}

void Scene::cleanup()
{
    cancelPendingTerrainGenerations();
    terrainComputeGenerator.reset();
    terrainGpuMeshBuilder.reset();
    terrainMaterial.reset();
    materialSystem.cleanup();
    registry.clear();
    spatialIndex.clear();
    indexedBounds.clear();
    retiredModels.clear();
    visibleEntities.clear();
    cameraEntity = entt::null;
}

bool Scene::selfTest()
{
    bool ok = true;

    // F2 / D-18: CPU terrain algorithms and mesh invariants.
    if (!terrain::selfTest())
        ok = false;

    entt::registry registry;
    const entt::entity parent = createTransformEntity(registry);
    const entt::entity child = createTransformEntity(registry);
    const entt::entity grandchild = createTransformEntity(registry);

    registry.get<TransformComponent>(parent).setTranslation({ 1.0f, 2.0f, 3.0f });
    registry.get<TransformComponent>(child).setTranslation({ 0.0f, 1.0f, 0.0f });
    registry.get<TransformComponent>(grandchild).setTranslation({ 0.0f, 0.0f, 2.0f });

    registry.get<HierarchyComponent>(parent).children.push_back(child);
    registry.get<HierarchyComponent>(child).parent = parent;
    registry.get<HierarchyComponent>(child).children.push_back(grandchild);
    registry.get<HierarchyComponent>(grandchild).parent = child;

    const uint32_t initialTransformUpdates = updateAllWorldTransforms(registry);
    const uint32_t cleanTransformUpdates = updateAllWorldTransforms(registry);

    glm::vec3 childPos = registry.get<WorldTransformComponent>(child).worldPosition;
    glm::vec3 grandchildPos = registry.get<WorldTransformComponent>(grandchild).worldPosition;
    if (initialTransformUpdates != 3u || cleanTransformUpdates != 0u ||
        glm::length(childPos - glm::vec3(1.0f, 3.0f, 3.0f)) > 0.0001f ||
        glm::length(grandchildPos - glm::vec3(1.0f, 3.0f, 5.0f)) > 0.0001f)
    {
        std::cerr << "[Scene] selfTest FAIL: hierarchy dirty-transform update mismatch\n";
        ok = false;
    }

    // Dirty child only: exactly one matrix rebuild; descendants follow.
    auto& childTransform = registry.get<TransformComponent>(child);
    childTransform.setTranslation({ 0.0f, 2.0f, 0.0f });
    if (updateAllWorldTransforms(registry) != 2u)
    {
        std::cerr << "[Scene] selfTest FAIL: dirty child subtree rebuilt wrong transform count\n";
        ok = false;
    }
    childPos = registry.get<WorldTransformComponent>(child).worldPosition;
    grandchildPos = registry.get<WorldTransformComponent>(grandchild).worldPosition;
    if (glm::length(childPos - glm::vec3(1.0f, 4.0f, 3.0f)) > 0.0001f ||
        glm::length(grandchildPos - glm::vec3(1.0f, 4.0f, 5.0f)) > 0.0001f)
    {
        std::cerr << "[Scene] selfTest FAIL: dirty child world position mismatch\n";
        ok = false;
    }

    // Dirty parent forces the whole subtree to rebuild, even if children are clean.
    registry.get<TransformComponent>(parent).setTranslation({ 2.0f, 2.0f, 3.0f });
    if (updateAllWorldTransforms(registry) != 3u)
    {
        std::cerr << "[Scene] selfTest FAIL: dirty parent did not rebuild subtree\n";
        ok = false;
    }
    childPos = registry.get<WorldTransformComponent>(child).worldPosition;
    grandchildPos = registry.get<WorldTransformComponent>(grandchild).worldPosition;
    if (glm::length(childPos - glm::vec3(2.0f, 4.0f, 3.0f)) > 0.0001f ||
        glm::length(grandchildPos - glm::vec3(2.0f, 4.0f, 5.0f)) > 0.0001f)
    {
        std::cerr << "[Scene] selfTest FAIL: parent-propagated world position mismatch\n";
        ok = false;
    }

    const auto& childWorld = registry.get<WorldTransformComponent>(child);
    if (childWorld.sourceRevision != childTransform.getLocalRevision() || !childWorld.valid)
    {
        std::cerr << "[Scene] selfTest FAIL: world transform revision not synchronized\n";
        ok = false;
    }

    SpatialIndex index(4.0f);
    const entt::entity nearEntity = registry.create();
    const entt::entity farEntity = registry.create();
    index.insert(nearEntity, AABB{ { 0.0f, 0.0f, 0.0f }, { 3.0f, 3.0f, 3.0f }, true });
    index.insert(farEntity, AABB{ { 100.0f, 100.0f, 100.0f }, { 104.0f, 104.0f, 104.0f }, true });

    std::vector<entt::entity> result;
    index.query(AABB{ { 1.0f, 1.0f, 1.0f }, { 2.0f, 2.0f, 2.0f }, true }, result);
    if (result.size() != 1 || result[0] != nearEntity)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial index query mismatch\n";
        ok = false;
    }

    // Incremental upsert/remove: moving an entity must leave no stale bucket.
    index.insert(nearEntity, AABB{ { 101.0f, 101.0f, 101.0f }, { 102.0f, 102.0f, 102.0f }, true });
    index.query(AABB{ { 1.0f, 1.0f, 1.0f }, { 2.0f, 2.0f, 2.0f }, true }, result);
    if (!result.empty())
    {
        std::cerr << "[Scene] selfTest FAIL: spatial index left stale bucket after upsert\n";
        ok = false;
    }
    index.query(AABB{ { 100.0f, 100.0f, 100.0f }, { 103.0f, 103.0f, 103.0f }, true }, result);
    if (result.size() != 2 || index.entryCount() != 2)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial index upsert/entry count mismatch\n";
        ok = false;
    }
    // B-06: region buckets are derived from the same world AABBs. Changing the
    // region size must rebuild existing entries, and remove() must clear both
    // the fine cells and the coarse region buckets.
    index.setRegionSize(10.0f);
    std::vector<entt::entity> regionResult;
    index.queryRegion(0, 0, regionResult);
    if (!regionResult.empty())
    {
        std::cerr << "[Scene] selfTest FAIL: empty spatial region returned candidates\n";
        ok = false;
    }
    index.queryRegion(10, 10, regionResult);
    if (regionResult.size() != 2 || index.regionCellCount() == 0)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial region query mismatch\n";
        ok = false;
    }
    index.queryRegionRange(0, 0, 100, 100, regionResult);
    if (regionResult.size() != 2)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial region range query mismatch\n";
        ok = false;
    }

    if (!index.remove(nearEntity) || index.remove(nearEntity) ||
        index.entryCount() != 1)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial index remove semantics mismatch\n";
        ok = false;
    }

    index.queryRegion(10, 10, regionResult);
    if (regionResult.size() != 1 || regionResult[0] != farEntity)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial region remove left stale bucket\n";
        ok = false;
    }

    // Per-chunk submesh culling must keep the visible center chunk and reject
    // a chunk fully outside the camera frustum.
    {
        const glm::mat4 identity(1.0f);
        render::SubMeshCuller culler(identity);
        if (!culler.intersects(glm::vec3(0.0f), glm::vec3(0.5f), identity))
        {
            std::cerr << "[Scene] selfTest FAIL: visible chunk was frustum-culled\n";
            ok = false;
        }
        if (culler.intersects(glm::vec3(1000.0f, 0.0f, 0.0f),
                              glm::vec3(1010.0f, 10.0f, 10.0f), identity))
        {
            std::cerr << "[Scene] selfTest FAIL: chunk outside frustum survived culling\n";
            ok = false;
        }

        if (render::selectTerrainLod(50.0f, 50.0f, 1.0f, 3u) != 0u ||
            render::selectTerrainLod(100.0f, 50.0f, 1.0f, 3u) != 1u ||
            render::selectTerrainLod(200.0f, 50.0f, 1.0f, 3u) != 2u ||
            render::selectTerrainLod(200.0f, 50.0f, 1.0f, 1u) != 0u)
        {
            std::cerr << "[Scene] selfTest FAIL: terrain LOD distance selection mismatch\n";
            ok = false;
        }

        // 15% hysteresis: stay on LOD0 at 70m but coarsen at 80m.
        if (render::selectTerrainLodHysteresis(70.0f, 50.0f, 1.0f, 3u, 0) != 0u ||
            render::selectTerrainLodHysteresis(80.0f, 50.0f, 1.0f, 3u, 0) != 1u ||
            render::selectTerrainLodHysteresis(60.0f, 50.0f, 1.0f, 3u, 1) != 1u ||
            render::selectTerrainLodHysteresis(40.0f, 50.0f, 1.0f, 3u, 1) != 0u)
        {
            std::cerr << "[Scene] selfTest FAIL: terrain LOD hysteresis mismatch\n";
            ok = false;
        }

        render::TerrainLodState transition;
        render::TerrainLodSelection initial = render::updateTerrainLodSelection(
            100.0f, 50.0f, 1.0f, 3u, 0.0f, transition);
        render::TerrainLodSelection refine = render::updateTerrainLodSelection(
            40.0f, 50.0f, 1.0f, 3u, 0.1f, transition);
        render::TerrainLodSelection midBlend = render::updateTerrainLodSelection(
            40.0f, 50.0f, 1.0f, 3u, 0.1f, transition);
        const render::TerrainLodSelection lateBlend =
            render::updateTerrainLodSelection(
                40.0f, 50.0f, 1.0f, 3u, 0.1f, transition);
        render::TerrainLodSelection done = render::updateTerrainLodSelection(
            40.0f, 50.0f, 1.0f, 3u, 0.1f, transition);
        if (initial.blending || initial.lodA != 1u ||
            !refine.blending || refine.lodA != 1u || refine.lodB != 0u ||
            !midBlend.blending || !(midBlend.blend > 0.0f && midBlend.blend < 1.0f) ||
            !lateBlend.blending || !(lateBlend.blend > midBlend.blend) ||
            done.blending || done.lodA != 0u)
        {
            std::cerr << "[Scene] selfTest FAIL: terrain LOD dither transition mismatch\n";
            ok = false;
        }
    }

    // 旋转现在以四元数为准，欧拉角只是显示值。这里钉住三件事：
    //   1. setEuler -> orientation -> eulerFromQuat 往返一致（非万向节区）；
    //   2. 四元数矩阵与旧的 T*Ry*Rx*Rz*S 完全等价（不能悄悄改变旋转约定）；
    //   3. 万向节附近 orientation 仍然有效，矩阵不是 NaN。
    {
        TransformComponent rotationTest;
        const glm::vec3 euler{ 0.31f, -0.72f, 1.13f };
        rotationTest.setEuler(euler);
        const glm::vec3 roundTrip = TransformComponent::eulerFromQuat(rotationTest.getOrientation());
        if (glm::length(roundTrip - euler) > 1e-4f)
        {
            std::cerr << "[Scene] selfTest FAIL: quaternion/euler round trip mismatch\n";
            ok = false;
        }

        glm::mat4 legacy = glm::translate(glm::mat4{ 1.0f }, rotationTest.getTranslation());
        legacy = glm::rotate(legacy, euler.y, glm::vec3(0.0f, 1.0f, 0.0f));
        legacy = glm::rotate(legacy, euler.x, glm::vec3(1.0f, 0.0f, 0.0f));
        legacy = glm::rotate(legacy, euler.z, glm::vec3(0.0f, 0.0f, 1.0f));
        legacy = glm::scale(legacy, rotationTest.getScale());
        const glm::mat4 fromQuat = rotationTest.localMatrix();
        float maxDiff = 0.0f;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                maxDiff = std::max(maxDiff, std::abs(legacy[column][row] - fromQuat[column][row]));
        if (maxDiff > 1e-5f)
        {
            std::cerr << "[Scene] selfTest FAIL: quaternion matrix differs from legacy Euler order\n";
            ok = false;
        }

        // pitch 到 90° 是欧拉角的奇异点；四元数必须仍然能生成有限矩阵。
        TransformComponent gimbalTest;
        gimbalTest.setOrientation(glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)) *
                                  TransformComponent::quatFromEuler({ 0.2f, 0.3f, 0.4f }));
        const glm::mat4 gimbalMatrix = gimbalTest.localMatrix();
        bool finite = true;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                finite = finite && std::isfinite(gimbalMatrix[column][row]);
        if (!finite)
        {
            std::cerr << "[Scene] selfTest FAIL: gimbal-lock orientation produced NaN\n";
            ok = false;
        }

        // 单轴旋转反解只能落在对应分量上；拖 X 手柄时 Inspector 不应该同时跳 Y/Z。
        const glm::vec3 axes[3] = {
            { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }
        };
        for (int axis = 0; axis < 3; ++axis)
        {
            const glm::vec3 e = TransformComponent::eulerFromQuat(
                glm::angleAxis(0.4f, axes[axis]));
            glm::vec3 expected(0.0f);
            expected[axis] = 0.4f;
            if (glm::length(e - expected) > 1e-4f)
            {
                std::cerr << "[Scene] selfTest FAIL: single-axis euler extraction mismatch\n";
                ok = false;
            }
        }
    }

    std::cout << "[Scene] ECS/spatial selfTest " << (ok ? "PASS" : "FAIL")
              << " | hierarchy=T*Ry*Rx*Rz*S | grid cells=" << index.cellCount() << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// 场景描述：导出 / 重建（编辑器与序列化共用）
// ---------------------------------------------------------------------------
scene::SceneDescription Scene::captureDescription() const
{
    scene::SceneDescription description;
    description.version = 1;

    // Streamed chunk entities are runtime derivatives of the manager; only the
    // manager template is serialized so load can reconstruct the active set.
    std::vector<entt::entity> serializableEntities;
    for (entt::entity entity : registry.view<TransformComponent>())
    {
        if (!registry.any_of<TerrainChunkComponent>(entity))
            serializableEntities.push_back(entity);
    }

    // 建立 "实体 -> 下标" 映射。父子关系用下标表示，因为实体 ID 跨进程不稳定。
    std::unordered_map<uint32_t, int> idToIndex;
    int nextIndex = 0;
    for (entt::entity entity : serializableEntities)
        idToIndex[static_cast<uint32_t>(entity)] = nextIndex++;

    description.entities.reserve(idToIndex.size());

    for (entt::entity entity : serializableEntities)
    {
        scene::EntityDescription e;
        e.name = registry.try_get<NameComponent>(entity)
            ? registry.get<NameComponent>(entity).name
            : std::string("Entity");

        const TransformComponent& transform = registry.get<TransformComponent>(entity);
        e.translation = transform.getTranslation();
        e.rotation = transform.getEuler();
        e.scale = transform.getScale();

        if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
        {
            if (hierarchy->parent != entt::null)
            {
                const auto it = idToIndex.find(static_cast<uint32_t>(hierarchy->parent));
                if (it != idToIndex.end())
                    e.parentIndex = it->second;
            }
        }

        if (const auto* mesh = registry.try_get<MeshComponent>(entity))
        {
            if (mesh->model)
                e.meshPath = mesh->model->getSourcePath();
        }

        if (const auto* tag = registry.try_get<RenderTagComponent>(entity))
        {
            e.isSkybox = tag->isSkybox;
            e.isGrid = tag->isGrid;
            e.isTerrain = tag->isTerrain;
        }

        if (const auto* streaming = registry.try_get<TerrainStreamingComponent>(entity))
        {
            e.isTerrainWorld = true;
            e.terrainWorldConfig = streaming->chunkConfig;
            e.terrainLoadRadius = streaming->loadRadius;
            e.terrainUnloadPadding = streaming->unloadPadding;
            e.terrainMaxLoadsPerFrame = streaming->maxLoadsPerFrame;
        }
        else if (const auto* terrainComp = registry.try_get<TerrainComponent>(entity))
        {
            e.isTerrain = true;
            e.terrainConfig = terrainComp->config;
        }
        if (const auto* character = registry.try_get<CharacterControllerComponent>(entity))
        {
            e.isCharacter = true;
            e.characterRadius = character->radius;
            e.characterHeight = character->height;
            e.characterMoveSpeed = character->moveSpeed;
            e.characterJumpSpeed = character->jumpSpeed;
            e.characterGravity = character->gravity;
            e.characterMaxSlopeAngle = character->maxSlopeAngle;
            e.characterStepHeight = character->stepHeight;
            e.characterGroundSnapDistance = character->groundSnapDistance;
        }

        // CameraComponent 是空 tag 组件，用 any_of 判断存在性。
        e.isCamera = registry.any_of<CameraComponent>(entity);

        if (const auto* materialComp = registry.try_get<MaterialComponent>(entity))
        {
            if (materialComp->material)
            {
                const Material& m = *materialComp->material;
                e.material.present = true;
                e.material.name = m.name;
                e.material.baseColorFactor = m.baseColorFactor;
                e.material.metallicFactor = m.metallicFactor;
                e.material.roughnessFactor = m.roughnessFactor;
                e.material.alphaCutoff = m.alphaCutoff;
                e.material.alphaMode = static_cast<int>(m.alphaMode);
                if (m.albedoMap)            e.material.albedoMap = m.albedoMap->getPath();
                if (m.normalMap)            e.material.normalMap = m.normalMap->getPath();
                if (m.metallicRoughnessMap) e.material.metallicRoughnessMap = m.metallicRoughnessMap->getPath();
            }
        }

        // 注意：这里用 push_back，顺序必须与我上面 enum 的顺序一致。
        // 因为 idToIndex 也是按同一个 view 顺序建的，所以两边一致。
        description.entities.push_back(std::move(e));
    }

    return description;
}

void Scene::clearEntities()
{
    discardPendingTerrainCallbacks();
    terrainStreamingEntity = entt::null;
    streamingTerrainChunks.clear();
    pendingStreamingTerrainChunks.clear();

    // 先归还材质槽位（延迟回收，不阻塞设备），再销毁实体。
    // 不归还的话，反复"新建场景"会慢慢吃满 256 个 bindless 槽位。
    for (entt::entity entity : registry.view<MaterialComponent>())
    {
        auto& materialComp = registry.get<MaterialComponent>(entity);
        if (materialComp.materialSetIndex != kInvalidMaterialSlot)
        {
            descriptorManager->releaseMaterialSet(materialComp.materialSetIndex);
            materialComp.materialSetIndex = kInvalidMaterialSlot;
        }
    }

    const entt::entity keepCamera = cameraEntity;

    std::vector<entt::entity> all;
    for (entt::entity entity : registry.view<TransformComponent>())
        all.push_back(entity);

    for (entt::entity entity : all)
    {
        if (entity == keepCamera)
            continue;
        // B-06: single production destroy path. destroyEntity retires the
        // mesh, removes spatial/index bookkeeping, and clears streaming maps.
        destroyEntity(entity);
    }

    // 相机实体本身也可能带有子节点，重新挂成根
    if (registry.valid(keepCamera))
    {
        auto& hierarchy = registry.get_or_emplace<HierarchyComponent>(keepCamera);
        hierarchy.parent = entt::null;
        hierarchy.children.clear();
    }

    spatialIndex.clear();
    indexedBounds.clear();
    dirtyTransformEntities.clear();
    dirtySpatialEntities.clear();
    visibleEntities.clear();
    updateWorldTransforms();
    refreshSpatialIndex();
}

bool Scene::buildFromDescription(const scene::SceneDescription& description, std::string& errorMessage)
{
    if (!device || !descriptorManager)
    {
        errorMessage = "scene is not initialized";
        return false;
    }

    clearEntities();

    // 注意：不要写成 vector<entt::entity>(n, entt::null)。
    // MSVC 在 vector 的填充构造里会拿 allocator<entity> 去实例化 entt_traits，
    // 报 "entity_type 不是 std::allocator<Entity> 的成员"。
    // 先把 entt::null 转成具名 entt::entity，转换就发生在非模板推导上下文里。
    const entt::entity nullEntity = entt::null;
    std::vector<entt::entity> created(description.entities.size(), nullEntity);
    bool foundCamera = false;

    for (size_t i = 0; i < description.entities.size(); ++i)
    {
        const scene::EntityDescription& e = description.entities[i];

        const entt::entity entity = registry.create();
        created[i] = entity;

        TransformComponent transform;
        transform.setTranslation(e.translation);
        transform.setEuler(e.rotation);
        transform.setScale(e.scale);
        registry.emplace<TransformComponent>(entity, transform);
        registry.emplace<HierarchyComponent>(entity);
        registry.emplace<NameComponent>(entity, NameComponent{ e.name });

        if (e.isCamera && !foundCamera)
        {
            // 复用已有的相机实体，而不是新建 —— Scene 里 cameraEntity 是唯一句柄
            destroyEntity(entity);
            created[i] = cameraEntity;
            registry.get<NameComponent>(cameraEntity).name = e.name;
            TransformComponent& cameraTransform = registry.get<TransformComponent>(cameraEntity);
            cameraTransform.setPose(transform.getTranslation(), transform.getEuler(),
                                    transform.getScale(), transform.getOrientation());
            foundCamera = true;
        }

        if (e.isTerrainWorld)
        {
            const entt::entity target = created[i];
            if (terrainStreamingEntity != entt::null &&
                registry.valid(terrainStreamingEntity))
            {
                errorMessage = "scene contains more than one streaming terrain manager";
                clearEntities();
                return false;
            }
            terrain::TerrainConfig config = e.terrainWorldConfig;
            config.chunkGridX = 1u;
            config.chunkGridZ = 1u;
            config.localColorRange = false;
            std::string configError;
            if (!config.validate(configError))
            {
                errorMessage = "streaming terrain config: " + configError;
                clearEntities();
                return false;
            }
            TerrainStreamingComponent streaming;
            streaming.chunkConfig = config;
            streaming.loadRadius = std::clamp<int32_t>(
                e.terrainLoadRadius, 1, kMaxTerrainStreamingRadius);
            streaming.unloadPadding = std::max(0, e.terrainUnloadPadding);
            streaming.maxLoadsPerFrame = std::clamp<uint32_t>(
                e.terrainMaxLoadsPerFrame, 1u, kMaxTerrainStreamingLoadsPerFrame);
            registry.emplace<TerrainStreamingComponent>(target, streaming);
            terrainStreamingEntity = target;
        }
        else if (e.isTerrain)
        {
            const entt::entity target = created[i];
            std::shared_ptr<Model> terrainModel;
            std::shared_ptr<terrain::HeightField> heightField;
            uint32_t vertexCount = 0;
            uint32_t indexCount = 0;
            double generationMs = 0.0;
            bool meshGeneratedOnGpu = false;
            if (!buildTerrainModel(e.terrainConfig, terrainModel, vertexCount,
                                   indexCount, generationMs, errorMessage,
                                   &heightField, &meshGeneratedOnGpu) || !terrainModel)
            {
                // Do not leave a half-built scene behind after a terrain failure.
                clearEntities();
                return false;
            }

            registry.emplace<MeshComponent>(target, MeshComponent{ terrainModel });
            RenderTagComponent tag;
            tag.isTerrain = true;
            registry.emplace<RenderTagComponent>(target, tag);

            TerrainComponent terrainComponent;
            terrainComponent.config = e.terrainConfig;
            terrainComponent.generatedConfig = e.terrainConfig;
            terrainComponent.revision = 1;
            terrainComponent.vertexCount = vertexCount;
            terrainComponent.indexCount = indexCount;
            terrainComponent.lastGenerationMs = generationMs;
            terrainComponent.generated = true;
            terrainComponent.meshGeneratedOnGpu = meshGeneratedOnGpu;
            terrainComponent.cpuHeightField = std::move(heightField);
            terrainComponent.generationBackend =
                (terrainComponent.cpuHeightField->generatedOnGpu
                    ? (meshGeneratedOnGpu ? "GPU height + GPU mesh" : "GPU height")
                    : (meshGeneratedOnGpu ? "GPU mesh" : "CPU fallback"));
            registry.emplace<TerrainComponent>(target, std::move(terrainComponent));
            setModelBounds(registry, target, terrainModel, &dirtySpatialEntities);
        }
        if (e.isCharacter)
        {
            CharacterControllerComponent character;
            character.radius = e.characterRadius;
            character.height = e.characterHeight;
            character.moveSpeed = e.characterMoveSpeed;
            character.jumpSpeed = e.characterJumpSpeed;
            character.gravity = e.characterGravity;
            character.maxSlopeAngle = e.characterMaxSlopeAngle;
            character.stepHeight = e.characterStepHeight;
            character.groundSnapDistance = e.characterGroundSnapDistance;
            registry.emplace<CharacterControllerComponent>(created[i], character);
        }
        else if (!e.meshPath.empty())
        {
            const entt::entity target = created[i];
            std::shared_ptr<Model> model;
            try
            {
                model = Model::createModelFromFile(*device, commandPool, e.meshPath);
            }
            catch (const std::exception& ex)
            {
                errorMessage = std::string("failed to load mesh '") + e.meshPath + "': " + ex.what();
                clearEntities();
                return false;
            }
            if (model)
            {
                registry.emplace<MeshComponent>(target, MeshComponent{ model });
                RenderTagComponent tag;
                tag.isSkybox = e.isSkybox;
                tag.isGrid = e.isGrid;
                registry.emplace<RenderTagComponent>(target, tag);
                setModelBounds(registry, target, model, &dirtySpatialEntities);
            }
            else
            {
                errorMessage = "mesh loader returned null for '" + e.meshPath + "'";
                clearEntities();
                return false;
            }
        }
        else if (e.isSkybox || e.isGrid)
        {
            RenderTagComponent tag;
            tag.isSkybox = e.isSkybox;
            tag.isGrid = e.isGrid;
            registry.emplace<RenderTagComponent>(created[i], tag);
        }

        // 材质：默认用 defaultMat（MaterialSystem 已建好的），有描述则覆盖参数
        if (created[i] != entt::null && registry.valid(created[i]))
        {
            if (!e.isTerrain && registry.try_get<MeshComponent>(created[i]))
            {
                MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(created[i]);
                materialComp.material = materialSystem.getOrCreateMaterial(
                    e.material.present ? e.material.name : std::string("scene_default"));
                materialComp.material->baseColorFactor = e.material.baseColorFactor;
                materialComp.material->metallicFactor = e.material.metallicFactor;
                materialComp.material->roughnessFactor = e.material.roughnessFactor;
                materialComp.material->alphaCutoff = e.material.alphaCutoff;
                materialComp.material->alphaMode = static_cast<AlphaMode>(e.material.alphaMode);

                // 贴图按需加载（走 MaterialSystem 的缓存）
                auto loadIfAny = [&](const std::string& path, bool srgb) -> std::shared_ptr<Texture>
                {
                    if (path.empty()) return nullptr;
                    return materialSystem.loadTexture(path, *device, commandPool, srgb);
                };
                if (!e.material.albedoMap.empty())
                    materialComp.material->albedoMap = loadIfAny(e.material.albedoMap, true);
                if (!e.material.normalMap.empty())
                    materialComp.material->normalMap = loadIfAny(e.material.normalMap, false);
                if (!e.material.metallicRoughnessMap.empty())
                    materialComp.material->metallicRoughnessMap = loadIfAny(e.material.metallicRoughnessMap, false);

                // 重新分配 bindless 槽位（序列化不存槽位号，因为它是运行期分发的）。
                // 槽位归实体所有，和 createMeshEntity 保持同一套规则。
                materialComp.materialSetIndex = descriptorManager->allocateMaterialSet(*materialComp.material);
            }
        }
    }

    for (entt::entity entity : created)
        attachDirtyTracking(entity);

    // 第二遍：挂父子关系（父必须先存在，所以不能和创建放在一个循环里）
    for (size_t i = 0; i < description.entities.size(); ++i)
    {
        const int parentIndex = description.entities[i].parentIndex;
        if (parentIndex < 0 || parentIndex >= static_cast<int>(created.size()))
            continue;

        // 相机实体被复用时 created[i] 指向它，同样要参与挂接
        const entt::entity child = created[i];
        const entt::entity parent = created[parentIndex];
        if (child == entt::null || parent == entt::null)
            continue;
        if (!registry.valid(child) || !registry.valid(parent))
            continue;
        if (child == parent)
            continue;

        registry.get<HierarchyComponent>(child).parent = parent;
        registry.get<HierarchyComponent>(parent).children.push_back(child);
    }

    markAllTransformsDirty();
    markAllSpatialDirty();
    refreshTransformsAndVisibility();
    updateTerrainStreaming();
    return true;
}

entt::entity Scene::createMeshEntity(const std::string& absolutePath, const std::string& name)
{
    if (!device || !descriptorManager)
        return entt::null;

    std::shared_ptr<Model> model;
    try
    {
        model = Model::createModelFromFile(*device, commandPool, absolutePath);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[Scene] createMeshEntity failed: " << ex.what() << "\n";
        return entt::null;
    }
    if (!model)
        return entt::null;

    const entt::entity entity = createTransformEntity(registry);
    markTransformDirty(entity);
    registry.emplace<MeshComponent>(entity, MeshComponent{ model });
    registry.emplace<RenderTagComponent>(entity);
    registry.emplace<NameComponent>(entity, NameComponent{ name });
    setModelBounds(registry, entity, model, &dirtySpatialEntities);
    attachDirtyTracking(entity);

    MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(entity);
    materialComp.material = materialSystem.getOrCreateMaterial(name + "_mat");

    // 实体级槽位归 MaterialComponent 所有：每个实体一个槽位，clearEntities()
    // 按实体归还。不要写 Material::descriptorSetIndex —— 那是材质对象的字段，
    // 同名的多个实体会共享同一个 Material，但各自持有独立的槽位。
    materialComp.materialSetIndex = descriptorManager->allocateMaterialSet(*materialComp.material);

    updateWorldTransforms();
    refreshSpatialIndex();
    return entity;
}

bool Scene::buildTerrainModelFromHeightField(
    const terrain::TerrainConfig& config,
    const std::shared_ptr<terrain::HeightField>& heightField,
    std::shared_ptr<Model>& outModel,
    uint32_t& outVertexCount,
    uint32_t& outIndexCount,
    double& outGenerationMs,
    std::string& errorMessage,
    bool* outMeshGeneratedOnGpu)
{
    outModel.reset();
    outVertexCount = 0;
    outIndexCount = 0;
    outGenerationMs = 0.0;
    errorMessage.clear();
    if (outMeshGeneratedOnGpu)
        *outMeshGeneratedOnGpu = false;

    if (!device)
    {
        errorMessage = "scene is not initialized";
        return false;
    }
    if (!heightField)
    {
        errorMessage = "terrain heightfield is missing";
        return false;
    }

    const bool gpuMeshEligible = terrainGpuMeshBuilder &&
        terrainGpuMeshBuilder->supports(config);
    if (gpuMeshEligible)
    {
        TerrainGpuMeshBuildResult gpuMesh;
        std::string gpuMeshError;
        if (terrainGpuMeshBuilder->build(config, *heightField, gpuMesh, gpuMeshError))
        {
            try
            {
                outModel = Model::createFromGpuMesh(
                    *device,
                    gpuMesh.vertexBuffer, gpuMesh.vertexAllocation,
                    gpuMesh.vertexCount,
                    gpuMesh.indexBuffer, gpuMesh.indexAllocation,
                    gpuMesh.indexCount,
                    std::move(gpuMesh.subMeshes),
                    // F2-e: terrain picking uses the height field, so the CPU
                    // vertex mirror is never retained (saves ~38 KB per chunk at 33x33).
                    std::vector<glm::vec3>{},
                    std::vector<uint32_t>{},
                    gpuMesh.boundsMin, gpuMesh.boundsMax,
                    gpuMesh.uvMin, gpuMesh.uvMax, gpuMesh.uvValid);
            }
            catch (...)
            {
                if (gpuMesh.vertexBuffer != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device->getVmaAllocator(),
                                     gpuMesh.vertexBuffer,
                                     gpuMesh.vertexAllocation);
                if (gpuMesh.indexBuffer != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device->getVmaAllocator(),
                                     gpuMesh.indexBuffer,
                                     gpuMesh.indexAllocation);
                throw;
            }

            outVertexCount = gpuMesh.vertexCount;
            outIndexCount = gpuMesh.indexCount;
            outGenerationMs = gpuMesh.assemblyMs;
            if (outMeshGeneratedOnGpu)
                *outMeshGeneratedOnGpu = true;
            return true;
        }

        static bool reportedGpuMeshFallback = false;
        if (!reportedGpuMeshFallback)
        {
            reportedGpuMeshFallback = true;
            std::cout << "[Terrain] GPU mesh assembly failed; CPU fallback | "
                      << gpuMeshError << "\n";
        }
    }

    const auto meshStart = std::chrono::steady_clock::now();
    terrain::TerrainMeshData terrainMesh;
    if (!buildTerrainMeshFromHeightField(config, *heightField, terrainMesh, errorMessage))
        return false;
    const double meshGenerationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - meshStart).count();

    double uploadMs = 0.0;
    if (!uploadTerrainMesh(*device, commandPool, terrainMesh, outModel,
                           outVertexCount, outIndexCount, uploadMs, errorMessage))
        return false;

    outGenerationMs = meshGenerationMs + uploadMs;
    return true;
}

bool Scene::buildTerrainModel(const terrain::TerrainConfig& config,
                              std::shared_ptr<Model>& outModel,
                              uint32_t& outVertexCount,
                              uint32_t& outIndexCount,
                              double& outGenerationMs,
                              std::string& errorMessage,
                              std::shared_ptr<terrain::HeightField>* outHeightField,
                              bool* outMeshGeneratedOnGpu)
{
    outModel.reset();
    outVertexCount = 0;
    outIndexCount = 0;
    outGenerationMs = 0.0;
    errorMessage.clear();
    if (outMeshGeneratedOnGpu)
        *outMeshGeneratedOnGpu = false;
    if (outHeightField)
        outHeightField->reset();

    if (!device)
    {
        errorMessage = "scene is not initialized";
        return false;
    }

    std::shared_ptr<terrain::HeightField> heightField;
    double heightGenerationMs = 0.0;
    if (!generateTerrainHeightField(config, heightField, heightGenerationMs,
                                    errorMessage, nullptr,
                                    terrainComputeGenerator.get()))
        return false;

    double meshGenerationMs = 0.0;
    if (!buildTerrainModelFromHeightField(config, heightField, outModel,
                                          outVertexCount, outIndexCount,
                                          meshGenerationMs, errorMessage,
                                          outMeshGeneratedOnGpu))
        return false;

    outGenerationMs = heightGenerationMs + meshGenerationMs;
    if (outHeightField)
        *outHeightField = std::move(heightField);
    return true;
}

bool Scene::selfTestTerrainCompute(std::string& errorMessage)
{
    errorMessage.clear();
    if (!terrainComputeGenerator || !terrainComputeGenerator->isAvailable())
    {
        errorMessage = "GPU terrain compute generator is unavailable";
        return false;
    }

    terrain::TerrainConfig config;
    config.seed = 424242u;
    config.resolutionX = 33u;
    config.resolutionZ = 33u;
    config.sizeX = 40.0f;
    config.sizeZ = 36.0f;
    config.sampleOriginX = 137.5f;
    config.sampleOriginZ = -88.25f;
    config.heightScale = 8.0f;
    config.octaveCount = 4u;
    config.ridgeWeight = 0.35f;
    config.detailWeight = 0.22f;
    config.islandWeight = 0.6f;
    config.islandRadius = 0.55f;
    config.edgeFalloff = 0.4f;
    config.heightCurve = 1.2f;

    terrain::HeightField cpuField;
    if (!cpuField.build(config, errorMessage))
        return false;

    terrain::HeightField gpuField;
    if (!terrainComputeGenerator->generate(config, gpuField, errorMessage))
        return false;

    if (cpuField.width != gpuField.width || cpuField.height != gpuField.height ||
        cpuField.heights.size() != gpuField.heights.size())
    {
        errorMessage = "GPU heightfield dimensions mismatch";
        return false;
    }

    float maxDifference = 0.0f;
    for (size_t i = 0; i < cpuField.heights.size(); ++i)
        maxDifference = std::max(maxDifference,
            std::abs(cpuField.heights[i] - gpuField.heights[i]));

    if (maxDifference > 5e-4f)
    {
        errorMessage = "GPU/CPU heightfield mismatch: maxDiff=" +
            std::to_string(maxDifference);
        return false;
    }

    std::cout << "[Terrain] GPU compute heightfield selfTest PASS | grid="
              << config.resolutionX << "x" << config.resolutionZ
              << " maxDiff=" << maxDifference << "\n";

    if (!terrainGpuMeshBuilder || !terrainGpuMeshBuilder->isAvailable())
    {
        errorMessage = "GPU terrain mesh builder is unavailable";
        return false;
    }
    if (!terrainGpuMeshBuilder->selfTest(errorMessage))
        return false;
    return true;
}

bool Scene::ensureTerrainMaterialSet(uint32_t materialSetIndex,
                                     std::string& errorMessage)
{
    if (!terrainMaterial)
    {
        errorMessage = "terrain material library is not initialized";
        return false;
    }
    return terrainMaterial->ensureMaterialSet(materialSetIndex, errorMessage);
}

bool Scene::ensureTerrainMaterialSetFromDirectory(uint32_t materialSetIndex,
                                                  const std::string& directoryOverride,
                                                  std::string& errorMessage)
{
    if (!terrainMaterial)
    {
        errorMessage = "terrain material library is not initialized";
        return false;
    }
    return terrainMaterial->ensureMaterialSetFromDirectory(
        materialSetIndex, directoryOverride, errorMessage);
}

TerrainMaterial::Stats Scene::getTerrainMaterialStats() const
{
    return terrainMaterial ? terrainMaterial->getStats() : TerrainMaterial::Stats{};
}

entt::entity Scene::createTerrainEntity(const terrain::TerrainConfig& config,
                                        const std::string& name,
                                        const glm::vec3& spawnPosition)
{
    if (!device)
        return entt::null;

    std::string materialError;
    if (!ensureTerrainMaterialSet(config.materialSetIndex, materialError))
    {
        std::cerr << "[Scene] createTerrainEntity material set failed: "
                  << materialError << "\n";
        return entt::null;
    }

    std::shared_ptr<Model> model;
    std::shared_ptr<terrain::HeightField> heightField;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    double generationMs = 0.0;
    bool meshGeneratedOnGpu = false;
    std::string errorMessage;
    if (!buildTerrainModel(config, model, vertexCount, indexCount,
                           generationMs, errorMessage, &heightField,
                           &meshGeneratedOnGpu) || !model)
    {
        std::cerr << "[Scene] createTerrainEntity failed: " << errorMessage << "\n";
        return entt::null;
    }

    const entt::entity entity = createTransformEntity(registry);
    markTransformDirty(entity);
    registry.get<TransformComponent>(entity).setTranslation(spawnPosition);
    registry.emplace<NameComponent>(entity, NameComponent{ name });
    registry.emplace<MeshComponent>(entity, MeshComponent{ model });

    RenderTagComponent tag;
    tag.isTerrain = true;
    registry.emplace<RenderTagComponent>(entity, tag);

    TerrainComponent terrainComponent;
    terrainComponent.config = config;
    terrainComponent.generatedConfig = config;
    terrainComponent.revision = 1;
    terrainComponent.vertexCount = vertexCount;
    terrainComponent.indexCount = indexCount;
    terrainComponent.lastGenerationMs = generationMs;
    terrainComponent.generated = true;
    terrainComponent.meshGeneratedOnGpu = meshGeneratedOnGpu;
    terrainComponent.cpuHeightField = std::move(heightField);
    terrainComponent.generationBackend =
        (terrainComponent.cpuHeightField->generatedOnGpu
            ? (meshGeneratedOnGpu ? "GPU height + GPU mesh" : "GPU height")
            : (meshGeneratedOnGpu ? "GPU mesh" : "CPU fallback"));
    registry.emplace<TerrainComponent>(entity, std::move(terrainComponent));

    setModelBounds(registry, entity, model, &dirtySpatialEntities);
    attachDirtyTracking(entity);
    refreshTransformsAndVisibility();
    return entity;
}

entt::entity Scene::createStreamingTerrain(const terrain::TerrainConfig& chunkConfig,
                                            int32_t loadRadius,
                                            uint32_t maxLoadsPerFrame,
                                            const std::string& name)
{
    if (!device)
        return entt::null;

    terrain::TerrainConfig config = chunkConfig;
    config.chunkGridX = 1u;
    config.chunkGridZ = 1u;
    config.localColorRange = false;

    std::string validationError;
    if (!config.validate(validationError))
    {
        std::cerr << "[Scene] createStreamingTerrain failed: " << validationError << "\n";
        return entt::null;
    }

    std::string materialError;
    if (!ensureTerrainMaterialSet(config.materialSetIndex, materialError))
    {
        std::cerr << "[Scene] createStreamingTerrain material set failed: "
                  << materialError << "\n";
        return entt::null;
    }

    if (terrainStreamingEntity != entt::null && registry.valid(terrainStreamingEntity))
        destroyEntity(terrainStreamingEntity);

    const entt::entity manager = createTransformEntity(registry);
    attachDirtyTracking(manager);
    registry.emplace<NameComponent>(manager, NameComponent{ name });

    TerrainStreamingComponent streaming;
    streaming.chunkConfig = config;
    streaming.loadRadius = std::clamp<int32_t>(loadRadius, 1, kMaxTerrainStreamingRadius);
    streaming.unloadPadding = 1;
    streaming.maxLoadsPerFrame = std::clamp<uint32_t>(
        maxLoadsPerFrame, 1u, kMaxTerrainStreamingLoadsPerFrame);
    registry.emplace<TerrainStreamingComponent>(manager, streaming);

    terrainStreamingEntity = manager;
    lastStreamingUpdateMs = 0.0;
    maxStreamingUpdateMs = 0.0;
    refreshStreamingTerrain();
    return manager;
}

bool Scene::hasStreamingTerrain() const
{
    return terrainStreamingEntity != entt::null &&
           registry.valid(terrainStreamingEntity) &&
           registry.any_of<TerrainStreamingComponent>(terrainStreamingEntity);
}

uint32_t Scene::getInFlightTerrainGpuRequests() const
{
    return terrainComputeGenerator ? terrainComputeGenerator->inFlightCount() : 0u;
}

void Scene::queryTerrainChunksInRegionRange(int32_t minRegionX, int32_t minRegionZ,
                                            int32_t maxRegionX, int32_t maxRegionZ,
                                            std::vector<entt::entity>& outEntities) const
{
    spatialIndex.queryRegionRange(minRegionX, minRegionZ, maxRegionX, maxRegionZ,
                                  outEntities);
    outEntities.erase(
        std::remove_if(outEntities.begin(), outEntities.end(),
                       [this](entt::entity entity)
                       {
                           if (!registry.valid(entity))
                               return true;
                           const auto* chunk = registry.try_get<TerrainChunkComponent>(entity);
                           if (!chunk)
                               return true;
                           return chunk->world != entt::null &&
                                  terrainStreamingEntity != entt::null &&
                                  chunk->world != terrainStreamingEntity;
                       }),
        outEntities.end());
}

void Scene::queryTerrainChunksInRegion(int32_t regionX, int32_t regionZ,
                                       std::vector<entt::entity>& outEntities) const
{
    queryTerrainChunksInRegionRange(regionX, regionZ, regionX, regionZ, outEntities);
}

entt::entity Scene::resolveTerrainChunkOwner(entt::entity entity) const
{
    if (!registry.valid(entity))
        return entt::null;
    if (const auto* chunk = registry.try_get<TerrainChunkComponent>(entity))
    {
        if (chunk->world != entt::null && registry.valid(chunk->world))
            return chunk->world;
    }
    return entity;
}

bool Scene::adoptStreamingTerrainManager(entt::entity entity)
{
    if (!registry.valid(entity) ||
        !registry.any_of<TerrainStreamingComponent>(entity))
        return false;
    if (terrainStreamingEntity != entt::null &&
        terrainStreamingEntity != entity &&
        registry.valid(terrainStreamingEntity))
        return false;

    terrainStreamingEntity = entity;
    streamingTerrainChunks.clear();
    pendingStreamingTerrainChunks.clear();
    lastStreamingUpdateMs = 0.0;
    maxStreamingUpdateMs = 0.0;
    refreshStreamingTerrain();
    return true;
}

void Scene::setStreamingTerrainBudget(float milliseconds)
{
    if (terrainStreamingEntity == entt::null || !registry.valid(terrainStreamingEntity))
        return;
    if (TerrainStreamingComponent* streaming =
            registry.try_get<TerrainStreamingComponent>(terrainStreamingEntity))
        streaming->maxUpdateMs = std::max(0.0f, milliseconds);
}

void Scene::rebaseWorldOrigin(const glm::vec3& delta)
{
if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f)
return;

// 1) Everything in the registry stores local coordinates, so shifting all of
// them at once keeps the world looking identical.
auto transforms = registry.view<TransformComponent>();
for (entt::entity entity : transforms)
{
TransformComponent& transform = transforms.get<TransformComponent>(entity);
transform.setTranslation(transform.getTranslation() - delta);
markTransformDirty(entity);
}

// 2) The camera controller caches absolute smoothing state.
cameraController.offsetWorldPosition(delta);

// 3) Streamed chunks are keyed by integer chunk indices; the shift is snapped
// to whole chunks so the keys stay integral.
float chunkSizeX = 32.0f;
float chunkSizeZ = 32.0f;
if (terrainStreamingEntity != entt::null)
{
if (auto* streaming = registry.try_get<TerrainStreamingComponent>(terrainStreamingEntity))
{
if (streaming->chunkConfig.sizeX > 0.0f)
chunkSizeX = streaming->chunkConfig.sizeX;
if (streaming->chunkConfig.sizeZ > 0.0f)
chunkSizeZ = streaming->chunkConfig.sizeZ;
}
}
const int32_t chunkDeltaX = static_cast<int32_t>(std::llround(
static_cast<double>(delta.x) / static_cast<double>(chunkSizeX)));
const int32_t chunkDeltaZ = static_cast<int32_t>(std::llround(
static_cast<double>(delta.z) / static_cast<double>(chunkSizeZ)));
if (chunkDeltaX != 0 || chunkDeltaZ != 0)
{
std::unordered_map<int64_t, entt::entity> rekeyedChunks;
rekeyedChunks.reserve(streamingTerrainChunks.size());
for (const auto& [key, entity] : streamingTerrainChunks)
{
if (!registry.valid(entity))
continue;
int32_t chunkX = static_cast<int32_t>(static_cast<int64_t>(key >> 32)) - chunkDeltaX;
int32_t chunkZ = static_cast<int32_t>(static_cast<int64_t>(
static_cast<uint32_t>(key & 0xFFFFFFFFu))) - chunkDeltaZ;
if (TerrainChunkComponent* chunk = registry.try_get<TerrainChunkComponent>(entity))
{
chunk->chunkX -= chunkDeltaX;
chunk->chunkZ -= chunkDeltaZ;
chunkX = chunk->chunkX;
chunkZ = chunk->chunkZ;
}
rekeyedChunks[terrainChunkKey(chunkX, chunkZ)] = entity;
}
streamingTerrainChunks.swap(rekeyedChunks);

std::unordered_set<int64_t> rekeyedPending;
rekeyedPending.reserve(pendingStreamingTerrainChunks.size());
for (int64_t key : pendingStreamingTerrainChunks)
{
const int32_t chunkX = static_cast<int32_t>(static_cast<int64_t>(key >> 32)) - chunkDeltaX;
const int32_t chunkZ = static_cast<int32_t>(static_cast<int64_t>(
static_cast<uint32_t>(key & 0xFFFFFFFFu))) - chunkDeltaZ;
rekeyedPending.insert(terrainChunkKey(chunkX, chunkZ));
}
pendingStreamingTerrainChunks.swap(rekeyedPending);
}

// 4) The absolute origin accumulates in double; float only carries the local
// coordinates near the origin.
worldOrigin += glm::dvec3(delta);
++originRebaseCount;

std::cout << "[Origin] rebased | delta=(" << delta.x << ", " << delta.y << ", " << delta.z
<< ") origin=(" << worldOrigin.x << ", " << worldOrigin.y << ", " << worldOrigin.z
<< ") rebases=" << originRebaseCount << std::endl;
}

bool Scene::maybeRebaseWorldOrigin()
{
const glm::vec3 local = getCameraTransform().getTranslation();
const float horizontal = std::sqrt(local.x * local.x + local.z * local.z);
maxLocalCameraDistance = std::max(maxLocalCameraDistance, horizontal);

if (!engineSettings.originRebaseEnabled)
return false;

const float threshold = std::max(64.0f, engineSettings.originRebaseThreshold);
if (horizontal < threshold)
return false;

float stepX = 32.0f;
float stepZ = 32.0f;
if (terrainStreamingEntity != entt::null)
{
if (auto* streaming = registry.try_get<TerrainStreamingComponent>(terrainStreamingEntity))
{
if (streaming->chunkConfig.sizeX > 0.0f)
stepX = streaming->chunkConfig.sizeX;
if (streaming->chunkConfig.sizeZ > 0.0f)
stepZ = streaming->chunkConfig.sizeZ;
}
}

const float deltaX = std::floor(local.x / stepX) * stepX;
const float deltaZ = std::floor(local.z / stepZ) * stepZ;
if (deltaX == 0.0f && deltaZ == 0.0f)
return false;

rebaseWorldOrigin(glm::vec3(deltaX, 0.0f, deltaZ));
return true;
}

void Scene::refreshStreamingTerrain()
{
    const auto start = std::chrono::steady_clock::now();
    streamingLoadsLastFrame = 0;
    streamingUnloadsLastFrame = 0;
    updateTerrainStreaming();
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    lastStreamingUpdateMs = elapsedMs;
    maxStreamingUpdateMs = std::max(maxStreamingUpdateMs, elapsedMs);
}

void Scene::updateTerrainStreaming()
{
    if (terrainStreamingEntity == entt::null)
        return;
    if (!registry.valid(terrainStreamingEntity))
    {
        terrainStreamingEntity = entt::null;
        streamingTerrainChunks.clear();
        pendingStreamingTerrainChunks.clear();
        return;
    }

    const TerrainStreamingComponent* streaming =
        registry.try_get<TerrainStreamingComponent>(terrainStreamingEntity);
    if (!streaming || !streaming->enabled)
        return;

    const float chunkSizeX = streaming->chunkConfig.sizeX;
    const float chunkSizeZ = streaming->chunkConfig.sizeZ;
    if (chunkSizeX <= 0.0f || chunkSizeZ <= 0.0f)
        return;

    // B-06: keep the coarse region grid aligned with the streaming chunk grid.
    // setRegionSize() rebuilds existing buckets, so this is safe mid-stream.
    const float regionSize = std::max(chunkSizeX, chunkSizeZ);
    if (std::abs(spatialIndex.getRegionSize() - regionSize) > 1.0e-4f)
        spatialIndex.setRegionSize(regionSize);

    const glm::vec3 focus = getCameraTransform().getTranslation();
    const int32_t centerX = static_cast<int32_t>(std::floor(focus.x / chunkSizeX));
    const int32_t centerZ = static_cast<int32_t>(std::floor(focus.z / chunkSizeZ));
    const int32_t radius = std::clamp(streaming->loadRadius, 1,
                                      kMaxTerrainStreamingRadius);
    const int32_t unloadRadius = radius + std::max(0, streaming->unloadPadding);

    // Retire chunks beyond the hysteresis ring and notice completed/failed
    // asynchronous jobs. Destroying an entity also erases it from the map, so
    // defer those calls until this iteration has finished.
    std::vector<entt::entity> chunksToDestroy;
    for (auto it = streamingTerrainChunks.begin(); it != streamingTerrainChunks.end();)
    {
        const int64_t key = it->first;
        const entt::entity entity = it->second;
        if (!registry.valid(entity))
        {
            pendingStreamingTerrainChunks.erase(key);
            it = streamingTerrainChunks.erase(it);
            continue;
        }

        const int32_t x = static_cast<int32_t>(static_cast<uint32_t>(key >> 32));
        const int32_t z = static_cast<int32_t>(static_cast<uint32_t>(key));
        const int32_t distance = std::max(std::abs(x - centerX), std::abs(z - centerZ));
        if (distance > unloadRadius)
        {
            pendingStreamingTerrainChunks.erase(key);
            chunksToDestroy.push_back(entity);
            ++it;
            continue;
        }

        if (pendingStreamingTerrainChunks.erase(key) > 0u)
        {
            const TerrainComponent* terrain = registry.try_get<TerrainComponent>(entity);
            if (terrain && terrain->generating)
            {
                pendingStreamingTerrainChunks.insert(key);
            }
            else if (!terrain || !terrain->generated)
            {
                const std::string reason = terrain ? terrain->lastError
                                                   : std::string("missing TerrainComponent");
                std::cerr << "[Scene] streamed terrain chunk generation failed | key="
                          << x << "," << z << " | " << reason << "\n";
                chunksToDestroy.push_back(entity);
                ++it;
                continue;
            }
        }
        ++it;
    }
    for (entt::entity entity : chunksToDestroy)
        destroyEntity(entity);
    if (!chunksToDestroy.empty())
    {
        streamingUnloadsLastFrame += static_cast<uint32_t>(chunksToDestroy.size());
        streamingChunksUnloadedTotal += chunksToDestroy.size();
    }

    struct Candidate
    {
        int32_t x;
        int32_t z;
        int64_t key;
        float distance2;
    };
    std::vector<Candidate> candidates;
    const float focusChunkX = focus.x / chunkSizeX - 0.5f;
    const float focusChunkZ = focus.z / chunkSizeZ - 0.5f;
    for (int32_t dz = -radius; dz <= radius; ++dz)
    {
        for (int32_t dx = -radius; dx <= radius; ++dx)
        {
            const int32_t x = centerX + dx;
            const int32_t z = centerZ + dz;
            const int64_t key = terrainChunkKey(x, z);
            if (streamingTerrainChunks.find(key) != streamingTerrainChunks.end())
                continue;
            const float ox = static_cast<float>(x) - focusChunkX;
            const float oz = static_cast<float>(z) - focusChunkZ;
            candidates.push_back(Candidate{ x, z, key, ox * ox + oz * oz });
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            return a.distance2 < b.distance2;
        });

    uint32_t issued = 0;
    bool budgetExhausted = false;
    const auto budgetStart = std::chrono::steady_clock::now();
    for (const Candidate& candidate : candidates)
    {
        if (issued >= streaming->maxLoadsPerFrame)
            break;
        // F2-d: a single slow frame must not try to spawn the whole ring.
        if (issued > 0u && streaming->maxUpdateMs > 0.0f)
        {
            const double spentMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - budgetStart).count();
            if (spentMs >= static_cast<double>(streaming->maxUpdateMs))
            {
                budgetExhausted = true;
                break;
            }
        }

        terrain::TerrainConfig config = streaming->chunkConfig;
        config.chunkGridX = 1u;
        config.chunkGridZ = 1u;
        config.localColorRange = false;
        const float centerWorldX = (static_cast<float>(candidate.x) + 0.5f) * chunkSizeX;
        const float centerWorldZ = (static_cast<float>(candidate.z) + 0.5f) * chunkSizeZ;
        // F2-c: sampleOrigin is the absolute noise anchor, not a local
        // coordinate. Keeping it absolute means chunks generated after a
        // rebase still match their neighbours generated before it.
        config.sampleOriginX = static_cast<float>(worldOrigin.x + static_cast<double>(centerWorldX));
        config.sampleOriginZ = static_cast<float>(worldOrigin.z + static_cast<double>(centerWorldZ));

        std::string configError;
        if (!config.validate(configError))
        {
            std::cerr << "[Scene] streamed terrain config invalid | " << configError << "\n";
            continue;
        }

        const entt::entity entity = createTransformEntity(registry);
        attachDirtyTracking(entity);
        registry.get<TransformComponent>(entity).setTranslation(
            glm::vec3(centerWorldX, 0.0f, centerWorldZ));
        registry.emplace<NameComponent>(entity,
            NameComponent{ "TerrainChunk_" + std::to_string(candidate.x) + "_" +
                           std::to_string(candidate.z) });

        TerrainComponent terrain;
        terrain.config = config;
        terrain.generatedConfig = config;
        terrain.generationBackend = "pending";
        registry.emplace<TerrainComponent>(entity, std::move(terrain));
        RenderTagComponent tag;
        tag.isTerrain = true;
        registry.emplace<RenderTagComponent>(entity, tag);
        registry.emplace<TerrainChunkComponent>(entity,
            TerrainChunkComponent{ terrainStreamingEntity, candidate.x, candidate.z });
        markTransformDirty(entity);

        ++issued;
        if (!requestRegenerateTerrain(entity))
        {
            const auto* failed = registry.try_get<TerrainComponent>(entity);
            std::cerr << "[Scene] streamed terrain request failed | key="
                      << candidate.x << "," << candidate.z;
            if (failed)
                std::cerr << " | " << failed->lastError;
            std::cerr << "\n";
            destroyEntity(entity);
            continue;
        }

        streamingTerrainChunks.emplace(candidate.key, entity);
        ++streamingLoadsLastFrame;
        ++streamingChunksLoadedTotal;
        pendingStreamingTerrainChunks.insert(candidate.key);
    }
    if (budgetExhausted)
        ++streamingBudgetHits;
}

bool Scene::regenerateTerrain(entt::entity entity, std::string& errorMessage)
{
    if (!registry.valid(entity))
    {
        errorMessage = "invalid terrain entity";
        return false;
    }

    TerrainComponent* terrainComponent = registry.try_get<TerrainComponent>(entity);
    if (!terrainComponent)
    {
        errorMessage = "entity has no TerrainComponent";
        return false;
    }

    // Keep the previous mesh alive until every fallible operation has
    // succeeded. buildTerrainModel validates and uploads into a temporary.
    std::shared_ptr<Model> model;
    std::shared_ptr<terrain::HeightField> heightField;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    double generationMs = 0.0;
    bool meshGeneratedOnGpu = false;
    std::string buildError;
    if (!buildTerrainModel(terrainComponent->config, model, vertexCount,
                           indexCount, generationMs, buildError,
                           &heightField, &meshGeneratedOnGpu) || !model)
    {
        terrainComponent->lastError = buildError.empty()
            ? "terrain generation failed"
            : buildError;
        errorMessage = terrainComponent->lastError;
        return false;
    }

    terrainComponent->generatedConfig = terrainComponent->config;
    terrainComponent->revision += 1;
    terrainComponent->vertexCount = vertexCount;
    terrainComponent->indexCount = indexCount;
    terrainComponent->lastGenerationMs = generationMs;
    terrainComponent->lastError.clear();
    terrainComponent->generated = true;
    terrainComponent->meshGeneratedOnGpu = meshGeneratedOnGpu;
    terrainComponent->cpuHeightField = std::move(heightField);
    terrainComponent->generationBackend =
        (terrainComponent->cpuHeightField->generatedOnGpu
            ? (meshGeneratedOnGpu ? "GPU height + GPU mesh" : "GPU height")
            : (meshGeneratedOnGpu ? "GPU mesh" : "CPU fallback"));

    std::shared_ptr<Model> previousModel;
    if (const auto* previousMesh = registry.try_get<MeshComponent>(entity))
        previousModel = previousMesh->model;

    registry.emplace_or_replace<MeshComponent>(entity, MeshComponent{ model });
    retireModel(previousModel);

    RenderTagComponent* tag = registry.try_get<RenderTagComponent>(entity);
    if (!tag)
        tag = &registry.emplace<RenderTagComponent>(entity);
    tag->isTerrain = true;

    setModelBounds(registry, entity, model, &dirtySpatialEntities);
    attachDirtyTracking(entity);
    refreshTransformsAndVisibility();
    errorMessage.clear();
    return true;
}

entt::entity Scene::createCharacterEntity(const std::string& name,
                                              const glm::vec3& spawnPosition)
{
    const entt::entity entity = createTransformEntity(registry);
    attachDirtyTracking(entity);
    auto& transform = registry.get<TransformComponent>(entity);
    transform.setTranslation(spawnPosition);
    registry.emplace<NameComponent>(entity, NameComponent{ name });
    registry.emplace<CharacterControllerComponent>(entity,
                                                   CharacterControllerComponent{});
    markTransformDirty(entity);
    refreshTransformsAndVisibility();
    return entity;
}

bool Scene::queryGroundAtWorld(float worldX, float worldZ, float& outHeight,
                               entt::entity& outEntity,
                               float* outSlopeDegrees) const
{
    outHeight = 0.0f;
    outEntity = entt::null;
    if (outSlopeDegrees)
        *outSlopeDegrees = 0.0f;
    if (!std::isfinite(worldX) || !std::isfinite(worldZ))
        return false;

    bool found = false;
    for (auto [entity, terrain, world] :
         registry.view<TerrainComponent, WorldTransformComponent>().each())
    {
        if (!terrain.generated || terrain.generating || !terrain.cpuHeightField ||
            !world.valid)
            continue;

        const terrain::HeightField& field = *terrain.cpuHeightField;
        if (field.width < 2u || field.height < 2u ||
            field.sizeX <= 0.0f || field.sizeZ <= 0.0f)
            continue;

        const glm::mat4 inverseWorld = glm::inverse(world.matrix);
        const glm::vec3 local = glm::vec3(inverseWorld *
            glm::vec4(worldX, 0.0f, worldZ, 1.0f));
        const float halfX = field.sizeX * 0.5f;
        const float halfZ = field.sizeZ * 0.5f;
        if (!std::isfinite(local.x) || !std::isfinite(local.z) ||
            local.x < -halfX || local.x > halfX ||
            local.z < -halfZ || local.z > halfZ)
            continue;

        const float localHeight = field.worldHeightAt(local.x, local.z);
        const glm::vec3 worldPoint = glm::vec3(world.matrix *
            glm::vec4(local.x, localHeight, local.z, 1.0f));
        if (!std::isfinite(worldPoint.y))
            continue;
        if (!found || worldPoint.y > outHeight)
        {
            found = true;
            outHeight = worldPoint.y;
            outEntity = entity;
        }
    }

    if (found && outSlopeDegrees)
    {
        const float epsilon = 0.5f;
        float hLeft = 0.0f, hRight = 0.0f, hBack = 0.0f, hFront = 0.0f;
        entt::entity ignored = entt::null;
        if (queryGroundAtWorld(worldX - epsilon, worldZ, hLeft, ignored, nullptr) &&
            queryGroundAtWorld(worldX + epsilon, worldZ, hRight, ignored, nullptr) &&
            queryGroundAtWorld(worldX, worldZ - epsilon, hBack, ignored, nullptr) &&
            queryGroundAtWorld(worldX, worldZ + epsilon, hFront, ignored, nullptr))
        {
            const float slopeX = (hRight - hLeft) / (2.0f * epsilon);
            const float slopeZ = (hFront - hBack) / (2.0f * epsilon);
            const float slopeRadians = std::atan(std::sqrt(slopeX * slopeX +
                                                           slopeZ * slopeZ));
            *outSlopeDegrees = glm::degrees(slopeRadians);
        }
    }
    return found;
}

bool Scene::sampleTerrainGroundHeightWorld(float worldX, float worldZ,
                                           float& outHeight,
                                           entt::entity* outEntity) const
{
    entt::entity entity = entt::null;
    const bool found = queryGroundAtWorld(worldX, worldZ, outHeight, entity, nullptr);
    if (outEntity)
        *outEntity = entity;
    return found;
}

bool Scene::updateCharacterController(entt::entity entity,
                                      const glm::vec3& worldMoveDirection,
                                      bool jump,
                                      float dt,
                                      CharacterMoveResult& outResult)
{
    outResult = {};
    if (!registry.valid(entity) || !std::isfinite(dt) || dt <= 0.0f)
        return false;

    auto* controller = registry.try_get<CharacterControllerComponent>(entity);
    auto* transform = registry.try_get<TransformComponent>(entity);
    if (!controller || !transform)
        return false;

    dt = std::min(dt, 0.1f);
    glm::vec3 position = transform->getTranslation();

    float groundHeight = 0.0f;
    entt::entity groundEntity = entt::null;
    const bool hasGround = queryGroundAtWorld(position.x, position.z,
                                              groundHeight, groundEntity, nullptr);

    glm::vec3 move = glm::vec3(worldMoveDirection.x, 0.0f, worldMoveDirection.z);
    if (glm::dot(move, move) > 1.0e-8f)
        move = glm::normalize(move);
    else
        move = glm::vec3(0.0f);

    const bool jumped = jump && controller->grounded;
    if (jumped)
    {
        controller->verticalVelocity = std::max(0.0f, controller->jumpSpeed);
        controller->grounded = false;
    }

    const float targetX = position.x + move.x * controller->moveSpeed * dt;
    const float targetZ = position.z + move.z * controller->moveSpeed * dt;

    float targetGroundHeight = 0.0f;
    entt::entity targetGroundEntity = entt::null;
    const bool hasTargetGround = queryGroundAtWorld(targetX, targetZ,
                                                    targetGroundHeight,
                                                    targetGroundEntity, nullptr);

    bool blocked = false;
    if (controller->grounded && hasGround && hasTargetGround &&
        glm::dot(move, move) > 1.0e-8f)
    {
        const float stepUp = targetGroundHeight - groundHeight;
        if (stepUp > controller->stepHeight)
        {
            blocked = true;
        }
        else if (stepUp > 1.0e-4f)
        {
            float slopeDegrees = 0.0f;
            entt::entity ignored = entt::null;
            queryGroundAtWorld(targetX, targetZ, targetGroundHeight,
                               ignored, &slopeDegrees);
            if (slopeDegrees > controller->maxSlopeAngle)
                blocked = true;
        }
    }

    if (!blocked && glm::dot(move, move) > 1.0e-8f)
    {
        position.x = targetX;
        position.z = targetZ;
    }

    // Re-sample after horizontal movement. Ground snapping keeps the feet on
    // the surface for small downhill steps, while larger drops become airborne.
    float currentGround = groundHeight;
    entt::entity currentGroundEntity = groundEntity;
    const bool hasCurrentGround = queryGroundAtWorld(position.x, position.z,
                                                     currentGround,
                                                     currentGroundEntity, nullptr);

    if (!jumped && controller->grounded)
    {
        if (hasCurrentGround)
        {
            const float groundDelta = currentGround - position.y;
            if (groundDelta >= -controller->groundSnapDistance &&
                groundDelta <= controller->stepHeight)
            {
                position.y = currentGround;
                controller->verticalVelocity = 0.0f;
            }
            else if (groundDelta < -controller->groundSnapDistance)
            {
                controller->grounded = false;
            }
        }
        else
        {
            controller->grounded = false;
        }
    }

    if (controller->grounded)
    {
        controller->verticalVelocity = 0.0f;
        if (hasCurrentGround)
            position.y = currentGround;
    }
    else
    {
        controller->verticalVelocity -= controller->gravity * dt;
        position.y += controller->verticalVelocity * dt;
        if (hasCurrentGround && position.y <= currentGround)
        {
            position.y = currentGround;
            controller->verticalVelocity = 0.0f;
            controller->grounded = true;
        }
    }

    if (hasCurrentGround)
    {
        controller->groundEntity = currentGroundEntity;
    }

    transform->setTranslation(position);
    outResult.grounded = controller->grounded;
    outResult.blocked = blocked;
    outResult.groundHeight = currentGround;
    outResult.groundEntity = currentGroundEntity;
    outResult.position = position;
    return true;
}

bool Scene::sculptTerrain(const terrain::TerrainBrushSettings& brush,
                          TerrainSculptResult& outResult,
                          std::string& errorMessage)
{
    outResult = {};
    errorMessage.clear();
    if (!std::isfinite(brush.center.x) || !std::isfinite(brush.center.z) ||
        !std::isfinite(brush.radius) || brush.radius <= 0.0f ||
        !std::isfinite(brush.strength))
    {
        errorMessage = "terrain brush is invalid";
        return false;
    }

    refreshTransformsAndVisibility();

    std::vector<entt::entity> candidates;
    const auto addCandidate = [&](entt::entity entity)
    {
        if (registry.valid(entity) &&
            registry.any_of<TerrainComponent>(entity) &&
            std::find(candidates.begin(), candidates.end(), entity) == candidates.end())
            candidates.push_back(entity);
    };

    if (terrainStreamingEntity != entt::null &&
        registry.valid(terrainStreamingEntity))
    {
        const float regionSize = spatialIndex.getRegionSize();
        if (regionSize > 0.0f)
        {
            const int32_t minX = static_cast<int32_t>(std::floor(
                (brush.center.x - brush.radius) / regionSize));
            const int32_t maxX = static_cast<int32_t>(std::floor(
                (brush.center.x + brush.radius) / regionSize));
            const int32_t minZ = static_cast<int32_t>(std::floor(
                (brush.center.z - brush.radius) / regionSize));
            const int32_t maxZ = static_cast<int32_t>(std::floor(
                (brush.center.z + brush.radius) / regionSize));
            std::vector<entt::entity> chunks;
            queryTerrainChunksInRegionRange(minX, minZ, maxX, maxZ, chunks);
            for (entt::entity chunk : chunks)
                addCandidate(chunk);
        }
    }

    // Manual terrain (and any loaded chunks outside the streaming manager) is
    // filtered by world bounds so a brush cannot touch unrelated terrain.
    for (auto [entity, terrain, worldBounds] :
         registry.view<TerrainComponent, WorldBoundsComponent>().each())
    {
        (void)terrain;
        if (!worldBounds.world.valid)
            continue;
        const AABB& bounds = worldBounds.world;
        if (brush.center.x + brush.radius < bounds.min.x ||
            brush.center.x - brush.radius > bounds.max.x ||
            brush.center.z + brush.radius < bounds.min.z ||
            brush.center.z - brush.radius > bounds.max.z)
            continue;
        addCandidate(entity);
    }

    struct Commit
    {
        entt::entity entity = entt::null;
        std::shared_ptr<terrain::HeightField> heightField;
        std::shared_ptr<Model> model;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t modifiedVertices = 0;
        double generationMs = 0.0;
        bool meshGeneratedOnGpu = false;
    };
    std::vector<Commit> commits;

    for (entt::entity entity : candidates)
    {
        TerrainComponent* terrain = registry.try_get<TerrainComponent>(entity);
        const WorldTransformComponent* worldTransform =
            registry.try_get<WorldTransformComponent>(entity);
        if (!terrain || !terrain->generated || terrain->generating ||
            !terrain->cpuHeightField || !worldTransform || !worldTransform->valid)
            continue;

        auto nextHeightField = std::make_shared<terrain::HeightField>(
            *terrain->cpuHeightField);
        terrain::TerrainBrushResult brushResult;
        std::string brushError;
        if (!terrain::applyTerrainBrush(*nextHeightField, worldTransform->matrix,
                                        brush, brushResult, brushError))
        {
            errorMessage = "terrain brush failed: " + brushError;
            return false;
        }
        if (brushResult.modifiedVertices == 0u)
            continue;

        Commit commit;
        commit.entity = entity;
        commit.heightField = std::move(nextHeightField);
        commit.modifiedVertices = brushResult.modifiedVertices;
        std::string buildError;
        if (!buildTerrainModelFromHeightField(terrain->config, commit.heightField,
                                              commit.model, commit.vertexCount,
                                              commit.indexCount, commit.generationMs,
                                              buildError, &commit.meshGeneratedOnGpu) ||
            !commit.model)
        {
            errorMessage = "terrain sculpt rebuild failed: " +
                (buildError.empty() ? std::string("model is empty") : buildError);
            return false;
        }
        commits.push_back(std::move(commit));
    }

    for (Commit& commit : commits)
    {
        TerrainComponent* terrain = registry.try_get<TerrainComponent>(commit.entity);
        if (!terrain || !registry.valid(commit.entity))
            continue;

        std::shared_ptr<Model> previousModel;
        if (const auto* previousMesh = registry.try_get<MeshComponent>(commit.entity))
            previousModel = previousMesh->model;

        terrain->generatedConfig = terrain->config;
        terrain->revision += 1u;
        terrain->vertexCount = commit.vertexCount;
        terrain->indexCount = commit.indexCount;
        terrain->lastGenerationMs = commit.generationMs;
        terrain->lastError.clear();
        terrain->generated = true;
        terrain->meshGeneratedOnGpu = commit.meshGeneratedOnGpu;
        terrain->cpuHeightField = commit.heightField;
        terrain->generationBackend = commit.meshGeneratedOnGpu
            ? "GPU mesh (sculpted)" : "CPU fallback (sculpted)";

        registry.emplace_or_replace<MeshComponent>(
            commit.entity, MeshComponent{ commit.model });
        retireModel(previousModel);
        RenderTagComponent* tag = registry.try_get<RenderTagComponent>(commit.entity);
        if (!tag)
            tag = &registry.emplace<RenderTagComponent>(commit.entity);
        tag->isTerrain = true;
        setModelBounds(registry, commit.entity, commit.model, &dirtySpatialEntities);

        outResult.modifiedVertices += commit.modifiedVertices;
        outResult.rebuiltEntities += 1u;
        outResult.rebuilt.push_back(commit.entity);
    }

    if (!commits.empty())
        refreshTransformsAndVisibility();
    return true;
}

bool Scene::sampleTerrainHeightLocal(entt::entity entity, float localX, float localZ,
                                     float& outHeight) const
{
    outHeight = 0.0f;
    if (!registry.valid(entity) || !std::isfinite(localX) || !std::isfinite(localZ))
        return false;

    const TerrainComponent* terrain = registry.try_get<TerrainComponent>(entity);
    if (!terrain || !terrain->cpuHeightField || !terrain->generated)
        return false;

    const terrain::HeightField& heightField = *terrain->cpuHeightField;
    if (heightField.width < 2u || heightField.height < 2u ||
        heightField.sizeX <= 0.0f || heightField.sizeZ <= 0.0f)
        return false;

    const float halfX = heightField.sizeX * 0.5f;
    const float halfZ = heightField.sizeZ * 0.5f;
    if (localX < -halfX || localX > halfX || localZ < -halfZ || localZ > halfZ)
        return false;

    outHeight = heightField.worldHeightAt(localX, localZ);
    return std::isfinite(outHeight);
}

bool Scene::sampleTerrainHeightWorld(entt::entity entity, float worldX, float worldZ,
                                     float& outHeight) const
{
    outHeight = 0.0f;
    if (!registry.valid(entity) || !std::isfinite(worldX) || !std::isfinite(worldZ))
        return false;

    const WorldBoundsComponent* worldBounds =
        registry.try_get<WorldBoundsComponent>(entity);
    if (!worldBounds || !worldBounds->world.valid)
        return false;

    const float topY = worldBounds->world.max.y;
    const float height = std::max(1.0f, worldBounds->world.max.y -
                                         worldBounds->world.min.y);
    const glm::vec3 origin(worldX, topY + height * 0.5f + 1.0f, worldZ);
    glm::vec3 hitPoint{};
    float distance = 0.0f;
    if (!raycastTerrain(entity, origin, glm::vec3(0.0f, -1.0f, 0.0f),
                        hitPoint, distance))
        return false;

    outHeight = hitPoint.y;
    return std::isfinite(outHeight);
}

// F2-e: terrain picking no longer depends on a CPU copy of the mesh. The
// height field is the authoritative surface, so march the ray against it in
// the chunk's local frame and refine the crossing with a few bisections.
static bool raycastHeightField(const terrain::HeightField& field,
                               const glm::mat4& worldMatrix,
                               const glm::vec3& rayOrigin,
                               const glm::vec3& direction,
                               float maxDistance,
                               glm::vec3& outHitPoint, float& outDistance)
{
    if (field.width < 2u || field.height < 2u || field.sizeX <= 0.0f ||
        field.sizeZ <= 0.0f)
        return false;

    const glm::mat4 inverseWorld = glm::inverse(worldMatrix);
    const glm::vec3 localOrigin = glm::vec3(inverseWorld * glm::vec4(rayOrigin, 1.0f));
    const glm::vec3 localDirRaw = glm::vec3(inverseWorld * glm::vec4(direction, 0.0f));
    const float dirScale = glm::length(localDirRaw);
    if (!std::isfinite(dirScale) || dirScale < 1.0e-8f)
        return false;
    const glm::vec3 localDir = localDirRaw / dirScale;
    const float localMax = maxDistance * dirScale;

    const float halfX = field.sizeX * 0.5f;
    const float halfZ = field.sizeZ * 0.5f;
    float sEnter = 0.0f;
    float sExit = localMax;
    const float axes[2][3] = {
        { localOrigin.x, localDir.x, halfX },
        { localOrigin.z, localDir.z, halfZ },
    };
    for (const auto& axis : axes)
    {
        if (std::abs(axis[1]) < 1.0e-8f)
        {
            if (axis[0] < -axis[2] || axis[0] > axis[2])
                return false;
            continue;
        }
        float t0 = (-axis[2] - axis[0]) / axis[1];
        float t1 = (axis[2] - axis[0]) / axis[1];
        if (t0 > t1)
            std::swap(t0, t1);
        sEnter = std::max(sEnter, t0);
        sExit = std::min(sExit, t1);
        if (sEnter > sExit)
            return false;
    }

    const float cellX = field.sizeX / static_cast<float>(field.width - 1u);
    const float cellZ = field.sizeZ / static_cast<float>(field.height - 1u);
    const float step = std::max(0.25f, std::min(cellX, cellZ));

    auto sampleDiff = [&field](const glm::vec3& local)
    {
        return local.y - field.worldHeightAt(local.x, local.z);
    };

    float previousS = sEnter;
    float previousDiff = sampleDiff(localOrigin + localDir * previousS);
    float hitS = -1.0f;
    float s = sEnter;
    while (s < sExit)
    {
        const float nextS = std::min(s + step, sExit);
        const float diff = sampleDiff(localOrigin + localDir * nextS);
        if (previousDiff > 0.0f && diff <= 0.0f)
        {
            float lo = previousS;
            float hi = nextS;
            for (int i = 0; i < 16; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                if (sampleDiff(localOrigin + localDir * mid) > 0.0f)
                    lo = mid;
                else
                    hi = mid;
            }
            hitS = hi;
            break;
        }
        previousS = nextS;
        previousDiff = diff;
        s = nextS;
    }
    if (hitS < 0.0f)
        return false;

    const glm::vec3 localHit = localOrigin + localDir * hitS;
    const glm::vec3 worldHit = glm::vec3(worldMatrix * glm::vec4(localHit, 1.0f));
    const float distance = glm::distance(rayOrigin, worldHit);
    if (!std::isfinite(distance) || distance <= 0.0f)
        return false;

    outHitPoint = worldHit;
    outDistance = distance;
    return true;
}

bool Scene::raycastTerrain(entt::entity entity, const glm::vec3& rayOrigin,
                           const glm::vec3& rayDirection, glm::vec3& outHitPoint,
                           float& outDistance) const
{
    outHitPoint = glm::vec3(0.0f);
    outDistance = 0.0f;
    if (!registry.valid(entity))
        return false;

    const TerrainComponent* terrain = registry.try_get<TerrainComponent>(entity);
    const WorldTransformComponent* worldTransform =
        registry.try_get<WorldTransformComponent>(entity);
    if (!terrain || !terrain->generated || !worldTransform || !worldTransform->valid)
        return false;

    const float directionLength = glm::length(rayDirection);
    if (!std::isfinite(directionLength) || directionLength < 1.0e-8f)
        return false;
    const glm::vec3 direction = rayDirection / directionLength;

    // The height field is the authoritative surface. Terrain meshes no longer
    // keep a CPU vertex mirror, so there is nothing to fall back to.
    if (terrain->cpuHeightField &&
        raycastHeightField(*terrain->cpuHeightField, worldTransform->matrix,
                           rayOrigin, direction, 5000.0f, outHitPoint, outDistance))
        return true;

    return false;
}

bool Scene::raycastTerrainWorld(const glm::vec3& rayOrigin,
                                const glm::vec3& rayDirection,
                                glm::vec3& outHitPoint,
                                entt::entity& outEntity,
                                float& outDistance) const
{
    outHitPoint = glm::vec3(0.0f);
    outEntity = entt::null;
    outDistance = std::numeric_limits<float>::max();

    const float directionLength = glm::length(rayDirection);
    if (!std::isfinite(directionLength) || directionLength < 1.0e-8f)
    {
        outDistance = 0.0f;
        return false;
    }
    const glm::vec3 direction = rayDirection / directionLength;

    bool found = false;
    for (auto [entity, terrain] : registry.view<TerrainComponent>().each())
    {
        if (!terrain.generated || terrain.generating)
            continue;

        glm::vec3 hitPoint{};
        float distance = 0.0f;
        if (!raycastTerrain(entity, rayOrigin, direction, hitPoint, distance))
            continue;
        if (!found || distance < outDistance)
        {
            found = true;
            outDistance = distance;
            outHitPoint = hitPoint;
            outEntity = entity;
        }
    }

    if (!found)
    {
        outDistance = 0.0f;
        return false;
    }
    return std::isfinite(outDistance) && std::isfinite(outHitPoint.x) &&
           std::isfinite(outHitPoint.y) && std::isfinite(outHitPoint.z);
}

bool Scene::requestRegenerateTerrain(entt::entity entity,
                                       TerrainGenerationCallback onComplete)
{
    if (!jobSystem)
    {
        if (auto* terrain = registry.try_get<TerrainComponent>(entity))
            terrain->lastError = "async terrain generation unavailable (JobSystem missing)";
        return false;
    }
    if (!registry.valid(entity))
        return false;

    TerrainComponent* terrain = registry.try_get<TerrainComponent>(entity);
    if (!terrain)
        return false;

    // Supersede existing jobs for this entity with cooperative cancellation.
    for (auto& pending : pendingTerrainBuilds)
    {
        if (pending->entity != entity)
            continue;
        pending->cancelRequested.store(true, std::memory_order_relaxed);
        if (pending->gpuMeshRequest != 0u && terrainGpuMeshBuilder)
        {
            terrainGpuMeshBuilder->release(pending->gpuMeshRequest);
            pending->gpuMeshRequest = 0u;
        }
        if (pending->gpuHeightRequest != 0u && terrainComputeGenerator)
        {
            terrainComputeGenerator->release(pending->gpuHeightRequest);
            pending->gpuHeightRequest = 0u;
            pending->failed = true;
            pending->error = "terrain request superseded";
        }
    }

    const terrain::TerrainConfig requestConfig = terrain->config;
    const uint64_t requestId = ++nextTerrainRequestSerial;

    auto task = std::make_shared<PendingTerrainBuild>();
    task->entity = entity;
    task->requestId = requestId;
    task->config = requestConfig;
    task->completion = std::move(onComplete);

    bool gpuHeightSubmitted = false;
    std::string gpuSubmitError;
    if (terrainGpuHeightEnabled && terrainComputeGenerator && terrainComputeGenerator->isAvailable())
    {
        task->heightStartTime = std::chrono::steady_clock::now();
        task->gpuHeightRequest = terrainComputeGenerator->submit(
            requestConfig, gpuSubmitError);
        gpuHeightSubmitted = task->gpuHeightRequest != 0u;
    }
    task->cpuHeightFallback = !gpuHeightSubmitted;
    if (!gpuHeightSubmitted && !gpuSubmitError.empty())
    {
        static bool reportedGpuAsyncFallback = false;
        if (!reportedGpuAsyncFallback)
        {
            reportedGpuAsyncFallback = true;
            std::cout << "[Terrain] async GPU heightfield unavailable; CPU fallback | "
                      << gpuSubmitError << "\n";
        }
    }

    terrain->generating = true;
    terrain->lastError.clear();

    PendingTerrainBuild* raw = task.get();
    pendingTerrainBuilds.push_back(task);
    latestTerrainRequest[entity] = requestId;

    if (gpuHeightSubmitted)
        return true;

    try
    {
        raw->handle = jobSystem->submit([raw]
        {
            try
            {
                raw->cpuSucceeded = generateTerrainMeshCpu(
                    raw->config, raw->mesh, raw->cpuGenerationMs, raw->error,
                    &raw->cancelRequested, &raw->heightField, nullptr);
            }
            catch (const std::exception& ex)
            {
                raw->cpuSucceeded = false;
                raw->error = std::string("terrain CPU job failed: ") + ex.what();
            }
            catch (...)
            {
                raw->cpuSucceeded = false;
                raw->error = "terrain CPU job failed with unknown exception";
            }
        });
    }
    catch (const std::exception& ex)
    {
        pendingTerrainBuilds.pop_back();
        latestTerrainRequest.erase(entity);
        terrain->generating = false;
        terrain->lastError = std::string("terrain job submission failed: ") + ex.what();
        return false;
    }

    return true;
}

void Scene::recordTerrainPollMs(double ms)
{
    ++terrainPollCount;
    maxTerrainPollMs = std::max(maxTerrainPollMs, ms);
}

TerrainGpuMeshBuilder::Stats Scene::getTerrainGpuMeshStats() const
{
    return terrainGpuMeshBuilder ? terrainGpuMeshBuilder->getStats()
                                 : TerrainGpuMeshBuilder::Stats{};
}

void Scene::retireTerrainBuild(std::shared_ptr<PendingTerrainBuild> task)
{
    if (!task)
        return;
    if (task->handle.valid())
    {
        // worker 仍可能在写 task 的成员，推迟到句柄完成后释放。
        retiringTerrainBuilds.push_back(std::move(task));
        return;
    }
    // 没有 CPU 作业持有该任务，可以直接释放。
}

void Scene::drainRetiringTerrainBuilds()
{
    for (auto it = retiringTerrainBuilds.begin(); it != retiringTerrainBuilds.end();)
    {
        const bool jobDone = !(*it)->handle.valid() || (*it)->handle.isComplete();
        if (!jobDone)
        {
            ++it;
            continue;
        }
        if ((*it)->handle.valid())
        {
            try
            {
                (*it)->handle.wait();
            }
            catch (...)
            {
                // 作业异常已写进 task->error；这里只负责回收内存。
            }
        }
        it = retiringTerrainBuilds.erase(it);
    }
}

void Scene::processTerrainGeneration()
{
    if (terrainComputeGenerator)
        terrainComputeGenerator->collectFinished();
    if (terrainGpuMeshBuilder)
        terrainGpuMeshBuilder->collectFinished();
    drainRetiringTerrainBuilds();
    if (pendingTerrainBuilds.empty())
        return;

    for (auto it = pendingTerrainBuilds.begin(); it != pendingTerrainBuilds.end();)
    {
        PendingTerrainBuild* task = it->get();

        const auto latestIt = latestTerrainRequest.find(task->entity);
        const bool isLatest = latestIt != latestTerrainRequest.end() &&
                              latestIt->second == task->requestId;
        if (!isLatest)
        {
            if (task->gpuHeightRequest != 0u && terrainComputeGenerator)
                terrainComputeGenerator->release(task->gpuHeightRequest);
            if (task->gpuMeshRequest != 0u && terrainGpuMeshBuilder)
                terrainGpuMeshBuilder->release(task->gpuMeshRequest);
            // 被更新请求取代：CPU 作业可能还在跑，必须延迟释放。
            task->cancelRequested.store(true, std::memory_order_relaxed);
            std::shared_ptr<PendingTerrainBuild> retired = std::move(*it);
            it = pendingTerrainBuilds.erase(it);
            retireTerrainBuild(std::move(retired));
            continue;
        }

        TerrainComponent* terrain = registry.valid(task->entity)
            ? registry.try_get<TerrainComponent>(task->entity)
            : nullptr;
        if (!terrain)
        {
            if (task->gpuHeightRequest != 0u && terrainComputeGenerator)
                terrainComputeGenerator->release(task->gpuHeightRequest);
            if (task->gpuMeshRequest != 0u && terrainGpuMeshBuilder)
                terrainGpuMeshBuilder->release(task->gpuMeshRequest);
            latestTerrainRequest.erase(task->entity);
            // 实体已销毁：worker 可能仍在写 task，延迟释放。
            task->cancelRequested.store(true, std::memory_order_relaxed);
            std::shared_ptr<PendingTerrainBuild> retired = std::move(*it);
            it = pendingTerrainBuilds.erase(it);
            retireTerrainBuild(std::move(retired));
            continue;
        }

        // Stage 1: asynchronous GPU heightfield. Poll only; never wait here.
        if (task->gpuHeightRequest != 0u)
        {
            terrain::HeightField heightField;
            std::string pollError;
            const auto status = terrainComputeGenerator
                ? terrainComputeGenerator->poll(task->gpuHeightRequest,
                                                heightField, pollError)
                : TerrainComputeGenerator::PollStatus::Failed;
            if (status == TerrainComputeGenerator::PollStatus::Pending)
            {
                ++it;
                continue;
            }

            task->gpuHeightRequest = 0u;
            if (status == TerrainComputeGenerator::PollStatus::Completed)
            {
                task->heightField = std::make_shared<terrain::HeightField>(
                    std::move(heightField));
                task->heightGenerationMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - task->heightStartTime).count();
            }
            else
            {
                task->failed = true;
                task->error = pollError.empty()
                    ? "GPU terrain heightfield generation failed"
                    : pollError;
            }
        }

        // Stage 2a: poll an in-flight asynchronous GPU mesh request. The
        // submit path never waits; this stage only observes the fence.
        if (!task->failed && task->gpuMeshRequest != 0u)
        {
            std::string pollError;
            const auto status = terrainGpuMeshBuilder
                ? terrainGpuMeshBuilder->poll(task->gpuMeshRequest,
                                              task->gpuMesh, pollError)
                : TerrainGpuMeshBuilder::PollStatus::Failed;
            if (status == TerrainGpuMeshBuilder::PollStatus::Pending)
            {
                ++it;
                continue;
            }

            task->gpuMeshRequest = 0u;
            if (status == TerrainGpuMeshBuilder::PollStatus::Completed)
            {
                task->gpuMeshReady = true;
                task->meshSubmitted = true;
                task->cpuGenerationMs = task->heightGenerationMs +
                    task->gpuMesh.assemblyMs;
            }
            else
            {
                static bool reportedAsyncGpuMeshFallback = false;
                if (!reportedAsyncGpuMeshFallback)
                {
                    reportedAsyncGpuMeshFallback = true;
                    std::cout << "[Terrain] async GPU mesh assembly failed; CPU fallback | "
                              << pollError << "\n";
                }
            }
        }

        // Stage 2b: submit mesh assembly once a heightfield is available. If
        // all async slots are busy, defer to a later frame rather than falling
        // back to a synchronous main-thread build.
        if (!task->failed && !task->cpuHeightFallback &&
            !task->meshSubmitted && !task->gpuMeshReady &&
            !task->gpuMeshAttempted)
        {
            if (!task->heightField)
            {
                task->failed = true;
                task->error = "GPU terrain heightfield is missing";
            }
            else if (terrainGpuMeshBuilder &&
                     terrainGpuMeshBuilder->supports(task->config))
            {
                if (!terrainGpuMeshBuilder->hasFreeSlot())
                {
                    // Keep the task pending; poll/collect frees a slot later.
                    ++it;
                    continue;
                }

                task->gpuMeshAttempted = true;
                std::string gpuMeshError;
                task->gpuMeshRequest = terrainGpuMeshBuilder->submit(
                    task->config, *task->heightField, gpuMeshError);
                if (task->gpuMeshRequest != 0u)
                {
                    ++it;
                    continue;
                }

                static bool reportedAsyncGpuMeshFallback = false;
                if (!reportedAsyncGpuMeshFallback)
                {
                    reportedAsyncGpuMeshFallback = true;
                    std::cout << "[Terrain] async GPU mesh submission failed; CPU fallback | "
                              << gpuMeshError << "\n";
                }
            }
        }

        // Stage 2c: CPU mesh fallback when GPU assembly is unavailable or the
        // asynchronous request failed. The worker owns the expensive topology
        // build; the main thread only uploads the completed CPU mesh later.
        if (!task->failed && !task->cpuHeightFallback &&
            !task->meshSubmitted && !task->gpuMeshReady)
        {
            task->meshSubmitted = true;
            PendingTerrainBuild* raw = task;
            try
            {
                task->handle = jobSystem->submit([raw]
                {
                    try
                    {
                        const auto meshStart = std::chrono::steady_clock::now();
                        raw->cpuSucceeded = buildTerrainMeshFromHeightField(
                            raw->config, *raw->heightField, raw->mesh,
                            raw->error, &raw->cancelRequested);
                        const auto meshEnd = std::chrono::steady_clock::now();
                        raw->cpuGenerationMs = raw->heightGenerationMs +
                            std::chrono::duration<double, std::milli>(
                                meshEnd - meshStart).count();
                    }
                    catch (const std::exception& ex)
                    {
                        raw->cpuSucceeded = false;
                        raw->error = std::string("terrain CPU job failed: ") + ex.what();
                    }
                    catch (...)
                    {
                        raw->cpuSucceeded = false;
                        raw->error = "terrain CPU job failed with unknown exception";
                    }
                });
            }
            catch (const std::exception& ex)
            {
                task->failed = true;
                task->error = std::string("terrain mesh job submission failed: ") + ex.what();
            }
            if (!task->failed)
            {
                ++it;
                continue;
            }
        }

        // Stage 3: wait for the CPU mesh job (UI/worker only; no GPU wait).
        if (!task->failed && !task->gpuMeshReady)
        {
            if (!task->handle.valid())
            {
                task->failed = true;
                task->error = "terrain mesh job handle is invalid";
            }
            else if (!task->handle.isComplete())
            {
                ++it;
                continue;
            }
            else
            {
                try
                {
                    task->handle.wait();
                }
                catch (const std::exception& ex)
                {
                    task->cpuSucceeded = false;
                    task->error = std::string("terrain CPU job failed: ") + ex.what();
                }
                catch (...)
                {
                    task->cpuSucceeded = false;
                    task->error = "terrain CPU job failed with unknown exception";
                }
            }
        }

        TerrainGenerationCallback completion = std::move(task->completion);
        const terrain::TerrainConfig config = task->config;
        const double cpuGenerationMs = task->cpuGenerationMs;
        std::string error = task->error;

        if (task->failed || (!task->gpuMeshReady && !task->cpuSucceeded))
        {
            if (error.empty())
                error = "terrain generation failed";
            terrain->generating = false;
            terrain->lastError = error;
            latestTerrainRequest.erase(task->entity);
            std::shared_ptr<PendingTerrainBuild> retired = std::move(*it);
            it = pendingTerrainBuilds.erase(it);
            retireTerrainBuild(std::move(retired));
            if (completion)
            {
                completion(false, error);
                return;   // callback may submit a new request and reallocate the vector
            }
            continue;
        }

        terrain::TerrainMeshData mesh = std::move(task->mesh);
        std::shared_ptr<terrain::HeightField> heightField = std::move(task->heightField);
        std::shared_ptr<Model> model;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        double uploadMs = 0.0;
        bool meshGeneratedOnGpu = false;
        std::string uploadError;

        if (task->gpuMeshReady)
        {
            try
            {
                model = Model::createFromGpuMesh(
                    *device,
                    task->gpuMesh.vertexBuffer, task->gpuMesh.vertexAllocation,
                    task->gpuMesh.vertexCount,
                    task->gpuMesh.indexBuffer, task->gpuMesh.indexAllocation,
                    task->gpuMesh.indexCount,
                    std::move(task->gpuMesh.subMeshes),
                    // F2-e: streamed chunks keep only the height field, not a CPU
                    // copy of the mesh; picking goes through raycastHeightField.
                    std::vector<glm::vec3>{},
                    std::vector<uint32_t>{},
                    task->gpuMesh.boundsMin, task->gpuMesh.boundsMax,
                    task->gpuMesh.uvMin, task->gpuMesh.uvMax,
                    task->gpuMesh.uvValid);
                vertexCount = task->gpuMesh.vertexCount;
                indexCount = task->gpuMesh.indexCount;
                meshGeneratedOnGpu = true;
            }
            catch (const std::exception& ex)
            {
                uploadError = std::string("GPU terrain mesh adoption failed: ") + ex.what();
            }
            catch (...)
            {
                uploadError = "GPU terrain mesh adoption failed with unknown exception";
            }

            if (!model)
            {
                if (task->gpuMesh.vertexBuffer != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device->getVmaAllocator(),
                                     task->gpuMesh.vertexBuffer,
                                     task->gpuMesh.vertexAllocation);
                if (task->gpuMesh.indexBuffer != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device->getVmaAllocator(),
                                     task->gpuMesh.indexBuffer,
                                     task->gpuMesh.indexAllocation);
            }
        }
        else
        {
            if (!uploadTerrainMesh(*device, commandPool, mesh, model,
                                   vertexCount, indexCount, uploadMs, uploadError) ||
                !model)
            {
                if (uploadError.empty())
                    uploadError = "GPU terrain upload failed";
            }
        }

        if (!model)
        {
            terrain->generating = false;
            terrain->lastError = uploadError;
            latestTerrainRequest.erase(task->entity);
            std::shared_ptr<PendingTerrainBuild> retired = std::move(*it);
            it = pendingTerrainBuilds.erase(it);
            retireTerrainBuild(std::move(retired));
            if (completion)
            {
                completion(false, uploadError);
                return;   // callback may submit a new request and reallocate the vector
            }
            continue;
        }

        // F2-g：几何统一进共享 arena（按实际大小分配）；成功后 Model 只留元数据。
        if (terrainMeshArena && terrainMeshArena->isAvailable() && model->hasGpuBuffers())
        {
            if (terrain->arenaIndexBase != TerrainMeshArena::kInvalidOffset)
            {
                TerrainMeshArena::Slice previous{};
                previous.vertexOffset = terrain->arenaVertexBase;
                previous.vertexCount = terrain->arenaVertexCount;
                previous.indexOffset = terrain->arenaIndexBase;
                previous.indexCount = terrain->arenaIndexCount;
                terrainMeshArena->release(previous);
                terrain->arenaVertexBase = TerrainMeshArena::kInvalidOffset;
                terrain->arenaIndexBase = TerrainMeshArena::kInvalidOffset;
                terrain->arenaVertexCount = 0u;
                terrain->arenaIndexCount = 0u;
            }
            TerrainMeshArena::Slice slice{};
            if (terrainMeshArena->acquire(vertexCount, indexCount, slice))
            {
                const VkCommandBuffer arenaCmd =
                    cmdBeginSingleTime(device->getDevice(), commandPool);
                if (arenaCmd != VK_NULL_HANDLE)
                {
                    terrainMeshArena->copyInto(arenaCmd, slice,
                                               model->getVertexBufferHandle(),
                                               model->getIndexBufferHandle());
                    cmdEndSingleTime(device->getDevice(), device->getGraphicsQueue(),
                                     commandPool, arenaCmd);
                    terrain->arenaVertexBase = slice.vertexOffset;
                    terrain->arenaIndexBase = slice.indexOffset;
                    terrain->arenaVertexCount = slice.vertexCount;
                    terrain->arenaIndexCount = slice.indexCount;
                    model->releaseGpuBuffers();
                }
                else
                {
                    terrainMeshArena->release(slice);
                }
            }
        }
        terrain->generatedConfig = config;
        terrain->revision += 1;
        terrain->vertexCount = vertexCount;
        terrain->indexCount = indexCount;
        terrain->lastGenerationMs = cpuGenerationMs + uploadMs;
        terrain->lastError.clear();
        terrain->generated = true;
        terrain->generating = false;
        terrain->meshGeneratedOnGpu = meshGeneratedOnGpu;
        terrain->cpuHeightField = std::move(heightField);
        terrain->generationBackend =
            (terrain->cpuHeightField->generatedOnGpu
                ? (meshGeneratedOnGpu ? "GPU height + GPU mesh" : "GPU height")
                : (meshGeneratedOnGpu ? "GPU mesh" : "CPU fallback"));

        std::shared_ptr<Model> previousModel;
        if (const auto* previousMesh = registry.try_get<MeshComponent>(task->entity))
            previousModel = previousMesh->model;

        registry.emplace_or_replace<MeshComponent>(task->entity, MeshComponent{ model });
        retireModel(previousModel);

        RenderTagComponent* tag = registry.try_get<RenderTagComponent>(task->entity);
        if (!tag)
            tag = &registry.emplace<RenderTagComponent>(task->entity);
        tag->isTerrain = true;

        setModelBounds(registry, task->entity, model, &dirtySpatialEntities);
        attachDirtyTracking(task->entity);
        latestTerrainRequest.erase(task->entity);
            std::shared_ptr<PendingTerrainBuild> retired = std::move(*it);
            it = pendingTerrainBuilds.erase(it);
            retireTerrainBuild(std::move(retired));
        refreshTransformsAndVisibility();
        if (completion)
        {
            completion(true, "");
            return;   // callback may submit a new request and reallocate the vector
        }
    }
}

void Scene::waitForTerrainGeneration()
{
    while (!pendingTerrainBuilds.empty() || !retiringTerrainBuilds.empty())
    {
        bool waitedJob = false;
        for (const auto& task : retiringTerrainBuilds)
        {
            if (!task->handle.valid())
                continue;
            waitedJob = true;
            try
            {
                task->handle.wait();
            }
            catch (...)
            {
                // processTerrainGeneration() 只负责回收。
            }
        }
        for (const auto& task : pendingTerrainBuilds)
        {
            if (task->handle.valid())
            {
                waitedJob = true;
                try
                {
                    task->handle.wait();
                }
                catch (...)
                {
                    // processTerrainGeneration() records the failure.
                }
            }
        }
        const auto terrainPollStart = std::chrono::steady_clock::now();
        processTerrainGeneration();
        recordTerrainPollMs(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - terrainPollStart).count());
        if (!waitedJob &&
            (!pendingTerrainBuilds.empty() || !retiringTerrainBuilds.empty()))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Scene::discardPendingTerrainCallbacks()
{
    latestTerrainRequest.clear();
    for (auto& task : pendingTerrainBuilds)
    {
        task->cancelRequested.store(true, std::memory_order_relaxed);
        task->completion = {};
        if (task->gpuHeightRequest != 0u && terrainComputeGenerator)
        {
            terrainComputeGenerator->release(task->gpuHeightRequest);
            task->gpuHeightRequest = 0u;
        }
        if (task->gpuMeshRequest != 0u && terrainGpuMeshBuilder)
        {
            terrainGpuMeshBuilder->release(task->gpuMeshRequest);
            task->gpuMeshRequest = 0u;
        }
    }
    for (auto& task : retiringTerrainBuilds)
    {
        task->cancelRequested.store(true, std::memory_order_relaxed);
        task->completion = {};
    }
}

void Scene::cancelPendingTerrainGenerations()
{
    for (const auto& task : retiringTerrainBuilds)
    {
        task->cancelRequested.store(true, std::memory_order_relaxed);
    }
    for (const auto& task : pendingTerrainBuilds)
    {
        task->cancelRequested.store(true, std::memory_order_relaxed);
        if (task->gpuHeightRequest != 0u && terrainComputeGenerator)
        {
            terrainComputeGenerator->release(task->gpuHeightRequest);
            task->gpuHeightRequest = 0u;
        }
        if (task->gpuMeshRequest != 0u && terrainGpuMeshBuilder)
        {
            terrainGpuMeshBuilder->release(task->gpuMeshRequest);
            task->gpuMeshRequest = 0u;
        }
    }
    if (terrainComputeGenerator)
        terrainComputeGenerator->waitAll();
    if (terrainGpuMeshBuilder)
        terrainGpuMeshBuilder->waitAll();

    for (const auto& task : pendingTerrainBuilds)
    {
        if (task->handle.valid())
        {
            try
            {
                task->handle.wait();
            }
            catch (...)
            {
            }
        }
    }
    pendingTerrainBuilds.clear();
    // retiring 任务同样必须 join 后才能释放，否则 worker 会写已释放内存。
    for (const auto& task : retiringTerrainBuilds)
    {
        if (task->handle.valid())
        {
            try
            {
                task->handle.wait();
            }
            catch (...)
            {
            }
        }
    }
    retiringTerrainBuilds.clear();
    latestTerrainRequest.clear();
}

void Scene::refreshTransformsAndVisibility()
{
    updateWorldTransforms();
    refreshSpatialIndex();
    updateVisibility(camera.getProjection() * camera.getView());
}
