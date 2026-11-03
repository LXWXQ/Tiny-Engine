#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace editor
{
namespace
{
TerrainComponent restorableTerrain(TerrainComponent terrain)
{
    // generating is a transient runtime flag; undo/redo snapshots must never
    // resurrect a pending-job marker that no longer has a job behind it.
    terrain.generating = false;
    return terrain;
}

TransformComponent restorableTransform(TransformComponent transform)
{
    // The snapshot may outlive the original entity ID. Never fire a callback
    // from the copy; Scene::attachDirtyTracking() rebinds after emplace.
    transform.clearDirtyOwner();
    transform.markDirty();
    return transform;
}

BoundsComponent restorableBounds(BoundsComponent bounds)
{
    bounds.clearDirtyOwner();
    bounds.markDirty();
    return bounds;
}
} // namespace


// ---------------------------------------------------------------------------
// SetTransformCommand
// ---------------------------------------------------------------------------
SetTransformCommand::SetTransformCommand(entt::registry& registry, entt::entity entity,
                                         const glm::vec3& oldTranslation, const glm::vec3& newTranslation,
                                         const glm::vec3& oldRotation, const glm::vec3& newRotation,
                                         const glm::vec3& oldScale, const glm::vec3& newScale,
                                         Scene* scene)
    : SetTransformCommand(registry, entity,
                          oldTranslation, newTranslation,
                          oldRotation, newRotation,
                          oldScale, newScale,
                          TransformComponent::quatFromEuler(oldRotation),
                          TransformComponent::quatFromEuler(newRotation),
                          scene)
{
}

SetTransformCommand::SetTransformCommand(entt::registry& registry, entt::entity entity,
                                         const glm::vec3& oldTranslation, const glm::vec3& newTranslation,
                                         const glm::vec3& oldRotation, const glm::vec3& newRotation,
                                         const glm::vec3& oldScale, const glm::vec3& newScale,
                                         const glm::quat& oldOrientation, const glm::quat& newOrientation,
                                         Scene* scene)
    : registry(&registry), scene(scene), entity(entity),
      oldTranslation(oldTranslation), newTranslation(newTranslation),
      oldRotation(oldRotation), newRotation(newRotation),
      oldScale(oldScale), newScale(newScale),
      oldOrientation(oldOrientation), newOrientation(newOrientation)
{
}

void SetTransformCommand::apply()
{
    if (!registry->valid(entity))
        return;
    if (auto* transform = registry->try_get<TransformComponent>(entity))
    {
        transform->setPose(newTranslation, newRotation, newScale, newOrientation);
        if (scene)
            scene->markTransformDirty(entity);
    }
}

void SetTransformCommand::revert()
{
    if (!registry->valid(entity))
        return;
    if (auto* transform = registry->try_get<TransformComponent>(entity))
    {
        transform->setPose(oldTranslation, oldRotation, oldScale, oldOrientation);
        if (scene)
            scene->markTransformDirty(entity);
    }
}

bool SetTransformCommand::mergeWith(const Command& previous) const
{
    // 只在"同一个实体的同一种命令"之间合并。
    // 用 dynamic_cast 而不是类型标签：命令类型很少，且这条路径不在渲染热路径上。
    const auto* other = dynamic_cast<const SetTransformCommand*>(&previous);
    return other && other->entity == entity;
}

void SetTransformCommand::absorb(const Command& newer)
{
    const auto* other = dynamic_cast<const SetTransformCommand*>(&newer);
    if (other == nullptr || other->entity != entity)
        return;
    newTranslation = other->newTranslation;
    newRotation = other->newRotation;
    newOrientation = other->newOrientation;
    newScale = other->newScale;
}

// ---------------------------------------------------------------------------
// SetTransformsCommand（E2：多选拖拽）
// ---------------------------------------------------------------------------
SetTransformsCommand::SetTransformsCommand(entt::registry& registryArg,
                                          std::vector<TransformPose> posesArg,
                                          Scene* sceneArg)
    : registry(&registryArg), scene(sceneArg), poses(std::move(posesArg))
{
}

void SetTransformsCommand::apply()
{
    for (const TransformPose& pose : poses)
    {
        if (!registry->valid(pose.entity))
            continue;
        if (auto* transform = registry->try_get<TransformComponent>(pose.entity))
        {
            transform->setPose(pose.newTranslation, pose.newRotation, pose.newScale,
                               pose.newOrientation);
            if (scene)
                scene->markTransformDirty(pose.entity);
        }
    }
}

void SetTransformsCommand::revert()
{
    for (const TransformPose& pose : poses)
    {
        if (!registry->valid(pose.entity))
            continue;
        if (auto* transform = registry->try_get<TransformComponent>(pose.entity))
        {
            transform->setPose(pose.oldTranslation, pose.oldRotation, pose.oldScale,
                               pose.oldOrientation);
            if (scene)
                scene->markTransformDirty(pose.entity);
        }
    }
}

bool SetTransformsCommand::mergeWith(const Command& previous) const
{
    const auto* other = dynamic_cast<const SetTransformsCommand*>(&previous);
    if (other == nullptr || other->poses.size() != poses.size())
        return false;
    for (size_t i = 0; i < poses.size(); ++i)
        if (other->poses[i].entity != poses[i].entity)
            return false;
    return true;
}

void SetTransformsCommand::absorb(const Command& newer)
{
    const auto* other = dynamic_cast<const SetTransformsCommand*>(&newer);
    if (other == nullptr || other->poses.size() != poses.size())
        return;
    for (size_t i = 0; i < poses.size(); ++i)
    {
        if (other->poses[i].entity != poses[i].entity)
            return;
        poses[i].newTranslation = other->poses[i].newTranslation;
        poses[i].newRotation = other->poses[i].newRotation;
        poses[i].newScale = other->poses[i].newScale;
        poses[i].newOrientation = other->poses[i].newOrientation;
    }
}

// ---------------------------------------------------------------------------
// RenameEntityCommand
// ---------------------------------------------------------------------------
RenameEntityCommand::RenameEntityCommand(entt::registry& registry, entt::entity entity,
                                         const std::string& oldName, const std::string& newName)
    : registry(&registry), entity(entity), oldName(oldName), newName(newName)
{
}

void RenameEntityCommand::apply()
{
    if (!registry->valid(entity))
        return;
    if (auto* name = registry->try_get<NameComponent>(entity))
        name->name = newName;
    else
        registry->emplace<NameComponent>(entity, NameComponent{ newName });
}

void RenameEntityCommand::revert()
{
    if (!registry->valid(entity))
        return;
    if (auto* name = registry->try_get<NameComponent>(entity))
        name->name = oldName;
}

bool RenameEntityCommand::mergeWith(const Command& previous) const
{
    const auto* other = dynamic_cast<const RenameEntityCommand*>(&previous);
    return other && other->entity == entity;
}

void RenameEntityCommand::absorb(const Command& newer)
{
    const auto* other = dynamic_cast<const RenameEntityCommand*>(&newer);
    if (other == nullptr || other->entity != entity)
        return;
    newName = other->newName;
}

// ---------------------------------------------------------------------------
// DeleteEntityCommand
// ---------------------------------------------------------------------------
DeleteEntityCommand::DeleteEntityCommand(entt::registry& registry, entt::entity entity,
                                         Scene* scene)
    : registry(&registry), scene(scene), rootEntity(entity)
{
    if (!registry.valid(entity))
        return;

    // Deleting a runtime chunk would desynchronise the streaming manager.
    // Resolve the selection to its manager before snapshotting.
    if (scene)
    {
        const entt::entity owner = scene->resolveTerrainChunkOwner(entity);
        if (owner != entt::null && owner != entity && registry.valid(owner))
        {
            entity = owner;
            rootEntity = owner;
        }
    }

    // 记录它原来的父节点，撤销时挂回去
    if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
        restoredParent = hierarchy->parent;

    // 深度优先收集子树，父一定排在子前面
    std::function<void(entt::entity, int)> collect = [&](entt::entity current, int parentIndex)
    {
        if (!registry.valid(current))
            return;

        const int myIndex = static_cast<int>(snapshot.size());

        // 【必须做值拷贝，不能用 try_get 返回的指针】
        //
        // registry.try_get<T>() 返回的是**指向组件存储的指针**。
        // 如果闭包捕获这个指针，apply() 一旦销毁实体，指针就悬空了，
        // revert() 再解引用就是未定义行为 —— 表现为"撤销删除后实体没回来"
        // 或者读到一串垃圾数值（本项目的自检就是这样抓到这个 bug 的）。
        //
        // 所以这里用 std::optional 做真正的值拷贝：拷贝发生在构造期，
        // 与实体的生命周期彻底解耦。
        struct Snapshot
        {
            std::optional<TransformComponent>   transform;
            std::optional<NameComponent>        name;
            std::optional<MeshComponent>        mesh;
            std::optional<MaterialComponent>    material;
            std::optional<RenderTagComponent>   tag;
            std::optional<BoundsComponent>      bounds;
            std::optional<TerrainComponent>    terrain;
            std::optional<TerrainStreamingComponent> streaming;
            bool hasCamera = false;
        };
        Snapshot snap;
        if (const auto* p = registry.try_get<TransformComponent>(current))  snap.transform = *p;
        if (const auto* p = registry.try_get<NameComponent>(current))       snap.name = *p;
        if (const auto* p = registry.try_get<MeshComponent>(current))       snap.mesh = *p;
        if (const auto* p = registry.try_get<MaterialComponent>(current))   snap.material = *p;
        if (const auto* p = registry.try_get<RenderTagComponent>(current))  snap.tag = *p;
        if (const auto* p = registry.try_get<BoundsComponent>(current))     snap.bounds = *p;
        if (const auto* p = registry.try_get<TerrainComponent>(current))    snap.terrain = *p;
        if (const auto* p = registry.try_get<TerrainStreamingComponent>(current)) snap.streaming = *p;
        // CameraComponent 是空 tag：查存在性要用 any_of，不能 try_get 取指针。
        snap.hasCamera = registry.any_of<CameraComponent>(current);

        Node node;
        node.original = current;
        node.parentIndex = parentIndex;
        node.rebuild = [&registry, snap, scene](entt::entity created)
        {
            if (snap.transform) registry.emplace<TransformComponent>(created, restorableTransform(*snap.transform));
            if (snap.name)      registry.emplace<NameComponent>(created, *snap.name);
            if (snap.mesh)      registry.emplace<MeshComponent>(created, *snap.mesh);
            if (snap.material)  registry.emplace<MaterialComponent>(created, *snap.material);
            if (snap.tag)       registry.emplace<RenderTagComponent>(created, *snap.tag);
            if (snap.bounds)    registry.emplace<BoundsComponent>(created, restorableBounds(*snap.bounds));
            if (snap.terrain)   registry.emplace<TerrainComponent>(created, restorableTerrain(*snap.terrain));
            if (snap.streaming) registry.emplace<TerrainStreamingComponent>(created, *snap.streaming);
            if (snap.hasCamera) registry.emplace<CameraComponent>(created);
            // get_or_emplace 标了 [[nodiscard]]，这里只关心"组件存在"，显式丢弃返回值。
            (void)registry.get_or_emplace<HierarchyComponent>(created);
            if (scene)
                scene->attachDirtyTracking(created);
        };
        snapshot.push_back(std::move(node));

        if (const auto* hierarchy = registry.try_get<HierarchyComponent>(current))
        {
            const std::vector<entt::entity> children = hierarchy->children;  // 拷贝，防迭代中修改
            for (entt::entity child : children)
                collect(child, myIndex);
        }
    };
    collect(entity, -1);
    captured = true;
}

void DeleteEntityCommand::apply()
{
    if (!captured)
        return;

    // After undo, destroy the rebuilt copies rather than the original IDs
    // (which are already invalid). This makes redo symmetric with the generic
    // delete command and mandatory for runtime streaming managers.
    const entt::entity rootToRemove =
        (restoredRootEntity != entt::null && registry->valid(restoredRootEntity))
            ? restoredRootEntity : rootEntity;

    // 先把它从父节点的 children 里摘掉，否则父节点会留下悬空 ID
    if (registry->valid(rootToRemove) && restoredParent != entt::null &&
        registry->valid(restoredParent))
    {
        auto& siblings = registry->get<HierarchyComponent>(restoredParent).children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), rootToRemove), siblings.end());
    }

    if (!createdEntities.empty())
    {
        for (auto it = createdEntities.rbegin(); it != createdEntities.rend(); ++it)
        {
            if (registry->valid(*it))
            {
                if (scene)
                    scene->destroyEntity(*it);
                else
                    registry->destroy(*it);
            }
        }
        createdEntities.clear();
        restoredRootEntity = entt::null;
        return;
    }

    // 从叶子往根销毁。snapshot 是父先子后的顺序，所以反序遍历即可，
    // 中途不会访问到已经悬空的父子指针。
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        if (registry->valid(it->original))
        {
            if (scene)
                scene->destroyEntity(it->original);
            else
                registry->destroy(it->original);
        }
    }

    createdEntities.clear();
    restoredRootEntity = entt::null;
}

