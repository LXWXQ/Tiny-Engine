#include "Editor/Prefab.h"

#include "Editor/SceneSerializer.h"
#include "Scene/Scene.h"

#include <functional>
#include <unordered_set>

namespace editor
{
namespace prefab
{

bool validate(const scene::SceneDescription& description, std::string& errorMessage)
{
    errorMessage.clear();

    if (description.entities.empty())
    {
        errorMessage = "prefab has no entities";
        return false;
    }
    if (!description.voxelWorlds.empty())
    {
        errorMessage = "prefabs cannot contain voxel world descriptions";
        return false;
    }

    const size_t count = description.entities.size();
    int rootCount = 0;

    for (size_t i = 0; i < count; ++i)
    {
        const scene::EntityDescription& e = description.entities[i];

        if (e.isCamera)
        {
            errorMessage = "prefabs cannot contain a camera";
            return false;
        }
        if (e.isTerrain || e.isTerrainWorld)
        {
            errorMessage = "prefabs cannot contain terrain entities";
            return false;
        }
        if (e.isSkybox || e.isGrid)
        {
            errorMessage = "prefabs cannot contain skybox or grid helpers";
            return false;
        }

        if (e.parentIndex < -1 || e.parentIndex >= static_cast<int>(count))
        {
            errorMessage = "prefab has an out-of-range parent index";
            return false;
        }
        if (e.parentIndex == static_cast<int>(i))
        {
            errorMessage = "prefab entity cannot parent itself";
            return false;
        }
        if (e.parentIndex < 0)
            ++rootCount;
    }

    if (rootCount != 1)
    {
        errorMessage = "prefab must have exactly one root entity";
        return false;
    }

    // 手改 JSON 可能造出环（A→B→A）；每个实体最多沿父链走 count 步。
    for (size_t i = 0; i < count; ++i)
    {
        int cursor = description.entities[i].parentIndex;
        size_t steps = 0;
        while (cursor >= 0)
        {
            if (++steps > count)
            {
                errorMessage = "prefab hierarchy contains a cycle";
                return false;
            }
            cursor = description.entities[static_cast<size_t>(cursor)].parentIndex;
        }
    }

    return true;
}

bool save(Scene& scene, entt::entity root, const std::string& filepath,
          std::string& errorMessage)
{
    errorMessage.clear();

    if (root == entt::null)
    {
        errorMessage = "no entity selected";
        return false;
    }

    // VoxelWorldComponent 没有进 SceneDescription（体素世界只以 seed + 差分
    // 存在顶层 voxelWorlds 里），所以子树捕获前必须先拒绝选中体素世界。
    // 否则保存出来的预制体会变成一棵空节点，实例化时体素世界静默丢失。
    {
        std::unordered_set<entt::entity> visited;
        std::function<bool(entt::entity)> containsVoxelWorld = [&](entt::entity entity) -> bool
        {
            if (entity == entt::null || !scene.getRegistry().valid(entity) ||
                !visited.insert(entity).second)
                return false;
            if (scene.getRegistry().any_of<VoxelWorldComponent>(entity))
                return true;
            if (const auto* hierarchy = scene.getRegistry().try_get<HierarchyComponent>(entity))
            {
                for (entt::entity child : hierarchy->children)
                    if (containsVoxelWorld(child))
                        return true;
            }
            return false;
        };
        if (containsVoxelWorld(root))
        {
            errorMessage = "prefabs cannot contain a voxel world";
            return false;
        }
    }

    scene::SceneDescription description = scene.captureDescription(root);
    if (!validate(description, errorMessage))
        return false;

    return SceneSerializer::saveToFile(description, filepath, errorMessage);
}

bool load(const std::string& filepath, scene::SceneDescription& outDescription,
          std::string& errorMessage)
{
    errorMessage.clear();

    if (!SceneSerializer::loadFromFile(filepath, outDescription, errorMessage))
        return false;

    return validate(outDescription, errorMessage);
}

} // namespace prefab
} // namespace editor
