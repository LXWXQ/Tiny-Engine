#pragma once

#include <entt/entt.hpp>

namespace editor
{
// UX-01：冷启动时编辑器不加载 Sponza，视口只有天空、Hierarchy 是空的。
// 这个判据决定要不要显示「场景是空的」引导 —— 天空盒与相机不算内容。
// 地形 chunk 算内容（它们有 MeshComponent），所以只看非天空盒的 MeshComponent。
bool isSceneEmpty(entt::registry& registry);
}
