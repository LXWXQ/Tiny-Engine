#include "Scene/Scene.h"
#include "AssetPaths.h"
#include "RHI/Texture.h"
#include "RHI/CommandUtils.h"
#include "Scene/GLTFLoader.h"
#include "Scene/FeatureComponents.h"
#include "Scene/NativeComponents.h"
#include "Scene/SceneComponents.h"
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
    constexpr int32_t kMaxTerrainStreamingRadius = Scene::kMaxStreamingRadius;
    constexpr uint32_t kMaxTerrainStreamingLoadsPerFrame = 16;

    Scene::TerrainChunkCoord terrainChunkCoord(int64_t x, int64_t z)
    {
        return Scene::TerrainChunkCoord{ x, z };
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

size_t Scene::TerrainChunkCoordHash::operator()(const Scene::TerrainChunkCoord& coord)
        const noexcept
    {
        uint64_t h = static_cast<uint64_t>(coord.x) * 0x9e3779b97f4a7c15ull;
        h ^= static_cast<uint64_t>(coord.z) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
    return static_cast<size_t>(h);
}

Scene::~Scene() = default;

Scene::Scene()
{
    // 纯 CPU 注册，不依赖设备；selfTest 直接构造 Scene 也能拿到同样的类型表。
    scene::registerFeatureComponents(componentRegistry);
    scene::registerNativeComponents(componentRegistry);
    scene::registerSceneComponents(componentRegistry);
    scene::registerFeatureSystems(systemScheduler);

    // 场景级组件需要一个不参与 Transform/父子/渲染的宿主实体。
    // 不挂 Name 与 Hierarchy，所以 Hierarchy 面板和场景实体遍历都看不到它。
    sceneSettingsEntity = registry.create();
    scene::resetSceneComponentsFromSettings(registry, sceneSettingsEntity, engineSettings);
}

bool Scene::acceptsComponentTarget(entt::entity entity,
                                   const scene::ComponentDescriptor& descriptor) const
{
    if (!registry.valid(entity))
        return false;
    if (descriptor.scope == scene::ComponentScope::Scene)
        return entity == sceneSettingsEntity;
    return entity != sceneSettingsEntity;
}

bool Scene::addSceneComponent(const std::string& type)
{
    return addComponent(sceneSettingsEntity, type);
}

bool Scene::removeSceneComponent(const std::string& type)
{
    return removeComponent(sceneSettingsEntity, type);
}

bool Scene::hasSceneComponent(const std::string& type) const
{
    return hasComponent(sceneSettingsEntity, type);
}

std::vector<scene::ComponentRecord> Scene::captureSceneComponents() const
{
    return captureComponentRecords(sceneSettingsEntity);
}

void Scene::resetSceneComponentsFromSettings()
{
    scene::resetSceneComponentsFromSettings(registry, sceneSettingsEntity, engineSettings);
    syncSceneSettings();
}

void Scene::clearSceneComponents()
{
    scene::clearSceneComponents(registry, sceneSettingsEntity);
    syncSceneSettings();
}

void Scene::syncSceneSettings()
{
    scene::syncSceneSettings(registry, sceneSettingsEntity, engineSettings);
}

bool Scene::addComponent(entt::entity entity, const std::string& type)
{
    if (!registry.valid(entity))
        return false;
    const scene::ComponentDescriptor* descriptor = componentRegistry.find(type);
    if (!descriptor || !descriptor->addable || !acceptsComponentTarget(entity, *descriptor))
        return false;
    descriptor->addDefault(registry, entity);
    if (descriptor->scope == scene::ComponentScope::Scene)
        syncSceneSettings();
    return descriptor->has(registry, entity);
}

bool Scene::removeComponent(entt::entity entity, const std::string& type)
{
    if (!registry.valid(entity))
        return false;
    const scene::ComponentDescriptor* descriptor = componentRegistry.find(type);
    if (!descriptor || !descriptor->has(registry, entity) ||
        !acceptsComponentTarget(entity, *descriptor))
        return false;
    if (descriptor->required || !descriptor->removable)
        return false;
    if (descriptor->canRemove && !descriptor->canRemove(registry, entity))
        return false;
    descriptor->remove(registry, entity);
    if (descriptor->scope == scene::ComponentScope::Scene)
        syncSceneSettings();
    return true;
}

bool Scene::hasComponent(entt::entity entity, const std::string& type) const
{
    if (!registry.valid(entity))
        return false;
    const scene::ComponentDescriptor* descriptor = componentRegistry.find(type);
    return descriptor && acceptsComponentTarget(entity, *descriptor) &&
           descriptor->has(registry, entity);
}

std::optional<scene::ComponentRecord> Scene::readComponent(entt::entity entity,
                                                           const std::string& type) const
{
    if (!registry.valid(entity))
        return std::nullopt;
    const scene::ComponentDescriptor* descriptor = componentRegistry.find(type);
    if (!descriptor || !acceptsComponentTarget(entity, *descriptor) ||
        !descriptor->has(registry, entity))
        return std::nullopt;
    scene::ComponentRecord record = descriptor->read(registry, entity);
    if (record.type.empty())
        record.type = type;
    return record;
}

std::optional<scene::ComponentRecord> Scene::defaultComponentRecord(const std::string& type) const
{
    const scene::ComponentDescriptor* descriptor = componentRegistry.find(type);
    if (!descriptor)
        return std::nullopt;
    scene::ComponentRecord record = descriptor->defaults();
    if (record.type.empty())
        record.type = type;
    return record;
}

bool Scene::applyComponentRecord(entt::entity entity, const scene::ComponentRecord& record)
{
    if (!registry.valid(entity) || record.type.empty())
        return false;
    const scene::ComponentDescriptor* descriptor = componentRegistry.find(record.type);
    if (!descriptor || !acceptsComponentTarget(entity, *descriptor))
        return false;
    descriptor->write(registry, entity, record);
    if (descriptor->scope == scene::ComponentScope::Scene)
        syncSceneSettings();
    return descriptor->has(registry, entity);
}

std::vector<scene::ComponentRecord> Scene::captureComponentRecords(entt::entity entity) const
{
    std::vector<scene::ComponentRecord> records;
    if (!registry.valid(entity))
        return records;
    for (const scene::ComponentDescriptor& descriptor : componentRegistry.descriptors())
    {
        // 原生组件的字段已经由 EntityDescription 的专用字段保存，不再重复
        // 写进 v3 的通用 components 数组。
        if (!descriptor.serialized)
            continue;
        if (!acceptsComponentTarget(entity, descriptor))
            continue;
        if (!descriptor.has(registry, entity))
            continue;
        scene::ComponentRecord record = descriptor.read(registry, entity);
        if (!record.type.empty())
            records.push_back(std::move(record));
    }
    return records;
}

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

    // 场景级组件的宿主不接受普通销毁；它随 Scene 生命周期存在。
    if (entity == sceneSettingsEntity)
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
        const TerrainChunkCoord key = terrainChunkCoord(chunk->chunkX, chunk->chunkZ);
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

    // V-04：体素不透明几何也走共享 arena，销毁实体时必须交还切片。
    if (auto* voxel = registry.try_get<VoxelChunkComponent>(entity))
    {
        if (terrainMeshArena && voxel->arenaIndexBase != TerrainMeshArena::kInvalidOffset)
        {
            TerrainMeshArena::Slice slice{};
            slice.vertexOffset = voxel->arenaVertexBase;
            slice.vertexCount = voxel->arenaVertexCount;
            slice.indexOffset = voxel->arenaIndexBase;
            slice.indexCount = voxel->arenaIndexCount;
            terrainMeshArena->release(slice);
        }
        voxel->arenaVertexBase = TerrainMeshArena::kInvalidOffset;
        voxel->arenaIndexBase = TerrainMeshArena::kInvalidOffset;
        voxel->arenaVertexCount = 0u;
        voxel->arenaIndexCount = 0u;
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
    const double genPollMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - terrainPollStart).count();
    recordTerrainPollMs(genPollMs);

    TransformComponent& cameraTransform = getCameraTransform();
    cameraController.inputEnabled = allowCameraInput;
    cameraController.processPCInput(window, dt, cameraTransform);
    if (cameraTransform.isDirty())
        markTransformDirty(cameraEntity);

    maybeRebaseWorldOrigin();
    camera.setViewYXZ(cameraTransform.getTranslation(), cameraTransform.getEuler());
    camera.setPerspectiveProjection(glm::radians(60.0f), aspectRatio, 0.1f, 5000.0f);

    // F2 收尾：分阶段计时（默认关闭，计时器在关闭时也被跳过）。
    double streamingMs = 0.0;
    double worldMs = 0.0;
    double spatialMs = 0.0;
    double visibilityMs = 0.0;
    if (updateProfiling)
    {
        const auto streamingStart = std::chrono::steady_clock::now();
        refreshStreamingTerrain();
        streamingMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - streamingStart).count();

        const auto worldStart = std::chrono::steady_clock::now();
        systemScheduler.run(registry, scene::SystemPhase::Simulation, dt);
        updateWorldTransforms();
        systemScheduler.run(registry, scene::SystemPhase::PostTransform, dt);
        worldMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - worldStart).count();

        const auto spatialStart = std::chrono::steady_clock::now();
        refreshSpatialIndex();
        spatialMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - spatialStart).count();

        const auto visibilityStart = std::chrono::steady_clock::now();
        updateVisibility(camera.getProjection() * camera.getView());
        visibilityMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - visibilityStart).count();

        reportUpdateProfile(genPollMs * 1000.0, streamingMs * 1000.0, worldMs * 1000.0,
                            spatialMs * 1000.0, visibilityMs * 1000.0);
        return;
    }

    refreshStreamingTerrain();
    processVoxelChunkBuilds();

    // 效果系统（Spin / Orbit…）先改局部 Transform，再统一刷新世界矩阵。
    // PostTransform 预留给依赖世界坐标的系统（当前无内置系统）。
    systemScheduler.run(registry, scene::SystemPhase::Simulation, dt);
    updateWorldTransforms();
    systemScheduler.run(registry, scene::SystemPhase::PostTransform, dt);
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

    // F2 收尾：直接线性扫描 renderable，不再走 spatialIndex.query(broadBounds)。
    //
    // 旧路径的问题：宽包围盒在小 cell 上经常超过 4096 个 cell，于是退化成
    // collectAll() —— 按 cell 把同一实体重复展开（半径 24 时 2401 个 chunk
    // 展开成约 1.5 万条），再 sortUnique() 排序去重。这两步在半径 24 时占了
    // Scene::update() 的大头，而且每帧还要算一次 viewProjection 的逆。
    // 线性扫描是 O(n)、无重复、无排序，剔除判据（6 个平面 vs 世界 AABB）不变。
    for (auto [entity, mesh] : registry.view<MeshComponent>().each())
    {
        (void)mesh;
        const auto* tag = registry.try_get<RenderTagComponent>(entity);
        if (tag != nullptr && tag->isSkybox)
        {
            // 天空盒跟相机走，永远要画。
            visibleEntities.push_back(entity);
            continue;
        }

        const auto* worldBounds = registry.try_get<WorldBoundsComponent>(entity);
        if (worldBounds == nullptr)
            continue;

        if (intersectsAABB(frustum, worldBounds->world))
            visibleEntities.push_back(entity);
    }
}

