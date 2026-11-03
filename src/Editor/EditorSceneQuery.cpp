#include "Editor/EditorSceneQuery.h"

#include "Scene/Components.h"

namespace editor
{
    bool isSceneEmpty(entt::registry& registry)
    {
        for (auto [entity, mesh] : registry.view<MeshComponent>().each())
        {
            (void)entity;
            (void)mesh;
            const auto* tag = registry.try_get<RenderTagComponent>(entity);
            if (tag != nullptr && tag->isSkybox)
                continue;
            return false;
        }
        return true;
    }
}
