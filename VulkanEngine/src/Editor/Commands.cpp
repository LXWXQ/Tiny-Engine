#include "Editor/Commands.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <optional>

namespace editor
{

// ---------------------------------------------------------------------------
// SetTransformCommand
// ---------------------------------------------------------------------------
SetTransformCommand::SetTransformCommand(entt::registry& registry, entt::entity entity,
                                         const glm::vec3& oldTranslation, const glm::vec3& newTranslation,
                                         const glm::vec3& oldRotation, const glm::vec3& newRotation,
                                         const glm::vec3& oldScale, const glm::vec3& newScale)
    : SetTransformCommand(registry, entity,
                          oldTranslation, newTranslation,
                          oldRotation, newRotation,
                          oldScale, newScale,
                          TransformComponent::quatFromEuler(oldRotation),
                          TransformComponent::quatFromEuler(newRotation))
{
}

SetTransformCommand::SetTransformCommand(entt::registry& registry, entt::entity entity,
                                         const glm::vec3& oldTranslation, const glm::vec3& newTranslation,
                                         const glm::vec3& oldRotation, const glm::vec3& newRotation,
                                         const glm::vec3& oldScale, const glm::vec3& newScale,
                                         const glm::quat& oldOrientation, const glm::quat& newOrientation)
    : registry(&registry), entity(entity),
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
        transform->translation = newTranslation;
        transform->rotation = newRotation;
        transform->orientation = newOrientation;
        transform->scale = newScale;
    }
}

void SetTransformCommand::revert()
{
    if (!registry->valid(entity))
        return;
    if (auto* transform = registry->try_get<TransformComponent>(entity))
    {
        transform->translation = oldTranslation;
        transform->rotation = oldRotation;
        transform->orientation = oldOrientation;
        transform->scale = oldScale;
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
DeleteEntityCommand::DeleteEntityCommand(entt::registry& registry, entt::entity entity)
    : registry(&registry), rootEntity(entity)
{
    if (!registry.valid(entity))
        return;

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
            bool hasCamera = false;
        };
        Snapshot snap;
        if (const auto* p = registry.try_get<TransformComponent>(current))  snap.transform = *p;
        if (const auto* p = registry.try_get<NameComponent>(current))       snap.name = *p;
        if (const auto* p = registry.try_get<MeshComponent>(current))       snap.mesh = *p;
        if (const auto* p = registry.try_get<MaterialComponent>(current))   snap.material = *p;
        if (const auto* p = registry.try_get<RenderTagComponent>(current))  snap.tag = *p;
        if (const auto* p = registry.try_get<BoundsComponent>(current))     snap.bounds = *p;
        // CameraComponent 是空 tag：查存在性要用 any_of，不能 try_get 取指针。
        snap.hasCamera = registry.any_of<CameraComponent>(current);

        Node node;
        node.original = current;
        node.parentIndex = parentIndex;
        node.rebuild = [&registry, snap](entt::entity created)
        {
            if (snap.transform) registry.emplace<TransformComponent>(created, *snap.transform);
            if (snap.name)      registry.emplace<NameComponent>(created, *snap.name);
            if (snap.mesh)      registry.emplace<MeshComponent>(created, *snap.mesh);
            if (snap.material)  registry.emplace<MaterialComponent>(created, *snap.material);
            if (snap.tag)       registry.emplace<RenderTagComponent>(created, *snap.tag);
            if (snap.bounds)    registry.emplace<BoundsComponent>(created, *snap.bounds);
            if (snap.hasCamera) registry.emplace<CameraComponent>(created);
            // get_or_emplace 标了 [[nodiscard]]，这里只关心"组件存在"，显式丢弃返回值。
            (void)registry.get_or_emplace<HierarchyComponent>(created);
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

    // 先把它从父节点的 children 里摘掉，否则父节点会留下悬空 ID
    if (registry->valid(rootEntity) && restoredParent != entt::null && registry->valid(restoredParent))
    {
        auto& siblings = registry->get<HierarchyComponent>(restoredParent).children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), rootEntity), siblings.end());
    }

    // 从叶子往根销毁。snapshot 是父先子后的顺序，所以反序遍历即可，
    // 中途不会访问到已经悬空的父子指针。
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        if (registry->valid(it->original))
            registry->destroy(it->original);
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
            transform->translation = spawnPosition;
        return;
    }

    entity = scene->createMeshEntity(meshPath, entityName);
    if (entity == entt::null)
        return;

    // 落到调用方指定的位置（通常是相机前方）。
    // 不这样做的话，新建的物体会堆在世界原点 —— 而相机可能离原点很远，
    // 用户会以为"没生成"。
    if (auto* transform = registry->try_get<TransformComponent>(entity))
        transform->translation = spawnPosition;

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
    registry->destroy(entity);
    entity = entt::null;
}

} // namespace editor