void Scene::cleanup()
{
    cancelPendingTerrainGenerations();

    // 与 clearEntities() 同理：作业仍持有 PendingVoxelChunk 的裸指针，
    // 这里只标记取消，由 JobSystem 在销毁时等待作业结束，避免 UAF。
    for (const std::shared_ptr<PendingVoxelChunk>& pending : pendingVoxelBuilds)
    {
        pending->cancelled = true;
        pending->entity = entt::null;
    }
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

    // ---- 元数据驱动 ECS：组件注册 / 属性读写 / 系统更新 ----
    // 这段不建 Vulkan 设备，直接构造一个 Scene（构造函数只注册组件和系统）
    // 就能覆盖编辑器依赖的全部纯 CPU 路径。
    std::size_t componentCount = 0;
    std::size_t systemCount = 0;
    {
        Scene scene;
        scene.getRegistry();
        componentCount = scene.getComponentRegistry().descriptors().size();
        systemCount = scene.getSystemScheduler().systemCount();
        if (componentCount != 25 || systemCount != 7)
        {
            std::cerr << "[Scene] selfTest FAIL: built-in component/system registration mismatch\n";
            ok = false;
        }

        const auto defaultSpin = scene.defaultComponentRecord("effects.spin");
        if (!defaultSpin || defaultSpin->type != "effects.spin" ||
            defaultSpin->properties.size() != 3)
        {
            std::cerr << "[Scene] selfTest FAIL: Spin default record mismatch\n";
            ok = false;
        }

        // F5-05：属性 UI 元数据必须随注册表一起可用，否则 Inspector 只能猜范围。
        auto findProperty = [](const scene::ComponentRegistry& registry,
                               const char* type, const char* name)
            -> const scene::PropertyDescriptor*
        {
            const scene::ComponentDescriptor* descriptor = registry.find(type);
            if (descriptor == nullptr)
                return nullptr;
            for (const scene::PropertyDescriptor& property : descriptor->properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        };

        const scene::PropertyDescriptor* spinSpeed =
            findProperty(scene.getComponentRegistry(), "effects.spin", "degreesPerSecond");
        if (spinSpeed == nullptr || !spinSpeed->hasRange ||
            spinSpeed->minimum != -3600.0f || spinSpeed->maximum != 3600.0f ||
            spinSpeed->dragSpeed != 0.5f || spinSpeed->tooltip.empty())
        {
            std::cerr << "[Scene] selfTest FAIL: effect property metadata mismatch\n";
            ok = false;
        }

        const scene::PropertyDescriptor* characterHeight =
            findProperty(scene.getComponentRegistry(), "native.characterController", "height");
        if (characterHeight == nullptr || !characterHeight->hasRange ||
            characterHeight->minimum != 0.2f || characterHeight->maximum != 10.0f ||
            characterHeight->tooltip.empty())
        {
            std::cerr << "[Scene] selfTest FAIL: native property metadata mismatch\n";
            ok = false;
        }

        entt::registry& testRegistry = scene.getRegistry();
        const entt::entity testEntity = testRegistry.create();
        testRegistry.emplace<TransformComponent>(testEntity);

        if (scene.readComponent(testEntity, "effects.spin").has_value())
        {
            std::cerr << "[Scene] selfTest FAIL: unregistered component read returned a record\n";
            ok = false;
        }

        if (!scene.addComponent(testEntity, "effects.spin") ||
            !scene.hasComponent(testEntity, "effects.spin"))
        {
            std::cerr << "[Scene] selfTest FAIL: addComponent failed\n";
            ok = false;
        }

        auto spin = scene.readComponent(testEntity, "effects.spin");
        if (!spin)
        {
            std::cerr << "[Scene] selfTest FAIL: readComponent returned no record\n";
            ok = false;
        }
        else
        {
            scene::NamedProperty* speed = spin->find("degreesPerSecond");
            if (!speed || speed->value.asFloat() != 45.0f)
            {
                std::cerr << "[Scene] selfTest FAIL: Spin default speed mismatch\n";
                ok = false;
            }
            else
            {
                speed->value = scene::PropertyValue::fromFloat(90.0f);
                if (!scene.applyComponentRecord(testEntity, *spin))
                {
                    std::cerr << "[Scene] selfTest FAIL: applyComponentRecord failed\n";
                    ok = false;
                }
                const auto updated = scene.readComponent(testEntity, "effects.spin");
                const scene::NamedProperty* updatedSpeed =
                    updated ? updated->find("degreesPerSecond") : nullptr;
                if (!updatedSpeed || updatedSpeed->value.asFloat() != 90.0f)
                {
                    std::cerr << "[Scene] selfTest FAIL: component property round trip mismatch\n";
                    ok = false;
                }
            }
        }

        // Spin 以 90°/s 跑 0.5s：四元数应变化，但幅度必须是 45°（点积≈cos22.5°）。
        const glm::quat beforeOrientation =
            testRegistry.get<TransformComponent>(testEntity).getOrientation();
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.5f);
        const glm::quat afterOrientation =
            testRegistry.get<TransformComponent>(testEntity).getOrientation();
        const float dot = std::abs(glm::dot(beforeOrientation, afterOrientation));
        if (dot > 0.9999f || dot < 0.9f)
        {
            std::cerr << "[Scene] selfTest FAIL: spin system rotation mismatch\n";
            ok = false;
        }

        if (!scene.removeComponent(testEntity, "effects.spin") ||
            scene.hasComponent(testEntity, "effects.spin"))
        {
            std::cerr << "[Scene] selfTest FAIL: removeComponent semantics mismatch\n";
            ok = false;
        }

        // 第一批效果组件：每个系统跑一个完整周期，验证动作发生且能回到原位。
        const auto makeEffectEntity = [&testRegistry]()
        {
            const entt::entity entity = testRegistry.create();
            testRegistry.emplace<TransformComponent>(entity);
            return entity;
        };

        const entt::entity bobEntity = makeEffectEntity();
        if (!scene.addComponent(bobEntity, "effects.bob"))
        {
            std::cerr << "[Scene] selfTest FAIL: Bob addComponent failed\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.25f);
        const float bobY =
            testRegistry.get<TransformComponent>(bobEntity).getTranslation().y;
        if (std::abs(bobY - 0.25f) > 1e-3f)
        {
            std::cerr << "[Scene] selfTest FAIL: Bob displacement mismatch\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.75f);
        if (std::abs(testRegistry.get<TransformComponent>(bobEntity).getTranslation().y) > 2e-3f)
        {
            std::cerr << "[Scene] selfTest FAIL: Bob did not return to origin\n";
            ok = false;
        }

        const entt::entity pulseEntity = makeEffectEntity();
        if (!scene.addComponent(pulseEntity, "effects.pulse"))
        {
            std::cerr << "[Scene] selfTest FAIL: Pulse addComponent failed\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.25f);
        const float pulseScale =
            testRegistry.get<TransformComponent>(pulseEntity).getScale().x;
        if (std::abs(pulseScale - 1.15f) > 1e-3f)
        {
            std::cerr << "[Scene] selfTest FAIL: Pulse scale mismatch\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.75f);
        if (std::abs(testRegistry.get<TransformComponent>(pulseEntity).getScale().x - 1.0f) > 2e-3f)
        {
            std::cerr << "[Scene] selfTest FAIL: Pulse did not return to unit scale\n";
            ok = false;
        }

        const entt::entity swayEntity = makeEffectEntity();
        if (!scene.addComponent(swayEntity, "effects.sway"))
        {
            std::cerr << "[Scene] selfTest FAIL: Sway addComponent failed\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.25f);
        const float swayMidDot = std::abs(glm::dot(
            glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
            testRegistry.get<TransformComponent>(swayEntity).getOrientation()));
        if (swayMidDot > 0.99999f)
        {
            std::cerr << "[Scene] selfTest FAIL: Sway did not rotate\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.75f);
        const float swayEndDot = std::abs(glm::dot(
            glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
            testRegistry.get<TransformComponent>(swayEntity).getOrientation()));
        if (swayEndDot < 0.9999f)
        {
            std::cerr << "[Scene] selfTest FAIL: Sway did not return to identity\n";
            ok = false;
        }

        const entt::entity lookAtEntity = makeEffectEntity();
        if (!scene.addComponent(lookAtEntity, "effects.lookAt"))
        {
            std::cerr << "[Scene] selfTest FAIL: Look At addComponent failed\n";
            ok = false;
        }
        auto lookAtRecord = scene.readComponent(lookAtEntity, "effects.lookAt");
        if (lookAtRecord)
        {
            if (scene::NamedProperty* target = lookAtRecord->find("target"))
                target->value = scene::PropertyValue::fromVec3(glm::vec3(10.0f, 0.0f, 0.0f));
            scene.applyComponentRecord(lookAtEntity, *lookAtRecord);
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 0.016f);
        const glm::vec3 lookDirection =
            glm::mat3_cast(testRegistry.get<TransformComponent>(lookAtEntity).getOrientation()) *
            glm::vec3(0.0f, 0.0f, -1.0f);
        if (lookDirection.x < 0.99f)
        {
            std::cerr << "[Scene] selfTest FAIL: Look At forward axis mismatch\n";
            ok = false;
        }

        const entt::entity followEntity = makeEffectEntity();
        if (!scene.addComponent(followEntity, "effects.follow"))
        {
            std::cerr << "[Scene] selfTest FAIL: Follow addComponent failed\n";
            ok = false;
        }
        auto followRecord = scene.readComponent(followEntity, "effects.follow");
        if (followRecord)
        {
            if (scene::NamedProperty* target = followRecord->find("target"))
                target->value = scene::PropertyValue::fromVec3(glm::vec3(10.0f, 0.0f, 0.0f));
            if (scene::NamedProperty* speed = followRecord->find("speed"))
                speed->value = scene::PropertyValue::fromFloat(2.0f);
            scene.applyComponentRecord(followEntity, *followRecord);
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 1.0f);
        if (std::abs(testRegistry.get<TransformComponent>(followEntity).getTranslation().x - 2.0f) >
            1e-3f)
        {
            std::cerr << "[Scene] selfTest FAIL: Follow speed mismatch\n";
            ok = false;
        }
        scene.getSystemScheduler().run(testRegistry, scene::SystemPhase::Simulation, 4.0f);
        if (std::abs(testRegistry.get<TransformComponent>(followEntity).getTranslation().x - 9.9f) >
            2e-3f)
        {
            std::cerr << "[Scene] selfTest FAIL: Follow stop distance mismatch\n";
            ok = false;
        }

        // 第二批原生组件：可添加/移除的 Character / RenderTag 与必需组件守卫。
        if (!scene.addComponent(testEntity, "native.characterController") ||
            !scene.hasComponent(testEntity, "native.characterController"))
        {
            std::cerr << "[Scene] selfTest FAIL: native Character Controller add failed\n";
            ok = false;
        }
        auto characterRecord = scene.readComponent(testEntity, "native.characterController");
        if (!characterRecord)
        {
            std::cerr << "[Scene] selfTest FAIL: native Character Controller read failed\n";
            ok = false;
        }
        else
        {
            const scene::NamedProperty* radius = characterRecord->find("radius");
            if (radius == nullptr || std::abs(radius->value.asFloat() - 0.35f) > 1e-4f)
            {
                std::cerr << "[Scene] selfTest FAIL: native Character default radius mismatch\n";
                ok = false;
            }
            else
            {
                scene::NamedProperty* editable = characterRecord->find("radius");
                editable->value = scene::PropertyValue::fromFloat(0.42f);
                if (!scene.applyComponentRecord(testEntity, *characterRecord))
                {
                    std::cerr << "[Scene] selfTest FAIL: native Character apply failed\n";
                    ok = false;
                }
                const auto updated = scene.readComponent(testEntity, "native.characterController");
                const scene::NamedProperty* updatedRadius =
                    updated ? updated->find("radius") : nullptr;
                if (updatedRadius == nullptr ||
                    std::abs(updatedRadius->value.asFloat() - 0.42f) > 1e-4f)
                {
                    std::cerr << "[Scene] selfTest FAIL: native Character round trip mismatch\n";
                    ok = false;
                }
            }
        }
        if (!scene.addComponent(testEntity, "native.renderTag") ||
            !scene.removeComponent(testEntity, "native.renderTag"))
        {
            std::cerr << "[Scene] selfTest FAIL: native Render Tag add/remove failed\n";
            ok = false;
        }

        // 原生组件不进通用 components 数组（序列化去重）。
        const std::vector<scene::ComponentRecord> nativeCapture =
            scene.captureComponentRecords(testEntity);
        if (std::any_of(nativeCapture.begin(), nativeCapture.end(),
                        [](const scene::ComponentRecord& record)
                        {
                            return record.type.rfind("native.", 0) == 0;
                        }))
        {
            std::cerr << "[Scene] selfTest FAIL: native component leaked into v3 components\n";
            ok = false;
        }

        // 必需组件不能被加/删。
        if (scene.addComponent(testEntity, "native.transform") ||
            scene.removeComponent(testEntity, "native.transform"))
        {
            std::cerr << "[Scene] selfTest FAIL: required component guard failed\n";
            ok = false;
        }

        if (!scene.removeComponent(testEntity, "native.characterController") ||
            scene.hasComponent(testEntity, "native.characterController"))
        {
            std::cerr << "[Scene] selfTest FAIL: native Character remove failed\n";
            ok = false;
        }

        if (scene.addComponent(testEntity, "missing.component"))
        {
            std::cerr << "[Scene] selfTest FAIL: unknown component was accepted\n";
            ok = false;
        }

        if (!defaultSpin || !scene.defaultComponentRecord("effects.spin") ||
            !defaultSpin->equivalentTo(*scene.defaultComponentRecord("effects.spin")))
        {
            std::cerr << "[Scene] selfTest FAIL: default record equivalence mismatch\n";
            ok = false;
        }

        // 场景级组件：和实体组件共用同一套 descriptor / record，但只能挂在
        // 隐藏的场景设置实体上，并且值变化会立即同步到 EngineSettings。
        const entt::entity settingsEntity = scene.getSceneSettingsEntity();
        std::size_t sceneDescriptorCount = 0;
        std::size_t mountedSceneComponents = 0;
        for (const scene::ComponentDescriptor& descriptor : scene.getComponentRegistry().descriptors())
        {
            if (descriptor.scope != scene::ComponentScope::Scene)
                continue;
            ++sceneDescriptorCount;
            if (scene.hasSceneComponent(descriptor.type))
                ++mountedSceneComponents;
        }
        if (sceneDescriptorCount != 9 || mountedSceneComponents != 9 ||
            settingsEntity == entt::null || !testRegistry.valid(settingsEntity) ||
            testRegistry.any_of<TransformComponent>(settingsEntity) ||
            testRegistry.any_of<HierarchyComponent>(settingsEntity) ||
            testRegistry.any_of<NameComponent>(settingsEntity))
        {
            std::cerr << "[Scene] selfTest FAIL: scene component registration/host mismatch"
                      << " | descriptors=" << sceneDescriptorCount
                      << " mounted=" << mountedSceneComponents << "\n";
            ok = false;
        }

        // scope 是硬约束：场景组件不能挂普通实体，实体组件也不能挂设置实体。
        if (scene.addComponent(testEntity, "scene.shadows") ||
            scene.hasComponent(testEntity, "scene.shadows") ||
            scene.readComponent(testEntity, "scene.shadows").has_value() ||
            scene.addComponent(settingsEntity, "effects.spin") ||
            scene.hasComponent(settingsEntity, "effects.spin"))
        {
            std::cerr << "[Scene] selfTest FAIL: component scope guard failed\n";
            ok = false;
        }
        if (const auto spinDefault = scene.defaultComponentRecord("effects.spin");
            spinDefault && scene.applyComponentRecord(settingsEntity, *spinDefault))
        {
            std::cerr << "[Scene] selfTest FAIL: entity record applied to scene settings entity\n";
            ok = false;
        }

        // 调参立即写回 EngineSettings；禁用 = 功能关闭但参数仍保留。
        auto environmentRecord = scene.readComponent(settingsEntity, "scene.environment");
        if (!environmentRecord)
        {
            std::cerr << "[Scene] selfTest FAIL: scene environment component missing\n";
            ok = false;
        }
        else if (scene::NamedProperty* exposure = environmentRecord->find("exposure"))
        {
            exposure->value = scene::PropertyValue::fromFloat(1.75f);
            if (!scene.applyComponentRecord(settingsEntity, *environmentRecord) ||
                std::abs(scene.getSettings().exposure - 1.75f) > 1e-4f)
            {
                std::cerr << "[Scene] selfTest FAIL: scene exposure did not sync\n";
                ok = false;
            }
        }
        else
        {
            std::cerr << "[Scene] selfTest FAIL: scene exposure property missing\n";
            ok = false;
        }

        auto bloomRecord = scene.readComponent(settingsEntity, "scene.bloom");
        if (!bloomRecord)
        {
            std::cerr << "[Scene] selfTest FAIL: scene bloom component missing\n";
            ok = false;
        }
        else if (scene::NamedProperty* enabled = bloomRecord->find("enabled"))
        {
            enabled->value = scene::PropertyValue::fromBool(false);
            if (!scene.applyComponentRecord(settingsEntity, *bloomRecord) ||
                scene.getSettings().bloomEnabled)
            {
                std::cerr << "[Scene] selfTest FAIL: disabled bloom still enabled\n";
                ok = false;
            }
            enabled = bloomRecord->find("enabled");
            enabled->value = scene::PropertyValue::fromBool(true);
            if (!scene.applyComponentRecord(settingsEntity, *bloomRecord) ||
                !scene.getSettings().bloomEnabled)
            {
                std::cerr << "[Scene] selfTest FAIL: bloom re-enable failed\n";
                ok = false;
            }
        }
        else
        {
            std::cerr << "[Scene] selfTest FAIL: scene bloom enabled property missing\n";
            ok = false;
        }

        // 移除 = 关闭；重新添加 = 默认值并重新启用。
        if (!scene.removeSceneComponent("scene.fog") || scene.getSettings().fogEnabled ||
            !scene.addSceneComponent("scene.fog") || !scene.getSettings().fogEnabled)
        {
            std::cerr << "[Scene] selfTest FAIL: scene fog add/remove semantics mismatch\n";
            ok = false;
        }

        // 整场景导出：sceneComponents 总是 authored，且包含全部 9 项。
        const scene::SceneDescription captured = scene.captureDescription();
        bool capturedExposure = false;
        for (const scene::ComponentRecord& record : captured.sceneComponents)
        {
            if (record.type != "scene.environment")
                continue;
            const scene::NamedProperty* exposure = record.find("exposure");
            capturedExposure = exposure &&
                               std::abs(exposure->value.asFloat() - 1.75f) < 1e-4f;
        }
        if (!captured.sceneComponentsAuthored || captured.sceneComponents.size() != 9 ||
            !capturedExposure)
        {
            std::cerr << "[Scene] selfTest FAIL: scene component capture mismatch | authored="
                      << captured.sceneComponentsAuthored
                      << " count=" << captured.sceneComponents.size() << "\n";
            ok = false;
        }

        // 清空 = 全部功能关闭；reset 从 EngineSettings 恢复整套默认组件。
        scene.clearSceneComponents();
        if (!scene.captureSceneComponents().empty() ||
            scene.getSettings().shadowEnabled || scene.getSettings().bloomEnabled ||
            scene.getSettings().fogEnabled ||
            std::abs(scene.getSettings().exposure - EngineSettings{}.exposure) > 1e-4f)
        {
            std::cerr << "[Scene] selfTest FAIL: clearing scene components left features enabled\n";
            ok = false;
        }
        // 从一份真实的 engine.json 默认值恢复（clear 已把当前 settings 改成全关）。
        scene.getSettings() = EngineSettings{};
        scene.resetSceneComponentsFromSettings();
        if (scene.captureSceneComponents().size() != 9 ||
            !scene.getSettings().shadowEnabled || !scene.getSettings().bloomEnabled ||
            !scene.getSettings().fogEnabled)
        {
            std::cerr << "[Scene] selfTest FAIL: scene component reset did not restore defaults\n";
            ok = false;
        }

        // 隐藏设置实体不能被普通删除路径销毁。
        scene.destroyEntity(settingsEntity);
        if (!scene.getRegistry().valid(settingsEntity) ||
            !scene.hasSceneComponent("scene.shadows"))
        {
            std::cerr << "[Scene] selfTest FAIL: scene settings entity was destroyed\n";
            ok = false;
        }
    }

    std::cout << "[Scene] ECS/spatial selfTest " << (ok ? "PASS" : "FAIL")
              << " | hierarchy=T*Ry*Rx*Rz*S | grid cells=" << index.cellCount() << "\n";
    std::cout << "[Scene] ECS component selfTest " << (ok ? "PASS" : "FAIL")
              << " | components=" << componentCount << " | systems=" << systemCount << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// 场景描述：导出 / 重建（编辑器与序列化共用）
// ---------------------------------------------------------------------------
scene::SceneDescription Scene::captureDescription() const
{
    return captureDescription(entt::null);
}

scene::SceneDescription Scene::captureDescription(entt::entity root) const
{
    scene::SceneDescription description;
    description.version = 1;

    if (root != entt::null && !registry.valid(root))
        return description;

    // Streamed chunk entities are runtime derivatives of the manager; only the
    // manager template is serialized so load can reconstruct the active set.
    std::vector<entt::entity> serializableEntities;
    if (root == entt::null)
    {
        for (entt::entity entity : registry.view<TransformComponent>())
        {
            // 体素 chunk 与地形 chunk 一样是运行期派生物，由世界管理器重建，
            // 不能当普通实体写进场景（否则载入后会多出一堆空实体）。
            if (!registry.any_of<TerrainChunkComponent>(entity) &&
                !registry.any_of<VoxelChunkComponent>(entity))
                serializableEntities.push_back(entity);
        }
    }
    else
    {
        // 预制体只导出选中实体及其后代。DFS 保证父一定排在子之前，
        // 这样 parentIndex 可以继续用“已导出数组下标”表达。
        std::unordered_set<entt::entity> visited;
        std::function<void(entt::entity)> collect = [&](entt::entity entity)
        {
            if (entity == entt::null || !registry.valid(entity) || !visited.insert(entity).second)
                return;
            if (!registry.any_of<TerrainChunkComponent>(entity) &&
                !registry.any_of<VoxelChunkComponent>(entity))
                serializableEntities.push_back(entity);
            if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
                for (entt::entity child : hierarchy->children)
                    collect(child);
        };
        collect(root);
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

        // 元数据注册过的扩展组件（Spin / Orbit / 以后新增的效果）。
        // 顺序由 ComponentRegistry 的注册顺序决定，序列化与撤销都按 type 匹配。
        e.components = captureComponentRecords(entity);

        // 注意：这里用 push_back，顺序必须与我上面 enum 的顺序一致。
        // 因为 idToIndex 也是按同一个 view 顺序建的，所以两边一致。
        description.entities.push_back(std::move(e));
    }

    // 场景级功能也属于场景文档：保存时总是写 sceneComponents（即使为空）。
    // 空数组是“用户把功能全删了”的明确状态，和旧文件“没有这个字段”不同。
    if (root == entt::null)
    {
        description.sceneComponents = captureSceneComponents();
        description.sceneComponentsAuthored = true;
    }

    // F3：体素世界按 seed + 差分存盘。差分用"按同一 seed 重新生成一份"对比得到，
    // 不用维护第二套 dirty 记录，也不会漏掉任何改动路径（挖、放、撤销、脚本）。
    // 预制体是子树，不能携带世界级差分，否则实例化会把整个体素世界重播一遍。
    if (root == entt::null)
    {
        for (entt::entity worldEntity : registry.view<VoxelWorldComponent>())
        {
            const VoxelWorldComponent& world = registry.get<VoxelWorldComponent>(worldEntity);
            scene::VoxelWorldDescription worldDesc;
            worldDesc.seed = world.seed;
            worldDesc.radiusChunks = world.radiusChunks;
            worldDesc.baseY = world.baseY;

            for (auto [chunkEntity, chunkComp] : registry.view<VoxelChunkComponent>().each())
            {
                (void)chunkEntity;
                if (chunkComp.world != worldEntity || !chunkComp.chunk)
                    continue;

                voxel::Chunk reference;
                reference.generate(world.seed, chunkComp.chunkX, chunkComp.chunkZ, world.baseY);

                scene::VoxelChunkDescription chunkDesc;
                chunkDesc.chunkX = chunkComp.chunkX;
                chunkDesc.chunkZ = chunkComp.chunkZ;
                for (int32_t y = 0; y < voxel::Chunk::kSize; ++y)
                    for (int32_t z = 0; z < voxel::Chunk::kSize; ++z)
                        for (int32_t x = 0; x < voxel::Chunk::kSize; ++x)
                        {
                            const uint8_t current = chunkComp.chunk->get(x, y, z);
                            if (current != reference.get(x, y, z))
                                chunkDesc.edits.push_back(scene::VoxelBlockEdit{ x, y, z, current });
                        }

                if (!chunkDesc.edits.empty())
                    worldDesc.chunks.push_back(std::move(chunkDesc));
            }

            description.voxelWorlds.push_back(std::move(worldDesc));
        }
    }

    return description;
}

bool Scene::applyMeshAndMaterialFromDescription(const scene::EntityDescription& e,
                                                      entt::entity entity,
                                                      std::string& errorMessage)
{
    if (entity == entt::null || !registry.valid(entity))
    {
        errorMessage = "invalid entity while applying prefab mesh";
        return false;
    }

    // 地形网格由 buildFromDescription 自己生成（需要 heightfield / GPU 构建器），
    // 这里不能重复加载，也不能给地形分配普通材质槽位。
    if (e.isTerrain)
        return true;

    if (!e.meshPath.empty())
    {
        std::shared_ptr<Model> model;
        try
        {
            model = Model::createModelFromFile(*device, commandPool, e.meshPath);
        }
        catch (const std::exception& ex)
        {
            errorMessage = std::string("failed to load mesh '") + e.meshPath + "': " + ex.what();
            return false;
        }
        if (!model)
        {
            errorMessage = "mesh loader returned null for '" + e.meshPath + "'";
            return false;
        }

        registry.emplace_or_replace<MeshComponent>(entity, MeshComponent{ model });
        RenderTagComponent tag;
        tag.isSkybox = e.isSkybox;
        tag.isGrid = e.isGrid;
        registry.emplace_or_replace<RenderTagComponent>(entity, tag);
        setModelBounds(registry, entity, model, &dirtySpatialEntities);
    }
    else if (e.isSkybox || e.isGrid)
    {
        // 天空盒/网格辅助实体只有 tag，没有 MeshComponent。
        RenderTagComponent tag;
        tag.isSkybox = e.isSkybox;
        tag.isGrid = e.isGrid;
        registry.emplace_or_replace<RenderTagComponent>(entity, tag);
    }

    if (registry.try_get<MeshComponent>(entity))
    {
        MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(entity);
        materialComp.material = materialSystem.getOrCreateMaterial(
            e.material.present ? e.material.name : std::string("scene_default"));
        if (!materialComp.material)
        {
            errorMessage = "material system returned null for '" + e.material.name + "'";
            return false;
        }

        materialComp.material->baseColorFactor = e.material.baseColorFactor;
        materialComp.material->metallicFactor = e.material.metallicFactor;
        materialComp.material->roughnessFactor = e.material.roughnessFactor;
        materialComp.material->alphaCutoff = e.material.alphaCutoff;
        materialComp.material->alphaMode = static_cast<AlphaMode>(e.material.alphaMode);

        auto loadIfAny = [&](const std::string& path, bool srgb) -> std::shared_ptr<Texture>
        {
            if (path.empty())
                return nullptr;
            return materialSystem.loadTexture(path, *device, commandPool, srgb);
        };
        if (!e.material.albedoMap.empty())
            materialComp.material->albedoMap = loadIfAny(e.material.albedoMap, true);
        if (!e.material.normalMap.empty())
            materialComp.material->normalMap = loadIfAny(e.material.normalMap, false);
        if (!e.material.metallicRoughnessMap.empty())
            materialComp.material->metallicRoughnessMap = loadIfAny(e.material.metallicRoughnessMap, false);

        // 槽位是运行期分发的，序列化不存；每次实例化都要重新分配。
        materialComp.materialSetIndex = descriptorManager->allocateMaterialSet(*materialComp.material);
    }

    return true;
}

entt::entity Scene::instantiatePrefab(const scene::SceneDescription& description,
                                      entt::entity parent,
                                      std::vector<entt::entity>& createdEntities,
                                      std::string& errorMessage)
{
    createdEntities.clear();
    errorMessage.clear();

    if (!device || !descriptorManager)
    {
        errorMessage = "scene is not initialized";
        return entt::null;
    }
    if (description.entities.empty())
    {
        errorMessage = "prefab has no entities";
        return entt::null;
    }
    if (!description.voxelWorlds.empty())
    {
        errorMessage = "prefabs cannot contain voxel world descriptions";
        return entt::null;
    }
    if (parent != entt::null && !registry.valid(parent))
    {
        errorMessage = "prefab parent entity is invalid";
        return entt::null;
    }

    const size_t count = description.entities.size();
    int rootIndex = -1;
    std::vector<int> parentIndices(count, -1);

    for (size_t i = 0; i < count; ++i)
    {
        const scene::EntityDescription& e = description.entities[i];
        if (e.isCamera || e.isTerrain || e.isTerrainWorld || e.isSkybox || e.isGrid)
        {
            errorMessage = "prefab contains a runtime-only entity";
            return entt::null;
        }

        const int parentIndex = e.parentIndex;
        if (parentIndex < -1 || parentIndex >= static_cast<int>(count))
        {
            errorMessage = "prefab has an out-of-range parent index";
            return entt::null;
        }
        if (parentIndex == static_cast<int>(i))
        {
            errorMessage = "prefab entity cannot parent itself";
            return entt::null;
        }
        if (parentIndex < 0)
        {
            if (rootIndex >= 0)
            {
                errorMessage = "prefab must have exactly one root entity";
                return entt::null;
            }
            rootIndex = static_cast<int>(i);
        }
        parentIndices[i] = parentIndex;
    }

    if (rootIndex < 0)
    {
        errorMessage = "prefab has no root entity";
        return entt::null;
    }

    // 防止手改 JSON 造成的环（A→B→A）。每个实体最多走 count 步。
    for (size_t i = 0; i < count; ++i)
    {
        int cursor = parentIndices[i];
        size_t steps = 0;
        while (cursor >= 0)
        {
            if (++steps > count)
            {
                errorMessage = "prefab hierarchy contains a cycle";
                return entt::null;
            }
            cursor = parentIndices[static_cast<size_t>(cursor)];
        }
    }

    const entt::entity nullEntity = entt::null;
    std::vector<entt::entity> created(count, nullEntity);

    auto cleanup = [&]()
    {
        // 逆序销毁：子节点先走，destroyEntity 不会递归整棵子树。
        for (auto it = createdEntities.rbegin(); it != createdEntities.rend(); ++it)
        {
            if (registry.valid(*it))
                destroyEntity(*it);
        }
        createdEntities.clear();
    };

    try
    {
        for (size_t i = 0; i < count; ++i)
        {
            const scene::EntityDescription& e = description.entities[i];
            const entt::entity entity = registry.create();
            created[i] = entity;
            createdEntities.push_back(entity);

            TransformComponent transform;
            transform.setTranslation(e.translation);
            transform.setEuler(e.rotation);
            transform.setScale(e.scale);
            registry.emplace<TransformComponent>(entity, transform);
            registry.emplace<HierarchyComponent>(entity);
            registry.emplace<NameComponent>(entity, NameComponent{ e.name });

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
                registry.emplace<CharacterControllerComponent>(entity, character);
            }

            // 角色与网格可以同时存在：这里不再用 else if，修掉了旧载入路径里
            // “角色带 meshPath 时网格被吞掉”的问题。
            if (!applyMeshAndMaterialFromDescription(e, entity, errorMessage))
            {
                cleanup();
                return entt::null;
            }
        }

        for (entt::entity entity : created)
            attachDirtyTracking(entity);

        for (size_t i = 0; i < count; ++i)
        {
            const scene::EntityDescription& e = description.entities[i];
            for (const scene::ComponentRecord& record : e.components)
            {
                if (!applyComponentRecord(created[i], record))
                {
                    errorMessage = "failed to apply component '" + record.type + "'";
                    cleanup();
                    return entt::null;
                }
            }
        }

        for (size_t i = 0; i < count; ++i)
        {
            if (parentIndices[i] >= 0)
                setParent(created[i], created[static_cast<size_t>(parentIndices[i])]);
        }

        const entt::entity root = created[static_cast<size_t>(rootIndex)];
        if (parent != entt::null)
            setParent(root, parent);

        markAllTransformsDirty();
        updateWorldTransforms();
        markAllSpatialDirty();
        refreshSpatialIndex();
        updateVisibility(camera.getProjection() * camera.getView());
        return root;
    }
    catch (const std::exception& ex)
    {
        errorMessage = std::string("prefab instantiation failed: ") + ex.what();
        cleanup();
        return entt::null;
    }
}

void Scene::clearEntities()
{
    discardPendingTerrainCallbacks();

    // F3-g：正在构建的体素 job 不能在这里直接 clear —— 作业 lambda 持有
    // PendingVoxelChunk* 的裸指针，提前销毁 vector 会 UAF。先打取消标记、
    // 摘掉实体 id，让作业自然结束；processVoxelChunkBuilds() 回收时跳过应用。
    for (const std::shared_ptr<PendingVoxelChunk>& pending : pendingVoxelBuilds)
    {
        pending->cancelled = true;
        pending->entity = entt::null;
    }

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
        // 角色组件与网格/材质是两条独立的路径：角色也可以带 meshPath。
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

        // 网格 / RenderTag / Bounds / 材质统一走与预制体相同的 helper。
        if (created[i] != entt::null && registry.valid(created[i]) &&
            !applyMeshAndMaterialFromDescription(e, created[i], errorMessage))
        {
            clearEntities();
            return false;
        }
    }

    for (entt::entity entity : created)
        attachDirtyTracking(entity);

    // 通用组件：相机实体会被复用，先把本次描述之外的注册组件摘掉，保证
    // "载入后的实体 = 文件里的实体"，不会残留上一个场景的 Spin/Orbit。
    for (size_t i = 0; i < description.entities.size(); ++i)
    {
        const entt::entity entity = created[i];
        if (entity == entt::null || !registry.valid(entity))
            continue;

        const scene::EntityDescription& e = description.entities[i];
        for (const scene::ComponentDescriptor& descriptor : componentRegistry.descriptors())
        {
            // 原生组件由专用字段重建（角色/渲染标签/地形……），不参与这段
            // “文件里没有就移除”的通用清理，否则会误删复用相机上的组件。
            if (!descriptor.serialized)
                continue;

            const bool inDescription = std::any_of(
                e.components.begin(), e.components.end(),
                [&descriptor](const scene::ComponentRecord& record)
                {
                    return record.type == descriptor.type;
                });
            if (!inDescription && descriptor.has(registry, entity))
                descriptor.remove(registry, entity);
        }

        for (const scene::ComponentRecord& record : e.components)
        {
            const scene::ComponentDescriptor* descriptor =
                componentRegistry.find(record.type);
            if (descriptor != nullptr && !descriptor->serialized)
            {
                std::cerr << "[Scene] ignoring non-serialized component '" << record.type
                          << "' while loading scene\n";
                continue;
            }
            if (!applyComponentRecord(entity, record))
            {
                std::cerr << "[Scene] failed to apply component '" << record.type
                          << "' while loading scene\n";
            }
        }
    }

    // 场景级组件：文件里明确写过 sceneComponents 就按文件恢复（空数组 =
    // 全部移除）；旧文件没有该字段时从当前 EngineSettings 初始化，画面不变。
    if (description.sceneComponentsAuthored)
    {
        scene::clearSceneComponents(registry, sceneSettingsEntity);
        for (const scene::ComponentRecord& record : description.sceneComponents)
        {
            const scene::ComponentDescriptor* descriptor =
                componentRegistry.find(record.type);
            if (descriptor == nullptr || descriptor->scope != scene::ComponentScope::Scene)
            {
                std::cerr << "[Scene] ignoring non-scene component '" << record.type
                          << "' in sceneComponents\n";
                continue;
            }
            if (!applyComponentRecord(sceneSettingsEntity, record))
            {
                std::cerr << "[Scene] failed to apply scene component '" << record.type
                          << "' while loading scene\n";
            }
        }
    }
    else
    {
        resetSceneComponentsFromSettings();
    }
    syncSceneSettings();

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

    // F3：体素世界 —— 先按 seed 生成（createVoxelWorld 内部生成 + 网格化），
    // 再把差分逐块写回，每个受影响的 chunk 只重建一次网格。
    for (const scene::VoxelWorldDescription& worldDesc : description.voxelWorlds)
    {
        const entt::entity worldEntity = createVoxelWorld(
            worldDesc.seed, worldDesc.radiusChunks, worldDesc.baseY);
        waitForVoxelBuilds();
        if (worldEntity == entt::null)
        {
            errorMessage = "voxel world rebuild failed";
            clearEntities();
            return false;
        }

        for (const scene::VoxelChunkDescription& chunkDesc : worldDesc.chunks)
        {
            const entt::entity chunkEntity = findVoxelChunk(worldEntity, chunkDesc.chunkX, chunkDesc.chunkZ);
            if (chunkEntity == entt::null)
                continue;
            auto& chunkComp = registry.get<VoxelChunkComponent>(chunkEntity);
            if (!chunkComp.chunk)
                continue;
            for (const scene::VoxelBlockEdit& edit : chunkDesc.edits)
                chunkComp.chunk->set(edit.x, edit.y, edit.z, edit.block);
            rebuildVoxelChunk(chunkEntity);
        }
    }
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

bool Scene::applyVoxelChunkMesh(entt::entity entity, const terrain::TerrainMeshData& mesh,
	const voxel::MeshStats& stats, const std::shared_ptr<terrain::TerrainMeshData>& glassMesh,
	const voxel::GlassStats& glassStats, const std::shared_ptr<Material>& material)
{
	if (entity == entt::null || !registry.valid(entity))
		return false;

	const bool hasOpaque = !mesh.vertices.empty();
	const bool hasGlass = glassMesh && !glassMesh->vertices.empty();

	// V-01：只有玻璃的 chunk 也是合法状态 —— 不建不透明 Model，只把玻璃网格
	// 挂在组件上；等 GlassPass 接上后直接消费，不透明管线完全不碰它。
	std::shared_ptr<Model> model;
	uint32_t arenaVertexBase = TerrainMeshArena::kInvalidOffset;
	uint32_t arenaIndexBase = TerrainMeshArena::kInvalidOffset;
	uint32_t arenaVertexCount = 0u;
	uint32_t arenaIndexCount = 0u;
	if (hasOpaque)
	{
		try
		{
			Model::Builder builder;
			builder.vertices.reserve(mesh.vertices.size());
			for (const terrain::TerrainVertex& vertex : mesh.vertices)
				builder.vertices.push_back(Vertex{ vertex.position, vertex.color, vertex.normal, vertex.uv });
			builder.indices = mesh.indices;
			builder.subMeshes.reserve(mesh.subMeshes.size());
			for (const terrain::TerrainSubMesh& source : mesh.subMeshes)
			{
				SubMesh subMesh;
				subMesh.firstIndex = source.firstIndex;
				subMesh.indexCount = source.indexCount;
				subMesh.hasBounds = true;
				subMesh.boundsMin = source.boundsMin;
				subMesh.boundsMax = source.boundsMax;
				builder.subMeshes.push_back(subMesh);
			}
			model = std::make_shared<Model>(*device, commandPool, builder);
		}
		catch (const std::exception& ex)
		{
			std::cerr << "[Voxel] chunk mesh upload failed: " << ex.what() << std::endl;
			return false;
		}
		if (!model)
			return false;

		// V-04：不透明几何优先拷进共享 arena，Model 只保留 CPU 几何给拾取/编辑。
		// 失败时保留 Model 自己的 GPU 缓冲，下面仍走逐实体绘制，不会丢画面。
		if (terrainMeshArena && terrainMeshArena->isAvailable() && model->hasGpuBuffers())
		{
			TerrainMeshArena::Slice slice{};
			if (terrainMeshArena->acquire(static_cast<uint32_t>(mesh.vertices.size()),
			                              static_cast<uint32_t>(mesh.indices.size()), slice))
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
					arenaVertexBase = slice.vertexOffset;
					arenaIndexBase = slice.indexOffset;
					arenaVertexCount = slice.vertexCount;
					arenaIndexCount = slice.indexCount;
					model->releaseGpuBuffers();
				}
				else
				{
					terrainMeshArena->release(slice);
				}
			}
		}
	}

	// 重建前归还旧切片；否则编辑体素会把 arena 一点点吃满。
	if (auto* previous = registry.try_get<VoxelChunkComponent>(entity))
	{
		if (terrainMeshArena && previous->arenaIndexBase != TerrainMeshArena::kInvalidOffset)
		{
			TerrainMeshArena::Slice slice{};
			slice.vertexOffset = previous->arenaVertexBase;
			slice.vertexCount = previous->arenaVertexCount;
			slice.indexOffset = previous->arenaIndexBase;
			slice.indexCount = previous->arenaIndexCount;
			terrainMeshArena->release(slice);
		}
		previous->arenaVertexBase = TerrainMeshArena::kInvalidOffset;
		previous->arenaIndexBase = TerrainMeshArena::kInvalidOffset;
		previous->arenaVertexCount = 0u;
		previous->arenaIndexCount = 0u;
	}

	if (model)
	{
		registry.emplace_or_replace<MeshComponent>(entity, MeshComponent{ model });
	}
	else if (const auto* oldMesh = registry.try_get<MeshComponent>(entity))
	{
		// V-01/V-04：块内不透明方块全被挖掉后只剩玻璃，旧的不透明网格必须移除，
		// 否则它会继续画着已经不存在的地形。
		retireModel(oldMesh->model);
		registry.remove<MeshComponent>(entity);
	}
	// V-01：玻璃也必须有材质槽 —— GlassPass 从 materialSetIndex 取图集/因子。
	// 只有玻璃的 chunk 没有 MeshComponent，但材质照旧要分配（否则没有槽位可采样）。
	if (model || hasGlass)
	{
		MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(entity);
		materialComp.material = material;
		if (materialComp.materialSetIndex == kInvalidMaterialSlot)
			materialComp.materialSetIndex = descriptorManager->allocateMaterialSet(*material);
	}
	if (auto* chunkComp = registry.try_get<VoxelChunkComponent>(entity))
	{
		chunkComp->revision += 1;
		chunkComp->faceCount = stats.faces;
		chunkComp->glassFaceCount = glassStats.faces;
		chunkComp->glassMesh = hasGlass ? glassMesh : nullptr;
		chunkComp->arenaVertexBase = arenaVertexBase;
		chunkComp->arenaIndexBase = arenaIndexBase;
		chunkComp->arenaVertexCount = arenaVertexCount;
		chunkComp->arenaIndexCount = arenaIndexCount;
	}
	if (model)
	{
		// F3-g 异步路径：chunk 实体在 createVoxelWorld() 里已经刷新过一次变换，
		// 作业完成时它的 transform/bounds 都不再是脏的。这里必须写进 Scene 持久的
		// dirtySpatialEntities，否则 refreshSpatialIndex() 不会生成 WorldBoundsComponent，
		// updateVisibility() 会把所有异步上传的 chunk 当成不可见剔除（编辑器打开
		// 体素示例全黑、draw calls=0 就是这个原因）。
		setModelBounds(registry, entity, model, &dirtySpatialEntities);
		attachDirtyTracking(entity);
	}
	return true;
}

entt::entity Scene::createVoxelWorld(uint64_t seed, int32_t radiusChunks, int32_t baseY)
{
	radiusChunks = std::clamp(radiusChunks, 1, kMaxVoxelRadius);

	const entt::entity world = createTransformEntity(registry);
	markTransformDirty(world);
	registry.emplace<NameComponent>(world, NameComponent{ "Voxel World" });
	VoxelWorldComponent& worldComp = registry.emplace<VoxelWorldComponent>(world);
	worldComp.seed = seed;
	worldComp.radiusChunks = radiusChunks;
	worldComp.baseY = baseY;

	std::shared_ptr<Material> material = materialSystem.getOrCreateMaterial("voxel");
	material->albedoMap = materialSystem.getVoxelAtlasTexture();
	material->metallicFactor = 0.0f;
	material->roughnessFactor = 0.9f;
	material->shaderFlags = 1.0f;

	const bool asyncBuild = jobSystem != nullptr;
	worldComp.building = asyncBuild;
	worldComp.pendingChunks = 0;

	const auto submitStart = std::chrono::steady_clock::now();
	int32_t created = 0;
	for (int32_t chunkZ = -radiusChunks; chunkZ <= radiusChunks; ++chunkZ)
	{
		for (int32_t chunkX = -radiusChunks; chunkX <= radiusChunks; ++chunkX)
		{
			const entt::entity entity = createTransformEntity(registry);
			// 场景树：chunk 必须挂在世界节点下。否则 Hierarchy 会把它显示成一排
			// 与 Voxel World 并列的根节点；挂成子树后也能按父节点统一移动/删除。
			setParent(entity, world);
			markTransformDirty(entity);
			registry.get<TransformComponent>(entity).setTranslation(glm::vec3(
				static_cast<float>(chunkX * voxel::Chunk::kSize), static_cast<float>(baseY),
				static_cast<float>(chunkZ * voxel::Chunk::kSize)));
			registry.emplace<NameComponent>(entity, NameComponent{
				"Voxel Chunk " + std::to_string(chunkX) + "," + std::to_string(chunkZ) });
			registry.emplace<RenderTagComponent>(entity);

			std::shared_ptr<voxel::Chunk> chunk = std::make_shared<voxel::Chunk>();
			VoxelChunkComponent& chunkComp = registry.emplace<VoxelChunkComponent>(entity);
			chunkComp.world = world;
			chunkComp.chunkX = chunkX;
			chunkComp.chunkZ = chunkZ;
			chunkComp.baseY = baseY;
			chunkComp.chunk = chunk;
			chunkComp.revision = 0;
			chunkComp.faceCount = 0;

			if (asyncBuild)
			{
				auto pending = std::make_shared<PendingVoxelChunk>();
				pending->entity = entity;
				pending->chunk = chunk;
				pending->chunkX = chunkX;
				pending->chunkZ = chunkZ;
				pending->baseY = baseY;
				pending->mesh = std::make_shared<terrain::TerrainMeshData>();
				pending->glassMesh = std::make_shared<terrain::TerrainMeshData>();
				PendingVoxelChunk* raw = pending.get();
				pending->handle = jobSystem->submit([raw, seed]()
				{
					raw->chunk->generate(seed, raw->chunkX, raw->chunkZ, raw->baseY);
					voxel::buildMesh(*raw->chunk, *raw->mesh, &raw->stats);
					voxel::buildGlassMesh(*raw->chunk, *raw->glassMesh, &raw->glassStats);
				});
				pendingVoxelBuilds.push_back(std::move(pending));
				worldComp.pendingChunks += 1;
			}
			else
			{
				chunk->generate(seed, chunkX, chunkZ, baseY);
				terrain::TerrainMeshData mesh;
				voxel::MeshStats stats;
				std::shared_ptr<terrain::TerrainMeshData> glassMesh = std::make_shared<terrain::TerrainMeshData>();
				voxel::GlassStats glassStats;
				if (voxel::buildMesh(*chunk, mesh, &stats) &&
					voxel::buildGlassMesh(*chunk, *glassMesh, &glassStats))
					applyVoxelChunkMesh(entity, mesh, stats, glassMesh, glassStats, material);
			}
			++created;
		}
	}

	// 场景树展示顺序按 chunk 坐标固定下来：Async 完成顺序、存档重建顺序都不该
	// 影响 Hierarchy 里看到的行序，否则同一世界每次打开列表顺序都不一样。
	if (auto* worldHierarchy = registry.try_get<HierarchyComponent>(world))
	{
		std::stable_sort(worldHierarchy->children.begin(), worldHierarchy->children.end(),
			[this](entt::entity lhs, entt::entity rhs)
			{
				const auto* a = registry.try_get<VoxelChunkComponent>(lhs);
				const auto* b = registry.try_get<VoxelChunkComponent>(rhs);
				if (a == nullptr || b == nullptr)
					return lhs < rhs;
				if (a->chunkZ != b->chunkZ)
					return a->chunkZ < b->chunkZ;
				return a->chunkX < b->chunkX;
			});
	}

	updateWorldTransforms();
	refreshSpatialIndex();
	refreshTransformsAndVisibility();
	const double submitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - submitStart).count();
	std::cout << "[Voxel] world created | seed=" << seed << " radius=" << radiusChunks
		<< " chunks=" << created << " baseY=" << baseY
		<< (asyncBuild ? " async" : " sync") << " submitMs=" << submitMs << std::endl;
	return world;
}

void Scene::processVoxelChunkBuilds()
{
	if (pendingVoxelBuilds.empty())
		return;

	constexpr size_t kMaxUploadsPerFrame = 4;
	std::shared_ptr<Material> material = materialSystem.getOrCreateMaterial("voxel");
	size_t uploads = 0;
	bool finishedAny = false;

	for (auto it = pendingVoxelBuilds.begin(); it != pendingVoxelBuilds.end();)
	{
		const std::shared_ptr<PendingVoxelChunk>& pending = *it;
		if (!pending->handle.isComplete())
		{
			++it;
			continue;
		}
		pending->handle.wait();
		if (!pending->cancelled && pending->entity != entt::null && registry.valid(pending->entity))
		{
			applyVoxelChunkMesh(pending->entity, *pending->mesh, pending->stats,
				pending->glassMesh, pending->glassStats, material);
			if (auto* chunkComp = registry.try_get<VoxelChunkComponent>(pending->entity))
			{
				if (auto* worldComp = registry.try_get<VoxelWorldComponent>(chunkComp->world))
				{
					if (worldComp->pendingChunks > 0)
						worldComp->pendingChunks -= 1;
					if (worldComp->pendingChunks == 0)
					{
						worldComp->building = false;
						finishedAny = true;
						// V-04：整世界落地后统计 arena 覆盖率；容量不够时一眼能看到
						// 有多少块回退成独立 GPU 缓冲，而不是等 draw call 数偏高才猜。
						uint32_t totalChunks = 0u;
						uint32_t arenaChunks = 0u;
						uint64_t arenaVertices = 0u;
						uint64_t arenaIndices = 0u;
						for (auto [chunkEntity, stats] :
						     registry.view<VoxelChunkComponent>().each())
						{
							(void)chunkEntity;
							if (stats.world != chunkComp->world)
								continue;
							totalChunks += 1u;
							if (stats.arenaIndexBase != TerrainMeshArena::kInvalidOffset)
							{
								arenaChunks += 1u;
								arenaVertices += stats.arenaVertexCount;
								arenaIndices += stats.arenaIndexCount;
							}
						}
						std::cout << "[Voxel] world build complete | chunks=" << totalChunks
						          << " arena=" << arenaChunks << " vertices=" << arenaVertices
						          << " indices=" << arenaIndices << std::endl;
					}
				}
			}
		}
		it = pendingVoxelBuilds.erase(it);
		if (++uploads >= kMaxUploadsPerFrame)
			break;
	}

	if (finishedAny || uploads > 0)
	{
		refreshSpatialIndex();
		refreshTransformsAndVisibility();
	}
}

void Scene::waitForVoxelBuilds()
{
	while (!pendingVoxelBuilds.empty())
	{
		processVoxelChunkBuilds();
		if (!pendingVoxelBuilds.empty())
			std::this_thread::yield();
	}
}


entt::entity Scene::findVoxelChunk(entt::entity world, int32_t chunkX, int32_t chunkZ) const
{
    for (auto [entity, chunk] : registry.view<VoxelChunkComponent>().each())
    {
        if (chunk.world == world && chunk.chunkX == chunkX && chunk.chunkZ == chunkZ)
            return entity;
    }
    return entt::null;
}

uint8_t Scene::getVoxelBlock(entt::entity world, int32_t x, int32_t y, int32_t z) const
{
    const int32_t chunkX = static_cast<int32_t>(x >> voxel::Chunk::kSizeShift);
    const int32_t chunkZ = static_cast<int32_t>(z >> voxel::Chunk::kSizeShift);
    const entt::entity chunkEntity = findVoxelChunk(world, chunkX, chunkZ);
    if (chunkEntity == entt::null)
        return static_cast<uint8_t>(voxel::BlockId::Air);
    const VoxelChunkComponent& chunkComp = registry.get<VoxelChunkComponent>(chunkEntity);
    if (!chunkComp.chunk)
        return static_cast<uint8_t>(voxel::BlockId::Air);
    return chunkComp.chunk->get(x & voxel::Chunk::kSizeMask, y, z & voxel::Chunk::kSizeMask);
}

int32_t Scene::placeVoxelGlassDemo(entt::entity world)
{
    if (registry.try_get<VoxelWorldComponent>(world) == nullptr)
        return 0;

    const uint8_t glass = static_cast<uint8_t>(voxel::BlockId::Glass);
    const int32_t baseY = 12;
    std::vector<entt::entity> dirtyChunks;
    int32_t placed = 0;

    const auto put = [&](int32_t x, int32_t y, int32_t z)
    {
        const entt::entity chunkEntity = findVoxelChunk(world,
            x >> voxel::Chunk::kSizeShift, z >> voxel::Chunk::kSizeShift);
        if (chunkEntity == entt::null)
            return;
        VoxelChunkComponent& chunkComp = registry.get<VoxelChunkComponent>(chunkEntity);
        if (!chunkComp.chunk)
            return;

        const int32_t localX = x & voxel::Chunk::kSizeMask;
        const int32_t localZ = z & voxel::Chunk::kSizeMask;
        if (chunkComp.chunk->get(localX, y, localZ) == glass)
            return;
        chunkComp.chunk->set(localX, y, localZ, glass);
        ++placed;
        if (std::find(dirtyChunks.begin(), dirtyChunks.end(), chunkEntity) == dirtyChunks.end())
            dirtyChunks.push_back(chunkEntity);
    };

    // 十字交叉的两片玻璃墙：交叉处的玻璃-玻璃共面必须被剔除，
    // 从近处看不会出现"自己挡自己"的深色接缝。
    for (int32_t x = 6; x <= 26; ++x)
        for (int32_t y = baseY; y < baseY + 9; ++y)
            put(x, y, 16);
    for (int32_t z = 6; z <= 26; ++z)
        for (int32_t y = baseY; y < baseY + 9; ++y)
            put(16, y, z);

    // 更靠近相机的一片独立玻璃：两片玻璃之间的前后顺序靠这里目测。
    for (int32_t x = 10; x <= 22; ++x)
        for (int32_t y = baseY + 2; y < baseY + 7; ++y)
            put(x, y, 26);

    for (entt::entity chunkEntity : dirtyChunks)
        rebuildVoxelChunk(chunkEntity);

    return placed;
}

bool Scene::setVoxelBlock(entt::entity world, int32_t x, int32_t y, int32_t z, uint8_t blockId)
{
    const int32_t chunkX = static_cast<int32_t>(x >> voxel::Chunk::kSizeShift);
    const int32_t chunkZ = static_cast<int32_t>(z >> voxel::Chunk::kSizeShift);
    const entt::entity chunkEntity = findVoxelChunk(world, chunkX, chunkZ);
    if (chunkEntity == entt::null)
        return false;

    VoxelChunkComponent& chunkComp = registry.get<VoxelChunkComponent>(chunkEntity);
    if (!chunkComp.chunk)
        return false;

    const int32_t localX = x & voxel::Chunk::kSizeMask;
    const int32_t localZ = z & voxel::Chunk::kSizeMask;
    if (chunkComp.chunk->get(localX, y, localZ) == blockId)
        return false;
    chunkComp.chunk->set(localX, y, localZ, blockId);

    rebuildVoxelChunk(chunkEntity);

    // 边界格会改变邻块朝外的面（被挡住 vs 露出），必须一起重建。
    const int32_t maxLocal = voxel::Chunk::kSize - 1;
    const int32_t offsetX = (localX == 0) ? -1 : ((localX == maxLocal) ? 1 : 0);
    const int32_t offsetZ = (localZ == 0) ? -1 : ((localZ == maxLocal) ? 1 : 0);
    if (offsetX != 0)
    {
        const entt::entity other = findVoxelChunk(world, chunkX + offsetX, chunkZ);
        if (other != entt::null)
            rebuildVoxelChunk(other);
    }
    if (offsetZ != 0)
    {
        const entt::entity other = findVoxelChunk(world, chunkX, chunkZ + offsetZ);
        if (other != entt::null)
            rebuildVoxelChunk(other);
    }
    if (offsetX != 0 && offsetZ != 0)
    {
        const entt::entity other = findVoxelChunk(world, chunkX + offsetX, chunkZ + offsetZ);
        if (other != entt::null)
            rebuildVoxelChunk(other);
    }
    return true;
}

bool Scene::sampleVoxelGroundHeight(int32_t x, int32_t z, int32_t& outY,
                                    entt::entity* outChunk,
                                    entt::entity worldFilter) const
{
    for (auto [chunkEntity, chunkComp] : registry.view<VoxelChunkComponent>().each())
    {
        if (!chunkComp.chunk)
            continue;
        if (worldFilter != entt::null && chunkComp.world != worldFilter)
            continue;
        if (static_cast<int32_t>(x >> voxel::Chunk::kSizeShift) != chunkComp.chunkX)
            continue;
        if (static_cast<int32_t>(z >> voxel::Chunk::kSizeShift) != chunkComp.chunkZ)
            continue;

        const int32_t localX = x & voxel::Chunk::kSizeMask;
        const int32_t localZ = z & voxel::Chunk::kSizeMask;
        for (int32_t y = voxel::Chunk::kSize - 1; y >= 0; --y)
        {
            if (voxel::isSolid(chunkComp.chunk->get(localX, y, localZ)))
            {
                outY = chunkComp.baseY + y + 1;
                if (outChunk)
                    *outChunk = chunkEntity;
                return true;
            }
        }
    }
    return false;
}

bool Scene::raycastVoxelWorld(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                              float maxDistance, glm::ivec3& outBlock,
                              glm::ivec3& outNormal, float& outDistance,
                              entt::entity* outChunk,
                              entt::entity worldFilter) const
{
    outDistance = maxDistance;
    bool hit = false;

    glm::vec3 direction = rayDirection;
    const float directionLength = glm::length(direction);
    if (directionLength < 1e-6f)
        return false;
    direction /= directionLength;

    // 逐 chunk 先做 AABB 粗筛，再在命中的 chunk 内做 Amanatides-Woo DDA。
    // chunk 数量只有几十，线性遍历足够。
    for (auto [chunkEntity, chunkComp] : registry.view<VoxelChunkComponent>().each())
    {
        if (!chunkComp.chunk)
            continue;
        if (worldFilter != entt::null && chunkComp.world != worldFilter)
            continue;

        const float size = static_cast<float>(voxel::Chunk::kSize);
        const glm::vec3 chunkMin(static_cast<float>(chunkComp.chunkX) * size,
                                 static_cast<float>(chunkComp.baseY),
                                 static_cast<float>(chunkComp.chunkZ) * size);
        const glm::vec3 chunkMax = chunkMin + glm::vec3(size);

        float entry = 0.0f;
        float exitDistance = maxDistance;
        bool intersects = true;
        int entryAxis = -1;
        for (int axis = 0; axis < 3 && intersects; ++axis)
        {
            const float origin = rayOrigin[axis];
            const float dir = direction[axis];
            if (std::abs(dir) < 1e-8f)
            {
                if (origin < chunkMin[axis] || origin > chunkMax[axis])
                    intersects = false;
                continue;
            }
            float near = (chunkMin[axis] - origin) / dir;
            float far = (chunkMax[axis] - origin) / dir;
            if (near > far)
                std::swap(near, far);
            if (near > entry)
            {
                entry = near;
                entryAxis = axis;
            }
            exitDistance = std::min(exitDistance, far);
            if (entry > exitDistance)
                intersects = false;
        }
        if (!intersects)
            continue;

        // 起点抬进 chunk 内部一点，避免浮点恰好落在边界格。
        float t = std::max(entry, 0.0f) + 1e-4f;
        const glm::vec3 position = rayOrigin + direction * t;
        glm::ivec3 cell(static_cast<int32_t>(std::floor(position.x)),
                        static_cast<int32_t>(std::floor(position.y)),
                        static_cast<int32_t>(std::floor(position.z)));
        // 命中格的外法线必须是轴对齐的：放置方块落在“鼠标指向的那一面”外侧。
        // 早期版本直接写 -step，射线斜着走时会得到 (-1,-1,-1) 这样的对角偏移，
        // 方块就会出现在鼠标旁边的斜角上。
        glm::ivec3 faceNormal(0);
        if (entry > 0.0f && entryAxis >= 0)
            faceNormal[entryAxis] = (direction[entryAxis] > 0.0f) ? -1 : 1;
        glm::ivec3 step(0);
        glm::vec3 next(0.0f);
        glm::vec3 delta(0.0f);
        for (int axis = 0; axis < 3; ++axis)
        {
            const float dir = direction[axis];
            if (std::abs(dir) < 1e-8f)
            {
                step[axis] = 0;
                next[axis] = std::numeric_limits<float>::max();
                delta[axis] = std::numeric_limits<float>::max();
                continue;
            }
            step[axis] = (dir > 0.0f) ? 1 : -1;
            const float boundary = static_cast<float>(cell[axis] + (dir > 0.0f ? 1 : 0));
            next[axis] = (boundary - position[axis]) / dir;
            delta[axis] = std::abs(1.0f / dir);
        }

        const glm::ivec3 chunkCellMin(chunkComp.chunkX * voxel::Chunk::kSize,
                                      chunkComp.baseY,
                                      chunkComp.chunkZ * voxel::Chunk::kSize);
        const glm::ivec3 chunkCellMax = chunkCellMin + glm::ivec3(voxel::Chunk::kSize);

        while (true)
        {
            if (cell.x < chunkCellMin.x || cell.x >= chunkCellMax.x ||
                cell.z < chunkCellMin.z || cell.z >= chunkCellMax.z ||
                cell.y < chunkCellMin.y || cell.y >= chunkCellMax.y)
                break;

            const uint8_t block = chunkComp.chunk->get(cell.x - chunkCellMin.x,
                                                       cell.y - chunkCellMin.y,
                                                       cell.z - chunkCellMin.z);
            if (voxel::isSolid(block))
            {
                if (t < outDistance)
                {
                    outDistance = t;
                    outBlock = cell;
                    glm::ivec3 normal = faceNormal;
                    if (normal == glm::ivec3(0))
                    {
                        // 射线起点已经在方块内部（贴脸挖）：没有"进来的那一面"，
                        // 用视线主轴反向兜底，保证放置位置仍是相邻格而不是原地。
                        const glm::vec3 a = glm::abs(direction);
                        const int axis = (a.x > a.y) ? ((a.x > a.z) ? 0 : 2)
                                                        : ((a.y > a.z) ? 1 : 2);
                        normal[axis] = (direction[axis] > 0.0f) ? -1 : 1;
                    }
                    outNormal = normal;
                    if (outChunk)
                        *outChunk = chunkEntity;
                    hit = true;
                }
                break;
            }

            const int axis = (next.x < next.y) ? ((next.x < next.z) ? 0 : 2)
                                               : ((next.y < next.z) ? 1 : 2);
            t = next[axis];
            if (t > exitDistance || t > maxDistance)
                break;
            cell[axis] += step[axis];
            next[axis] += delta[axis];
            faceNormal = glm::ivec3(0);
            faceNormal[axis] = (step[axis] > 0) ? -1 : 1;
        }
    }

    return hit;
}

bool Scene::voxelSelfTest()
{
    if (!device || !descriptorManager)
        return false;

    const entt::entity world = createVoxelWorld(4242ull, 1, 0);
    waitForVoxelBuilds();
    if (world == entt::null)
    {
        std::cerr << "[Voxel] selfTest: createVoxelWorld failed" << std::endl;
        return false;
    }

    uint32_t chunkCount = 0;
    for (auto [entity, chunk] : registry.view<VoxelChunkComponent>().each())
    {
        if (chunk.world == world)
            ++chunkCount;
    }

    // F3-g 异步构建：作业全部回收后，每个 chunk 都该挂上 MeshComponent，
    // 世界组件也要退出 building 状态，否则就是"占位实体永远不落地"。
    size_t meshedChunks = 0;
    size_t boundedChunks = 0;
    for (auto [entity, chunk] : registry.view<VoxelChunkComponent>().each())
    {
        if (chunk.world != world)
            continue;
        if (registry.all_of<MeshComponent>(entity))
            ++meshedChunks;
        if (registry.all_of<WorldBoundsComponent>(entity))
            ++boundedChunks;
    }
    bool buildingDone = false;
    if (const auto* worldComp = registry.try_get<VoxelWorldComponent>(world))
        buildingDone = !worldComp->building && worldComp->pendingChunks == 0;

    const auto cleanup = [this, world]()
    {
        std::vector<entt::entity> toDestroy;
        for (auto [entity, chunk] : registry.view<VoxelChunkComponent>().each())
        {
            if (chunk.world == world)
                toDestroy.push_back(entity);
        }
        for (entt::entity entity : toDestroy)
            destroyEntity(entity);
        destroyEntity(world);
    };

    if (meshedChunks != chunkCount || boundedChunks != chunkCount || !buildingDone)
    {
        // bounding 缺失 = updateVisibility() 会把 chunk 全部剔除，画面全黑、
        // draw calls=0。这个断言专门守住 F3-g 异步上传后的空间索引登记。
        std::cerr << "[Voxel] selfTest: async build incomplete (meshed=" << meshedChunks
                  << " bounded=" << boundedChunks << " expected=" << chunkCount
                  << " buildingDone=" << buildingDone << ")" << std::endl;
        cleanup();
        return false;
    }

    if (chunkCount != 9)
    {
        std::cerr << "[Voxel] selfTest: expected 9 chunks, got " << chunkCount << std::endl;
        cleanup();
        return false;
    }

    // 从高空垂直向下打：必须先命中草皮方块，法线朝上。
    glm::ivec3 hitBlock(0);
    glm::ivec3 hitNormal(0);
    float hitDistance = 0.0f;
    const glm::vec3 rayOrigin(16.5f, 60.0f, 16.5f);
    const bool hitGround = raycastVoxelWorld(rayOrigin, glm::vec3(0.0f, -1.0f, 0.0f),
                                             100.0f, hitBlock, hitNormal, hitDistance,
                                             nullptr, world);
    if (!hitGround || hitNormal != glm::ivec3(0, 1, 0))
    {
        std::cerr << "[Voxel] selfTest: raycast did not hit ground from above" << std::endl;
        cleanup();
        return false;
    }

    // 斜射：命中面的外法线必须是轴对齐的（只有一个分量是 ±1）。
    // 之前直接写 -step，斜着看的时候会得到 (-1,-1,-1)，右键放的方块就会
    // 落到命中格的对角位置 —— 用户报的"生成位置和鼠标不一致"就是这个。
    {
        glm::ivec3 sideBlock(0);
        glm::ivec3 sideNormal(0);
        float sideDistance = 0.0f;
        const glm::vec3 sideOrigin(0.5f, 40.0f, 0.5f);
        const glm::vec3 sideDir = glm::normalize(glm::vec3(1.0f, -0.65f, 1.0f));
        const bool sideHit = raycastVoxelWorld(sideOrigin, sideDir, 200.0f, sideBlock,
                                               sideNormal, sideDistance, nullptr, world);
        const int components = (sideNormal.x != 0 ? 1 : 0) + (sideNormal.y != 0 ? 1 : 0) +
                               (sideNormal.z != 0 ? 1 : 0);
        const int magnitude = std::abs(sideNormal.x) + std::abs(sideNormal.y) +
                              std::abs(sideNormal.z);
        const glm::ivec3 placeBlock = sideBlock + sideNormal;
        const int manhattan = std::abs(placeBlock.x - sideBlock.x) +
                              std::abs(placeBlock.y - sideBlock.y) +
                              std::abs(placeBlock.z - sideBlock.z);
        if (!sideHit || components != 1 || magnitude != 1 || manhattan != 1)
        {
            std::cerr << "[Voxel] selfTest: diagonal ray normal must be axis aligned ("
                      << sideNormal.x << "," << sideNormal.y << "," << sideNormal.z << ")"
                      << std::endl;
            cleanup();
            return false;
        }
    }

    const uint8_t original = getVoxelBlock(world, hitBlock.x, hitBlock.y, hitBlock.z);
    if (!voxel::isSolid(original))
    {
        std::cerr << "[Voxel] selfTest: hit block is not solid" << std::endl;
        cleanup();
        return false;
    }

    // V-04：编辑必须重建 arena 切片。早期 rebuildVoxelChunk() 自己建 Model、
    // 不归还旧切片，arenaIndexBase 仍指向旧几何，画面会停在编辑前。
    const int32_t editChunkX = static_cast<int32_t>(hitBlock.x >> voxel::Chunk::kSizeShift);
    const int32_t editChunkZ = static_cast<int32_t>(hitBlock.z >> voxel::Chunk::kSizeShift);
    const entt::entity editedChunk = findVoxelChunk(world, editChunkX, editChunkZ);
    uint32_t arenaIndexBefore = TerrainMeshArena::kInvalidOffset;
    if (editedChunk != entt::null)
    {
        if (const VoxelChunkComponent* edited = registry.try_get<VoxelChunkComponent>(editedChunk))
            arenaIndexBefore = edited->arenaIndexBase;
    }
    const bool hadArenaSlice = arenaIndexBefore != TerrainMeshArena::kInvalidOffset;

    // 挖掉这块之后同一射线必须打到更低的方块（深度必须变大）。
    if (!setVoxelBlock(world, hitBlock.x, hitBlock.y, hitBlock.z,
                       static_cast<uint8_t>(voxel::BlockId::Air)))
    {
        std::cerr << "[Voxel] selfTest: setVoxelBlock(Air) failed" << std::endl;
        cleanup();
        return false;
    }
    glm::ivec3 dugBlock(0);
    glm::ivec3 dugNormal(0);
    float dugDistance = 0.0f;
    const bool hitAfterDig = raycastVoxelWorld(rayOrigin, glm::vec3(0.0f, -1.0f, 0.0f),
                                               100.0f, dugBlock, dugNormal, dugDistance,
                                               nullptr, world);
    const bool dugDeeper = hitAfterDig &&
                           (dugBlock.y < hitBlock.y) &&
                           (dugDistance > hitDistance);
    bool arenaRebuilt = false;
    if (editedChunk != entt::null)
    {
        if (const VoxelChunkComponent* edited = registry.try_get<VoxelChunkComponent>(editedChunk))
        {
            arenaRebuilt = hadArenaSlice &&
                           edited->arenaIndexBase != TerrainMeshArena::kInvalidOffset &&
                           edited->arenaIndexBase != arenaIndexBefore;
        }
    }
    if (!arenaRebuilt)
    {
        std::cerr << "[Voxel] selfTest: edited chunk arena slice not rebuilt (before="
                  << arenaIndexBefore << " hadSlice=" << (hadArenaSlice ? 1 : 0)
                  << " chunk=" << static_cast<uint32_t>(editedChunk) << ")" << std::endl;
    }

    // 放回去（模拟"建造"），同一位置必须恢复成原来的方块。
    if (!setVoxelBlock(world, hitBlock.x, hitBlock.y, hitBlock.z, original))
    {
        std::cerr << "[Voxel] selfTest: setVoxelBlock(restore) failed" << std::endl;
        cleanup();
        return false;
    }
    const bool restored = getVoxelBlock(world, hitBlock.x, hitBlock.y, hitBlock.z) == original;

    // 地面高度查询必须与射线命中的方块一致。
    int32_t groundY = 0;
    const bool sampled = sampleVoxelGroundHeight(hitBlock.x, hitBlock.z, groundY,
                                                 nullptr, world);
    const bool groundMatches = sampled && groundY == hitBlock.y + 1;

    // F3-b：在命中面上"盖一格"，同一列的地面高度必须随之抬升。
    bool placedOnFace = false;
    {
        const int32_t placeY = hitBlock.y + hitNormal.y;
        const uint8_t stone = static_cast<uint8_t>(voxel::BlockId::Stone);
        if (getVoxelBlock(world, hitBlock.x, placeY, hitBlock.z) ==
                static_cast<uint8_t>(voxel::BlockId::Air) &&
            setVoxelBlock(world, hitBlock.x, placeY, hitBlock.z, stone))
        {
            int32_t placedGroundY = 0;
            placedOnFace = sampleVoxelGroundHeight(hitBlock.x, hitBlock.z, placedGroundY,
                                                   nullptr, world) &&
                           placedGroundY == placeY + 1;
            // 收拾干净，后面的断言还按原来的高度算。
            setVoxelBlock(world, hitBlock.x, placeY, hitBlock.z,
                          static_cast<uint8_t>(voxel::BlockId::Air));
        }
    }

    // F3-c：地面查询（角色控制器用的那条路径）也必须看见体素方块。
    float worldHeight = 0.0f;
    entt::entity groundEntity = entt::null;
    const bool groundQuery = queryGroundAtWorld(static_cast<float>(hitBlock.x) + 0.5f,
                                                static_cast<float>(hitBlock.z) + 0.5f,
                                                worldHeight, groundEntity, nullptr);
    const bool groundIsVoxel = groundQuery &&
                               groundEntity != entt::null &&
                               registry.all_of<VoxelChunkComponent>(groundEntity) &&
                               std::abs(worldHeight - static_cast<float>(hitBlock.y + 1)) < 0.01f;

    // F3-c：让角色站在方块顶上。放一个角色在命中列上方 3 米，跑 60 帧它必须落到
    // 方块顶面、而不是穿过去或者停在空中。
    // 场景里可能还叠着地形：地形更高时 queryGroundAtWorld 会返回地形，
    // 这不是体素分支坏了。所以仅在“没有地形”时要求严格命中体素顶面。
    const bool terrainPresent = registry.view<TerrainComponent>().size() > 0;
    bool characterGrounded = false;
    bool characterLanded = false;
    {
        const entt::entity character = createCharacterEntity(
            "Voxel Test Character",
            glm::vec3(static_cast<float>(hitBlock.x) + 0.5f,
                      static_cast<float>(hitBlock.y) + 3.5f,
                      static_cast<float>(hitBlock.z) + 0.5f));
        for (int frame = 0; frame < 60; ++frame)
        {
            CharacterMoveResult moveResult;
            updateCharacterController(character, glm::vec3(0.0f), false, 1.0f / 60.0f,
                                      moveResult);
        }
        const auto* testTransform = registry.try_get<TransformComponent>(character);
        const auto* testController = registry.try_get<CharacterControllerComponent>(character);
        if (testTransform != nullptr && testController != nullptr)
        {
            const float restingY = testTransform->getTranslation().y;
            characterGrounded = testController->grounded;
            characterLanded = characterGrounded &&
                              std::abs(restingY - (static_cast<float>(hitBlock.y) + 1.0f)) < 0.05f;
        }
        destroyEntity(character);
    }

    // F3 持久化：差分捕获必须精确 —— 挖掉一格恰好产生一条 Air 记录，放回后清零。
    // 这里只验证 capture（序列化本体在编辑器的 JSON 往返用例里覆盖），
    // 避免在自检里 buildFromDescription 把宿主场景整个清掉。
    bool persistCapture = false;
    bool persistRestore = false;
    {
        const int32_t chunkX = voxel::Chunk::toChunkCoord(hitBlock.x);
        const int32_t chunkZ = voxel::Chunk::toChunkCoord(hitBlock.z);
        const int32_t localX = voxel::Chunk::toLocal(hitBlock.x);
        const int32_t localY = voxel::Chunk::toLocal(hitBlock.y);
        const int32_t localZ = voxel::Chunk::toLocal(hitBlock.z);

        auto scanEdits = [&](const scene::SceneDescription& description, bool& foundAir)
        {
            size_t total = 0;
            foundAir = false;
            for (const scene::VoxelWorldDescription& worldDesc : description.voxelWorlds)
            {
                if (worldDesc.seed != 4242ull)
                    continue;
                for (const scene::VoxelChunkDescription& chunkDesc : worldDesc.chunks)
                {
                    if (chunkDesc.chunkX != chunkX || chunkDesc.chunkZ != chunkZ)
                        continue;
                    for (const scene::VoxelBlockEdit& edit : chunkDesc.edits)
                    {
                        ++total;
                        if (edit.x == localX && edit.y == localY && edit.z == localZ &&
                            edit.block == static_cast<uint8_t>(voxel::BlockId::Air))
                            foundAir = true;
                    }
                }
            }
            return total;
        };

        if (setVoxelBlock(world, hitBlock.x, hitBlock.y, hitBlock.z,
                          static_cast<uint8_t>(voxel::BlockId::Air)))
        {
            bool foundAir = false;
            const size_t editsAfterDig = scanEdits(captureDescription(), foundAir);
            persistCapture = foundAir && editsAfterDig == 1;

            setVoxelBlock(world, hitBlock.x, hitBlock.y, hitBlock.z, original);
            bool foundRestored = false;
            const size_t editsAfterRestore = scanEdits(captureDescription(), foundRestored);
            persistRestore = !foundRestored && editsAfterRestore == 0;
        }
    }

    cleanup();

    const bool groundOk = groundIsVoxel || terrainPresent;
    const bool characterOk = characterLanded || (terrainPresent && characterGrounded);
    if (!dugDeeper || !restored || !groundMatches || !groundOk ||
        !characterOk || !placedOnFace || !persistCapture || !persistRestore ||
        !arenaRebuilt)
    {
        std::cerr << "[Voxel] selfTest: dig/place/ground mismatch (groundIsVoxel="
                  << groundIsVoxel << " characterLanded=" << characterLanded
                  << " placedOnFace=" << placedOnFace
                  << " arenaRebuilt=" << (arenaRebuilt ? 1 : 0) << ")" << std::endl;
        return false;
    }

    std::cout << "[Voxel] scene selfTest PASS | chunks=" << chunkCount
              << " hit=(" << hitBlock.x << "," << hitBlock.y << "," << hitBlock.z << ")"
              << " dugDeeper=" << (dugDeeper ? 1 : 0)
              << " restored=" << (restored ? 1 : 0)
              << " groundY=" << groundY
              << " characterLanded=" << (characterLanded ? 1 : 0)
              << " placedOnFace=" << (placedOnFace ? 1 : 0)
              << " arenaRebuilt=" << (arenaRebuilt ? 1 : 0)
              << " persistCapture=" << (persistCapture ? 1 : 0)
              << " persistRestore=" << (persistRestore ? 1 : 0) << std::endl;
    return true;
}

bool Scene::rebuildVoxelChunk(entt::entity chunkEntity)
{
    if (!device)
        return false;

    VoxelChunkComponent* chunkComp = registry.try_get<VoxelChunkComponent>(chunkEntity);
    // V-01：glass-only chunk 没有不透明 Model，所以 MeshComponent 是可选的。
    if (chunkComp == nullptr || !chunkComp->chunk)
        return false;

    terrain::TerrainMeshData mesh;
    voxel::MeshStats stats;
    if (!voxel::buildMesh(*chunkComp->chunk, mesh, &stats))
        return false;

    std::shared_ptr<terrain::TerrainMeshData> glassMesh = std::make_shared<terrain::TerrainMeshData>();
    voxel::GlassStats glassStats;
    if (!voxel::buildGlassMesh(*chunkComp->chunk, *glassMesh, &glassStats))
        return false;

    // 编辑后的重建也必须走 applyVoxelChunkMesh()：它负责归还旧 arena 切片、
    // 把临时 Model 拷进共享 arena 并释放 GPU 缓冲、复用已有材质槽。
    // 早期这里自己建 Model 并直接替换 MeshComponent，结果旧切片没归还、
    // arena 基址仍指向旧几何，编辑过的 chunk 会继续画编辑前的网格。
    std::shared_ptr<Material> material;
    if (const MaterialComponent* materialComp = registry.try_get<MaterialComponent>(chunkEntity))
        material = materialComp->material;
    if (!material)
        material = materialSystem.getOrCreateMaterial("voxel");

    if (!applyVoxelChunkMesh(chunkEntity, mesh, stats, glassMesh, glassStats, material))
        return false;

    refreshTransformsAndVisibility();
    return true;
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

entt::entity Scene::createTerrainEntityAsync(const terrain::TerrainConfig& config,
                                             const std::string& name,
                                             const glm::vec3& spawnPosition,
                                             TerrainGenerationCallback onComplete)
{
    if (!device || !jobSystem)
        return entt::null;

    std::string validationError;
    if (!config.validate(validationError))
    {
        std::cerr << "[Scene] createTerrainEntityAsync invalid config: " << validationError << std::endl;
        return entt::null;
    }

    std::string materialError;
    if (!ensureTerrainMaterialSet(config.materialSetIndex, materialError))
    {
        std::cerr << "[Scene] createTerrainEntityAsync material set failed: "
                  << materialError << std::endl;
        return entt::null;
    }

    const entt::entity entity = createTransformEntity(registry);
    markTransformDirty(entity);
    registry.get<TransformComponent>(entity).setTranslation(spawnPosition);
    registry.emplace<NameComponent>(entity, NameComponent{ name });

    RenderTagComponent tag;
    tag.isTerrain = true;
    registry.emplace<RenderTagComponent>(entity, tag);

    TerrainComponent terrainComponent;
    terrainComponent.config = config;
    terrainComponent.generatedConfig = config;
    terrainComponent.revision = 0;
    terrainComponent.generated = false;
    terrainComponent.generating = true;
    terrainComponent.generationBackend = "pending (async)";
    registry.emplace<TerrainComponent>(entity, std::move(terrainComponent));

    attachDirtyTracking(entity);

    if (!requestRegenerateTerrain(entity, std::move(onComplete)))
    {
        // 作业系统提交失败：实体留在场景里但永远是空的，直接撤掉更干净。
        destroyEntity(entity);
        return entt::null;
    }

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

    // F2-h: size the mesh arena for the whole ring instead of the default
    // radius-12 budget. Chunks are destroyed above, so every slice has been
    // returned already and the old arena can be dropped safely.
    const int32_t resolvedRadius = std::clamp<int32_t>(loadRadius, 1, kMaxTerrainStreamingRadius);
    const uint64_t ringChunks = static_cast<uint64_t>(2 * resolvedRadius + 1) *
                                static_cast<uint64_t>(2 * resolvedRadius + 1);
    const uint32_t wantVertices = static_cast<uint32_t>(std::min<uint64_t>(ringChunks * 1700ull, 8000000ull));
    const uint32_t wantIndices = static_cast<uint32_t>(std::min<uint64_t>(ringChunks * 9000ull, 42000000ull));
    if (terrainMeshArena && device)
    {
        const uint32_t haveVertices = terrainMeshArena->getVertexCapacity();
        const uint32_t haveIndices = terrainMeshArena->getIndexCapacity();
        if (wantVertices > haveVertices || wantIndices > haveIndices)
        {
            // Rendering code keeps a raw pointer to the arena, so grow it in place.
            terrainMeshArena->reconfigure(wantVertices, wantIndices);
            std::cout << "[Terrain] mesh arena resized | radius=" << resolvedRadius
                      << " chunks=" << ringChunks
                      << " vertexCapacity=" << wantVertices
                      << " indexCapacity=" << wantIndices << std::endl;
        }
    }

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

entt::entity Scene::resolveVoxelChunkOwner(entt::entity entity) const
{
    if (!registry.valid(entity))
        return entt::null;
    if (const auto* chunk = registry.try_get<VoxelChunkComponent>(entity))
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
    std::unordered_map<TerrainChunkCoord, entt::entity, TerrainChunkCoordHash> rekeyedChunks;
    rekeyedChunks.reserve(streamingTerrainChunks.size());
    for (const auto& [coord, entity] : streamingTerrainChunks)
    {
        if (!registry.valid(entity))
            continue;
        int64_t chunkX = coord.x - chunkDeltaX;
        int64_t chunkZ = coord.z - chunkDeltaZ;
        if (TerrainChunkComponent* chunk = registry.try_get<TerrainChunkComponent>(entity))
        {
            chunk->chunkX -= chunkDeltaX;
            chunk->chunkZ -= chunkDeltaZ;
            chunkX = chunk->chunkX;
            chunkZ = chunk->chunkZ;
        }
        rekeyedChunks[terrainChunkCoord(chunkX, chunkZ)] = entity;
    }
    streamingTerrainChunks.swap(rekeyedChunks);

    std::unordered_set<TerrainChunkCoord, TerrainChunkCoordHash> rekeyedPending;
    rekeyedPending.reserve(pendingStreamingTerrainChunks.size());
    for (const TerrainChunkCoord& coord : pendingStreamingTerrainChunks)
    {
        rekeyedPending.insert(
            terrainChunkCoord(coord.x - chunkDeltaX, coord.z - chunkDeltaZ));
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
    const int64_t centerX = static_cast<int64_t>(std::floor(focus.x / chunkSizeX));
    const int64_t centerZ = static_cast<int64_t>(std::floor(focus.z / chunkSizeZ));
    const int32_t radius = std::clamp(streaming->loadRadius, 1,
                                      kMaxTerrainStreamingRadius);
    const int32_t unloadRadius = radius + std::max(0, streaming->unloadPadding);

    // F2 收尾：稳态早退。
    //
    // 这趟扫描每帧要做 ~(2r+1)^2 次 map 查找、排序候选，再逐个检查已有 chunk；
    // 半径 24（2401 块）时实测约 70 us/帧，是 Scene::update() 里最大的一项。
    // 但相机还停在同一个 chunk 里、环也填满、没有待办任务时，这一帧不可能有
    // 任何 chunk 生命周期的变化，所以整趟可以跳过。
    const size_t ringChunks = static_cast<size_t>(2 * static_cast<int64_t>(radius) + 1) *
                              static_cast<size_t>(2 * static_cast<int64_t>(radius) + 1);
    if (centerX == lastStreamingCenterX && centerZ == lastStreamingCenterZ &&
        radius == lastStreamingRadius && pendingStreamingTerrainChunks.empty() &&
        streamingTerrainChunks.size() == ringChunks)
    {
        return;
    }
    lastStreamingCenterX = centerX;
    lastStreamingCenterZ = centerZ;
    lastStreamingRadius = radius;

    // Retire chunks beyond the hysteresis ring and notice completed/failed
    // asynchronous jobs. Destroying an entity also erases it from the map, so
    // defer those calls until this iteration has finished.
    std::vector<entt::entity> chunksToDestroy;
    for (auto it = streamingTerrainChunks.begin(); it != streamingTerrainChunks.end();)
    {
        const TerrainChunkCoord key = it->first;
        const entt::entity entity = it->second;
        if (!registry.valid(entity))
        {
            pendingStreamingTerrainChunks.erase(key);
            it = streamingTerrainChunks.erase(it);
            continue;
        }

        const int64_t x = key.x;
        const int64_t z = key.z;
        const int64_t dxAbs = x > centerX ? x - centerX : centerX - x;
        const int64_t dzAbs = z > centerZ ? z - centerZ : centerZ - z;
        const int64_t distance = std::max(dxAbs, dzAbs);
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
        int64_t x;
        int64_t z;
        TerrainChunkCoord key;
        float distance2;
    };
    std::vector<Candidate> candidates;
    const float focusChunkX = focus.x / chunkSizeX - 0.5f;
    const float focusChunkZ = focus.z / chunkSizeZ - 0.5f;
    for (int64_t dz = -static_cast<int64_t>(radius); dz <= static_cast<int64_t>(radius); ++dz)
    {
        for (int64_t dx = -static_cast<int64_t>(radius);
             dx <= static_cast<int64_t>(radius); ++dx)
        {
            const int64_t x = centerX + dx;
            const int64_t z = centerZ + dz;
            const TerrainChunkCoord key = terrainChunkCoord(x, z);
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
        const float centerWorldX = static_cast<float>(
            (static_cast<double>(candidate.x) + 0.5) * static_cast<double>(chunkSizeX));
        const float centerWorldZ = static_cast<float>(
            (static_cast<double>(candidate.z) + 0.5) * static_cast<double>(chunkSizeZ));
        // F2-c: sampleOrigin is the absolute noise anchor, not a local
        // coordinate. Keeping it absolute means chunks generated after a
        // rebase still match their neighbours generated before it.
        config.sampleOriginX = worldOrigin.x + static_cast<double>(centerWorldX);
        config.sampleOriginZ = worldOrigin.z + static_cast<double>(centerWorldZ);

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

bool Scene::attachCharacterModel(entt::entity entity,
                                 const std::string& glbPath,
                                 float uniformScale)
{
    if (!registry.valid(entity) || glbPath.empty() || !device)
        return false;

    tiny::GLTFLoadContext ctx;
    ctx.device = device;
    ctx.commandPool = commandPool;
    ctx.materialSystem = &materialSystem;
    ctx.descriptorManager = descriptorManager;

    const std::shared_ptr<Model> model = tiny::loadGLTFModel(ctx, glbPath);
    if (!model || !model->isSkinned())
    {
        std::cerr << "[Character] model attach failed | path=" << glbPath << "\n";
        return false;
    }

    std::shared_ptr<Model> previousModel;
    if (const auto* previousMesh = registry.try_get<MeshComponent>(entity))
        previousModel = previousMesh->model;
    registry.emplace_or_replace<MeshComponent>(entity, MeshComponent{ model });
    retireModel(previousModel);

    auto& transform = registry.get<TransformComponent>(entity);
    const float scale = uniformScale > 0.0f ? uniformScale : 1.0f;
    transform.setScale(glm::vec3(scale));

    CharacterAnimationComponent animation;
    animation.model = model;
    animation.currentClip = model->findAnimation(animation.idleClip);
    if (animation.currentClip >= 0)
    {
        animation.currentClipName = animation.idleClip;
        model->applyAnimation(animation.currentClip, 0.0f);
    }
    registry.emplace_or_replace<CharacterAnimationComponent>(entity,
                                                            std::move(animation));

    setModelBounds(registry, entity, model, &dirtySpatialEntities);
    attachDirtyTracking(entity);
    markTransformDirty(entity);
    refreshTransformsAndVisibility();

    std::cout << "[Character] model attached | " << glbPath
              << " | skinned=1 animations=" << model->getAnimations().size()
              << " scale=" << scale << "\n";
    return true;
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

    // F3：体素方块也是地面。方块网格与世界上轴对齐，所以直接把 XZ 落到整数格，
    // 取该列最高实心方块的上表面；比地形高就赢。
    {
        const int32_t blockX = static_cast<int32_t>(std::floor(worldX));
        const int32_t blockZ = static_cast<int32_t>(std::floor(worldZ));
        int32_t voxelTop = 0;
        entt::entity voxelChunk = entt::null;
        if (sampleVoxelGroundHeight(blockX, blockZ, voxelTop, &voxelChunk))
        {
            const float voxelHeight = static_cast<float>(voxelTop);
            if (!found || voxelHeight > outHeight)
            {
                found = true;
                outHeight = voxelHeight;
                outEntity = voxelChunk;
            }
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

namespace
{
void updateCharacterAnimation(entt::registry& registry, entt::entity entity,
                              const CharacterControllerComponent& controller,
                              float horizontalSpeed, bool justLanded, float dt)
{
    auto* animation = registry.try_get<CharacterAnimationComponent>(entity);
    if (!animation || !animation->model)
        return;

    if (justLanded)
        animation->landTimer = 0.45f;
    if (animation->landTimer > 0.0f)
        animation->landTimer = std::max(0.0f, animation->landTimer - dt);

    Model& model = *animation->model;
    std::string clipName;
    bool clipLoops = true;
    if (animation->landTimer > 0.0f && model.findAnimation(animation->jumpLandClip) >= 0)
    {
        clipName = animation->jumpLandClip;
        clipLoops = false;
    }
    else if (!controller.grounded)
    {
        if (controller.verticalVelocity > 0.2f)
        {
            clipName = animation->jumpClip;
            clipLoops = false;
        }
        else
        {
            clipName = animation->jumpIdleClip;
        }
    }
    else if (horizontalSpeed > 5.0f)
    {
        clipName = animation->runClip;
    }
    else if (horizontalSpeed > 0.2f)
    {
        clipName = animation->walkClip;
    }
    else
    {
        clipName = animation->idleClip;
    }

    const int clip = model.findAnimation(clipName);
    if (clip < 0)
        return;

    if (clip != animation->currentClip)
    {
        animation->currentClip = clip;
        animation->currentClipName = clipName;
        animation->time = 0.0f;
    }
    animation->loop = clipLoops;

    const float duration = model.getAnimationDuration(clip);
    animation->time += dt * animation->playbackSpeed;
    if (duration > 0.0f)
    {
        if (clipLoops)
            animation->time = std::fmod(animation->time, duration);
        else
            animation->time = std::min(animation->time, duration);
    }
    else
    {
        animation->time = 0.0f;
    }

    model.applyAnimation(clip, animation->time);
}
} // namespace

bool Scene::updateCharacterController(entt::entity entity,
                                      const glm::vec3& worldMoveDirection,
                                      bool jump,
                                      float dt,
                                      CharacterMoveResult& outResult,
                                      float speedMultiplier)
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
    const glm::vec3 startPosition = position;
    const bool wasGrounded = controller->grounded;
    const float previousVerticalVelocity = controller->verticalVelocity;
    speedMultiplier = std::max(speedMultiplier, 0.0f);

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

    const float targetX = position.x + move.x * controller->moveSpeed * speedMultiplier * dt;
    const float targetZ = position.z + move.z * controller->moveSpeed * speedMultiplier * dt;

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

    const float horizontalSpeed =
        glm::length(glm::vec2(position.x - startPosition.x,
                              position.z - startPosition.z)) / dt;
    if (glm::dot(move, move) > 1.0e-8f && horizontalSpeed > 0.05f)
    {
        // 角色只绕 Y 轴转向；前向为 +Z，和 glTF 角色默认朝向一致（截图会复核）。
        transform->setEuler(glm::vec3(0.0f, std::atan2(move.x, move.z), 0.0f));
    }

    const bool justLanded = !wasGrounded && controller->grounded &&
                            previousVerticalVelocity < -1.0f;
    updateCharacterAnimation(registry, entity, *controller, horizontalSpeed,
                             justLanded, dt);

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

void Scene::reportUpdateProfile(double genPollUs, double streamingUs, double worldUs,
                               double spatialUs, double visibilityUs)
{
    const double totalUs = genPollUs + streamingUs + worldUs + spatialUs + visibilityUs;
    profileGenPollUs += genPollUs;
    profileStreamingUs += streamingUs;
    profileWorldUs += worldUs;
    profileSpatialUs += spatialUs;
    profileVisibilityUs += visibilityUs;
    profileMaxTotalUs = std::max(profileMaxTotalUs, totalUs);
    ++profileFrames;

    if (profileFrames < 60)
        return;

    const double frames = static_cast<double>(profileFrames);
    std::cout << "[Update] avg us | genPoll=" << (profileGenPollUs / frames)
              << " streaming=" << (profileStreamingUs / frames)
              << " world=" << (profileWorldUs / frames)
              << " spatial=" << (profileSpatialUs / frames)
              << " visibility=" << (profileVisibilityUs / frames)
              << " | maxTotal=" << profileMaxTotalUs << std::endl;

    profileFrames = 0;
    profileGenPollUs = 0.0;
    profileStreamingUs = 0.0;
    profileWorldUs = 0.0;
    profileSpatialUs = 0.0;
    profileVisibilityUs = 0.0;
    profileMaxTotalUs = 0.0;
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

std::shared_ptr<Texture> Scene::loadMaterialTexture(const std::string& filepath, bool srgb)
{
    if (device == nullptr || commandPool == VK_NULL_HANDLE || filepath.empty())
        return nullptr;
    return materialSystem.loadTexture(filepath, *device, commandPool, srgb);
}