void DeleteEntityCommand::revert()
{
    if (!captured)
        return;

    createdEntities.clear();
    restoredRootEntity = entt::null;

    // 父先子后重建
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        const entt::entity created = registry->create();
        snapshot[i].rebuild(created);
        if (scene)
        {
            scene->markTransformDirty(created);
            scene->markSpatialDirty(created);
        }
        createdEntities.push_back(created);
        if (snapshot[i].parentIndex < 0)
            restoredRootEntity = created;
    }

    // 按记录的下标重新挂父子关系
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        const int parentIndex = snapshot[i].parentIndex;
        if (parentIndex < 0)
            continue;
        if (parentIndex >= static_cast<int>(createdEntities.size()))
            continue;

        const entt::entity child = createdEntities[i];
        const entt::entity parent = createdEntities[parentIndex];
        registry->get<HierarchyComponent>(child).parent = parent;
        registry->get<HierarchyComponent>(parent).children.push_back(child);
    }

    // 子树根挂回原来的父节点
    if (restoredRootEntity != entt::null && restoredParent != entt::null && registry->valid(restoredParent))
    {
        registry->get<HierarchyComponent>(restoredRootEntity).parent = restoredParent;
        registry->get<HierarchyComponent>(restoredParent).children.push_back(restoredRootEntity);
    }

    // A restored streaming manager must be re-adopted by Scene so its runtime
    // chunk set can stream back in around the camera.
    if (scene)
    {
        for (entt::entity created : createdEntities)
        {
            if (registry->valid(created) &&
                registry->any_of<TerrainStreamingComponent>(created))
            {
                scene->adoptStreamingTerrainManager(created);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// CreateEntityCommand
// ---------------------------------------------------------------------------
CreateEntityCommand::CreateEntityCommand(entt::registry& registry, Scene& scene,
                                         std::string meshPath, std::string name,
                                         const glm::vec3& spawnPosition)
    : registry(&registry), scene(&scene),
      meshPath(std::move(meshPath)), entityName(std::move(name)),
      spawnPosition(spawnPosition)
{
}

void CreateEntityCommand::apply()
{
    // 第一次：真的加载模型并创建实体。
    // 之后（redo）：用快照重建组件，跳过加载。
    if (meshSnapshot.has_value())
    {
        entity = scene->createEntity();
        registry->emplace<MeshComponent>(entity, *meshSnapshot);
        if (materialSnapshot.has_value())
            registry->emplace<MaterialComponent>(entity, *materialSnapshot);
        registry->emplace<NameComponent>(entity, NameComponent{ entityName });
        registry->emplace<RenderTagComponent>(entity);
        if (auto* transform = registry->try_get<TransformComponent>(entity))
            transform->setTranslation(spawnPosition);
        scene->markTransformDirty(entity);
        scene->markSpatialDirty(entity);
        return;
    }

    entity = scene->createMeshEntity(meshPath, entityName);
    if (entity == entt::null)
        return;

    // 落到调用方指定的位置（通常是相机前方）。
    // 不这样做的话，新建的物体会堆在世界原点 —— 而相机可能离原点很远，
    // 用户会以为"没生成"。
    if (auto* transform = registry->try_get<TransformComponent>(entity))
        transform->setTranslation(spawnPosition);

    // 快照：shared_ptr 拷贝，Model 的 GPU 缓冲因此活到命令被丢弃为止。
    if (const auto* mesh = registry->try_get<MeshComponent>(entity))
        meshSnapshot = *mesh;
    if (const auto* material = registry->try_get<MaterialComponent>(entity))
        materialSnapshot = *material;
}

void CreateEntityCommand::revert()
{
    if (entity == entt::null || !registry->valid(entity))
        return;

    // 只销毁实体；Model 仍被 meshSnapshot 持有，所以 redo 不用重新加载。
    scene->destroyEntity(entity);
    entity = entt::null;
}


// ---------------------------------------------------------------------------
// Terrain commands
// ---------------------------------------------------------------------------
CreateTerrainCommand::CreateTerrainCommand(entt::registry& registry, Scene& scene,
                                           terrain::TerrainConfig config,
                                           std::string name,
                                           const glm::vec3& spawnPosition)
    : registry(&registry), scene(&scene), config(std::move(config)),
      entityName(std::move(name)), spawnPosition(spawnPosition)
{
}

void CreateTerrainCommand::apply()
{
    // Redo: restore from snapshots, never regenerate noise or upload again.
    if (meshSnapshot.has_value() && terrainSnapshot.has_value())
    {
        entity = scene->createEntity();
        if (auto* transform = registry->try_get<TransformComponent>(entity))
            transform->setTranslation(spawnPosition);

        registry->emplace_or_replace<NameComponent>(entity, NameComponent{ entityName });
        registry->emplace_or_replace<MeshComponent>(entity, *meshSnapshot);
        registry->emplace_or_replace<TerrainComponent>(entity, restorableTerrain(*terrainSnapshot));
        if (tagSnapshot.has_value())
            registry->emplace_or_replace<RenderTagComponent>(entity, *tagSnapshot);
        if (boundsSnapshot.has_value())
            registry->emplace_or_replace<BoundsComponent>(entity, restorableBounds(*boundsSnapshot));
        scene->markTransformDirty(entity);
        scene->markSpatialDirty(entity);
        scene->refreshTransformsAndVisibility();
        return;
    }

    entity = scene->createTerrainEntity(config, entityName, spawnPosition);
    if (entity == entt::null || !registry->valid(entity))
    {
        entity = entt::null;
        return;
    }

    if (const auto* mesh = registry->try_get<MeshComponent>(entity))
        meshSnapshot = *mesh;
    if (const auto* terrain = registry->try_get<TerrainComponent>(entity))
        terrainSnapshot = *terrain;
    if (const auto* tag = registry->try_get<RenderTagComponent>(entity))
        tagSnapshot = *tag;
    if (const auto* bounds = registry->try_get<BoundsComponent>(entity))
        boundsSnapshot = *bounds;
}

void CreateTerrainCommand::revert()
{
    if (entity == entt::null || !registry->valid(entity))
        return;

    // Keep snapshots alive; Scene::destroyEntity only removes the ECS entity.
    scene->destroyEntity(entity);
    entity = entt::null;
    scene->refreshTransformsAndVisibility();
}

CreateCharacterCommand::CreateCharacterCommand(entt::registry& registry, Scene& scene,
                                               std::string name,
                                               const glm::vec3& spawnPosition)
    : registry(&registry), scene(&scene), entityName(std::move(name)),
      spawnPosition(spawnPosition)
{
}

void CreateCharacterCommand::apply()
{
    entity = scene->createCharacterEntity(entityName, spawnPosition);
}

void CreateCharacterCommand::revert()
{
    if (entity == entt::null || !registry->valid(entity))
        return;
    scene->destroyEntity(entity);
    entity = entt::null;
}

// ---------------------------------------------------------------------------
SetTerrainConfigCommand::SetTerrainConfigCommand(entt::registry& registry,
                                                 entt::entity entity,
                                                 const terrain::TerrainConfig& oldConfig,
                                                 const terrain::TerrainConfig& newConfig)
    : registry(&registry), entity(entity), oldConfig(oldConfig), newConfig(newConfig)
{
}

void SetTerrainConfigCommand::apply()
{
    if (!registry->valid(entity))
        return;
    if (auto* terrain = registry->try_get<TerrainComponent>(entity))
        terrain->config = newConfig;
}

void SetTerrainConfigCommand::revert()
{
    if (!registry->valid(entity))
        return;
    if (auto* terrain = registry->try_get<TerrainComponent>(entity))
        terrain->config = oldConfig;
}

bool SetTerrainConfigCommand::mergeWith(const Command& previous) const
{
    const auto* other = dynamic_cast<const SetTerrainConfigCommand*>(&previous);
    return other && other->entity == entity;
}

void SetTerrainConfigCommand::absorb(const Command& newer)
{
    const auto* other = dynamic_cast<const SetTerrainConfigCommand*>(&newer);
    if (other == nullptr || other->entity != entity)
        return;
    newConfig = other->newConfig;
}

// ---------------------------------------------------------------------------
RegenerateTerrainCommand::RegenerateTerrainCommand(entt::registry& registry, Scene& scene,
                                                   entt::entity entity,
                                                   const TerrainComponent& oldTerrain,
                                                   const TerrainComponent& newTerrain,
                                                   const MeshComponent& oldMesh,
                                                   const MeshComponent& newMesh,
                                                   const BoundsComponent& oldBounds,
                                                   const BoundsComponent& newBounds,
                                                   const RenderTagComponent& oldTag,
                                                   const RenderTagComponent& newTag)
    : registry(&registry), scene(&scene), entity(entity),
      oldTerrain(oldTerrain), newTerrain(newTerrain),
      oldMesh(oldMesh), newMesh(newMesh),
      oldBounds(oldBounds), newBounds(newBounds),
      oldTag(oldTag), newTag(newTag)
{
}

void RegenerateTerrainCommand::restore(const TerrainComponent& terrain,
                                       const MeshComponent& mesh,
                                       const BoundsComponent& bounds,
                                       const RenderTagComponent& tag)
{
    if (!registry->valid(entity))
        return;

    registry->emplace_or_replace<TerrainComponent>(entity, restorableTerrain(terrain));
    registry->emplace_or_replace<MeshComponent>(entity, mesh);
    registry->emplace_or_replace<BoundsComponent>(entity, restorableBounds(bounds));
    registry->emplace_or_replace<RenderTagComponent>(entity, tag);
    scene->attachDirtyTracking(entity);
    scene->markSpatialDirty(entity);
    scene->refreshTransformsAndVisibility();
}

void RegenerateTerrainCommand::apply()
{
    restore(newTerrain, newMesh, newBounds, newTag);
}

void RegenerateTerrainCommand::revert()
{
    restore(oldTerrain, oldMesh, oldBounds, oldTag);
}

// ---------------------------------------------------------------------------
glm::vec3 terrainSpawnPosition(const Scene& scene,
                               const terrain::TerrainConfig& config)
{
    const TransformComponent& cameraTransform = scene.getCameraTransform();
    glm::vec3 forward(std::sin(cameraTransform.getEuler().y), 0.0f,
                      std::cos(cameraTransform.getEuler().y));
    if (glm::dot(forward, forward) < 1e-8f)
        forward = glm::vec3(0.0f, 0.0f, 1.0f);
    else
        forward = glm::normalize(forward);

    const float distance = config.sizeZ * 0.5f + 10.0f;
    return {
        cameraTransform.getTranslation().x + forward.x * distance,
        0.0f,
        cameraTransform.getTranslation().z + forward.z * distance
    };
}

// ---------------------------------------------------------------------------
// E1：CompositeCommand
// ---------------------------------------------------------------------------
CompositeCommand::CompositeCommand(std::string name,
                                   std::vector<std::unique_ptr<Command>> childrenArg)
    : commandName(std::move(name)), children(std::move(childrenArg))
{
}

void CompositeCommand::apply()
{
    for (std::unique_ptr<Command>& child : children)
        child->apply();
}

void CompositeCommand::revert()
{
    // 反序撤销：先建的后撤，保证父子/依赖关系成立。
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        (*it)->revert();
}

// ---------------------------------------------------------------------------
// E1：DuplicateEntityCommand
// ---------------------------------------------------------------------------
namespace
{
void copyDataComponents(entt::registry& from, entt::entity src,
                        entt::registry& registry, entt::entity dst)
{
    if (const auto* name = from.try_get<NameComponent>(src))
        registry.emplace<NameComponent>(dst, NameComponent{ name->name + " Copy" });
    if (const auto* transform = from.try_get<TransformComponent>(src))
        registry.emplace<TransformComponent>(dst, *transform);
    if (const auto* mesh = from.try_get<MeshComponent>(src))
        registry.emplace<MeshComponent>(dst, *mesh);
    if (const auto* material = from.try_get<MaterialComponent>(src))
        registry.emplace<MaterialComponent>(dst, *material);
    if (const auto* tag = from.try_get<RenderTagComponent>(src))
        registry.emplace<RenderTagComponent>(dst, *tag);
    if (const auto* bounds = from.try_get<BoundsComponent>(src))
        registry.emplace<BoundsComponent>(dst, *bounds);
    if (const auto* controller = from.try_get<CharacterControllerComponent>(src))
        registry.emplace<CharacterControllerComponent>(dst, *controller);
}
} // namespace

bool DuplicateEntityCommand::canDuplicate(entt::registry& registry, entt::entity entity)
{
    if (entity == entt::null || !registry.valid(entity))
        return false;
    if (registry.any_of<TerrainComponent>(entity) ||
        registry.any_of<TerrainChunkComponent>(entity) ||
        registry.any_of<TerrainStreamingComponent>(entity) ||
        registry.any_of<CameraComponent>(entity))
        return false;
    return true;
}

DuplicateEntityCommand::DuplicateEntityCommand(entt::registry& registryArg, Scene* sceneArg,
                                               entt::entity entity)
    : registry(&registryArg), scene(sceneArg), source(entity)
{
}

void DuplicateEntityCommand::apply()
{
    if (!registry->valid(source) || !canDuplicate(*registry, source))
        return;

    createdEntities.clear();
    createdRoot = entt::null;

    struct Pending
    {
        entt::entity src = entt::null;
        entt::entity dst = entt::null;
    };
    std::vector<Pending> pending;
    pending.push_back(Pending{ source, entt::null });

    for (size_t i = 0; i < pending.size(); ++i)
    {
        const entt::entity src = pending[i].src;
        if (!registry->valid(src))
            continue;

        const entt::entity dst = (scene != nullptr) ? scene->createEntity() : registry->create();
        copyDataComponents(*registry, src, *registry, dst);

        if (pending[i].dst != entt::null)
        {
            if (scene != nullptr)
                scene->setParent(dst, pending[i].dst);
            else
                registry->get_or_emplace<HierarchyComponent>(dst).parent = pending[i].dst;
            registry->get_or_emplace<HierarchyComponent>(pending[i].dst).children.push_back(dst);
        }
        else
        {
            createdRoot = dst;
        }
        createdEntities.push_back(dst);

        if (const auto* hierarchy = registry->try_get<HierarchyComponent>(src))
        {
            for (entt::entity child : hierarchy->children)
                pending.push_back(Pending{ child, dst });
        }
    }

    if (createdRoot != entt::null)
    {
        if (scene != nullptr)
            scene->refreshTransformsAndVisibility();
        else
            (void)0;
    }
}

void DuplicateEntityCommand::revert()
{
    for (auto it = createdEntities.rbegin(); it != createdEntities.rend(); ++it)
    {
        const entt::entity entity = *it;
        if (!registry->valid(entity))
            continue;
        if (scene != nullptr)
            scene->destroyEntity(entity);
        else
            registry->destroy(entity);
    }
    createdEntities.clear();
    createdRoot = entt::null;
}

entt::entity createTerrainThroughCommand(entt::registry& registry,
                                         Scene& scene,
                                         CommandStack& commands,
                                         const terrain::TerrainConfig& config,
                                         const std::string& name)
{
    auto command = std::make_unique<CreateTerrainCommand>(
        registry, scene, config, name, terrainSpawnPosition(scene, config));
    CreateTerrainCommand* raw = command.get();
    commands.execute(std::move(command));
    return raw->currentEntity();
}

} // namespace editor
