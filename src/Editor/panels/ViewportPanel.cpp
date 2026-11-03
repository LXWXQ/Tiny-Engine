#include "Editor/panels/ViewportPanel.h"
#include "Editor/EditorState.h"
#include "Editor/EditorSceneQuery.h"
#include "Editor/Commands.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace editor
{

namespace
{
// 射线与 AABB 求交（slab 法）。返回 true 时 outT 是入射距离。
bool rayAabb(const glm::vec3& origin, const glm::vec3& dir, const AABB& box, float& outT)
{
    if (!box.valid)
        return false;

    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis)
    {
        const float d = dir[axis];
        const float o = origin[axis];
        const float lo = box.min[axis];
        const float hi = box.max[axis];

        if (std::abs(d) < 1e-8f)
        {
            // 射线与该轴平行：起点必须在 slab 内，否则永不相交
            if (o < lo || o > hi)
                return false;
            continue;
        }

        const float inv = 1.0f / d;
        float t1 = (lo - o) * inv;
        float t2 = (hi - o) * inv;
        if (t1 > t2) std::swap(t1, t2);

        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax)
            return false;
    }

    outT = tMin;
    return true;
}

// 屏幕像素 -> 世界射线
void screenToRay(const ImVec2& screen, const ImVec2& rectMin, const ImVec2& rectMax,
                 const glm::mat4& view, const glm::mat4& projection,
                 glm::vec3& outOrigin, glm::vec3& outDir)
{
    const float width = std::max(rectMax.x - rectMin.x, 1.0f);
    const float height = std::max(rectMax.y - rectMin.y, 1.0f);

    const float ndcX = ((screen.x - rectMin.x) / width) * 2.0f - 1.0f;
    // 投影矩阵翻转了 Y，NDC +Y 指向屏幕下方
    const float ndcY = ((screen.y - rectMin.y) / height) * 2.0f - 1.0f;

    const glm::mat4 invVP = glm::inverse(projection * view);

    // 深度 0/1：工程全局启用 GLM_FORCE_DEPTH_ZERO_TO_ONE
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;

    outOrigin = glm::vec3(nearP);
    outDir = glm::normalize(glm::vec3(farP - nearP));
}

// 从当前场景里挑出射线命中的最近实体。
//
// 用 AABB 而不是三角形级精确拾取：场景里几百个实体、每个几万三角形，
// 逐三角形测试在编辑器里没有必要。AABB 对"点一下选中"这个用途足够了，
// 而且 WorldBoundsComponent 每帧已经在 Scene::rebuildSpatialIndex 里算好，
// 零额外开销。
// 从当前场景里挑出射线命中的最近实体。
//
// 【为什么必须精确到三角形，而不能只用 AABB】
// Sponza 整栋是一个实体，它的世界 AABB 把相机包在里面 —— 射线与 AABB 的
// 入射距离恒为 0，于是"点任何东西都选中 Sponza"，甚至点在它前面的球也一样。
// 这不是阈值没调好，是 AABB 在"相机位于物体内部"时完全失效。
//
// 所以：先用 AABB 做快速剔除（绝大多数实体一次比较就排除），
// 再用 Möller-Trumbore 做真正的三角形求交。
entt::entity pickEntity(entt::registry& registry, const glm::vec3& origin, const glm::vec3& dir)
{
    entt::entity best = entt::null;
    float bestDistance = std::numeric_limits<float>::max();

    for (auto [entity, bounds, mesh] : registry.view<WorldBoundsComponent, MeshComponent>().each())
    {
        if (!mesh.model)
            continue;

        // 天空盒是相机跟随的巨球，永远包住射线，不能参与拾取
        if (const auto* tag = registry.try_get<RenderTagComponent>(entity))
        {
            if (tag->isSkybox)
                continue;
        }

        // AABB 粗筛：射线连包围盒都没碰到就不可能命中三角形
        float aabbT = 0.0f;
        if (!rayAabb(origin, dir, bounds.world, aabbT))
            continue;

        // 已经比当前最近命中更远的实体，不必再测三角形
        if (aabbT > bestDistance)
            continue;

        const auto* worldTransform = registry.try_get<WorldTransformComponent>(entity);
        if (!worldTransform)
            continue;

        float distance = 0.0f;
        if (!mesh.model->intersectRay(worldTransform->matrix, origin, dir, distance))
            continue;

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = entity;
        }
    }

    return best;
}
} // namespace

void ViewportPanel::draw(EditorState& state)
{
    // 视口窗口不要标题栏、不要滚动条、内边距为 0：
    // 任何一点内边距都会让图片和面板边缘之间露出底色，看起来像"画面没铺满"。
    // NoMove / NoResize 是必须的：视口是"固定占满中央区域"的，不是可拖动的浮动窗口。
    // 少了这两个标志，在视口里按住左键拖拽（想转视角或拖手柄）会把整块视口拖走，
    // 而且位置还会被 imgui.ini 记住，下次启动依然是错的。
    // NoSavedSettings 保证它不会被写进 config/imgui.ini。
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (!ImGui::Begin("Viewport", nullptr, flags))
    {
        ImGui::PopStyleVar(2);
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar(2);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t width = (uint32_t)(avail.x > 1.0f ? avail.x : 1.0f);
    const uint32_t height = (uint32_t)(avail.y > 1.0f ? avail.y : 1.0f);

    // 尺寸变了就请求引擎重建离屏纹理。
    // 这里只"报数"，不自己建 —— 重建需要 vkDeviceWaitIdle，属于引擎职责，
    // 而且必须在帧首做（现在命令缓冲已在录制）。
    if (width != state.requestedViewportWidth || height != state.requestedViewportHeight)
    {
        state.requestedViewportWidth = width;
        state.requestedViewportHeight = height;
        if (state.services.requestViewportSize)
            state.services.requestViewportSize(width, height);
    }

    const ImTextureID textureId = state.services.getViewportTextureId
        ? state.services.getViewportTextureId(state.frameSlot)
        : (ImTextureID)0;

    if (textureId == (ImTextureID)0)
    {
        ImGui::TextDisabled("Viewport texture not ready.");
        ImGui::End();
        return;
    }


    ImGui::Image(textureId, avail, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    // 拾取与 gizmo 都用这个矩形做像素/NDC 换算。
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const bool imageHovered = ImGui::IsItemHovered();

    // UX-14：视口现在可以被拖动、缩放，所以布局矩形以真实窗口为准，
    // 不再由 drawPanels 的常量推算（那边只是首帧的初值）。
    state.viewportX = rectMin.x;
    state.viewportY = rectMin.y;
    state.viewportWidth = rectMax.x - rectMin.x;
    state.viewportHeight = rectMax.y - rectMin.y;

    // UX-01：编辑器冷启动不加载 Sponza，画面只有天空。没有这段文字时，
    // 新用户的第一反应是「程序没跑起来」，不会想到「场景是空的」。
    if (state.services.registry != nullptr && isSceneEmpty(*state.services.registry))
    {
        const float cardWidth = 420.0f;
        ImGui::SetCursorScreenPos(ImVec2(rectMin.x + (avail.x - cardWidth) * 0.5f,
                                     rectMin.y + avail.y * 0.32f));
        ImGui::BeginChild("##empty_scene", ImVec2(cardWidth, 0.0f), ImGuiChildFlags_AutoResizeY);
        ImGui::Text("场景是空的");
        ImGui::Separator();
        ImGui::TextWrapped("所以这里只有天空和网格 —— 渲染是正常的，只是还没有东西可看。");
        ImGui::Spacing();
        ImGui::TextDisabled("下一步：双击左侧 Assets 里的模型，或用下面的按钮先造一块地形。");
        ImGui::Spacing();
        // UX-08：这里不直接建地形 —— 交给 EditorLayer 的两阶段请求，
        // 先把"生成中"画出来，下一帧再真正 build（同样一条逻辑覆盖菜单与快捷键）。
        if (ImGui::Button("创建地形"))
            state.terrainCreatePending = true;
        if (state.terrainCreatePending)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("生成中…");
        }
        ImGui::SameLine();
        if (ImGui::Button("创建空对象"))
        {
            const entt::entity entity = state.services.scene->createEntity();
            state.services.registry->emplace_or_replace<NameComponent>(entity, NameComponent{ "Empty" });
            state.selection.select(entity);
            state.markDirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Shift+/ 看全部快捷键");
        ImGui::EndChild();
    }

    state.viewportHovered = imageHovered;

    // ---- 三轴手柄 ----
    GizmoView gizmoView;
    gizmoView.rectMin = rectMin;
    gizmoView.rectMax = rectMax;
    if (state.services.scene != nullptr)
    {
        gizmoView.view = state.services.scene->getCamera().getView();
        gizmoView.projection = state.services.scene->getCamera().getProjection();
        gizmoView.cameraPosition = state.services.scene->getCameraTransform().getTranslation();
    }
    // ---- 顺序很重要：交互 -> 画包围盒 -> 画手柄 ----
    //
    // 1) interact() 可能修改 transform，并在内部**立刻**刷新世界变换与包围盒；
    // 2) 所以包围盒必须在 interact() 之后读，否则拖拽时框会落后物体一帧
    //    （这正是"球和包围盒分离"的根因）；
    // 3) 手柄最后画，压在框上面才看得清。
    const bool gizmoConsumed = state.gizmo.interact(state, gizmoView);

    // 包围盒不再用 ImGui 画：ImGui 的 draw list 是纯 2D 叠加，没有深度概念，
    // 框会穿透墙体永远画在最前面。现在改由 EditorOverlayPass 作为真正的 3D 线段绘制，
    // 深度测试复用 G-Buffer 的深度图（见 docs/90-editor/gizmo.md）。

    state.gizmo.draw(gizmoView);

    // ---- B-07 地形雕刻 ----
    // Ctrl+左键把当前笔刷作用到射线命中的地形上。普通左键仍然用于选中，
    // 避免"只想选地形却改了高度"。
    bool terrainBrushConsumed = false;
    if (!gizmoConsumed && imageHovered && state.terrainBrushEnabled &&
        ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        state.services.scene != nullptr)
    {
        glm::vec3 rayOrigin, rayDir;
        screenToRay(ImGui::GetIO().MousePos, rectMin, rectMax,
                    gizmoView.view, gizmoView.projection, rayOrigin, rayDir);

        glm::vec3 hitPoint{ 0.0f };
        entt::entity hitTerrain = entt::null;
        float hitDistance = 0.0f;
        if (state.services.scene->raycastTerrainWorld(
                rayOrigin, rayDir, hitPoint, hitTerrain, hitDistance))
        {
            terrain::TerrainBrushSettings brush = state.terrainBrush;
            brush.center = hitPoint;

            Scene::TerrainSculptResult result;
            std::string errorMessage;
            if (state.services.scene->sculptTerrain(brush, result, errorMessage))
            {
                state.terrainBrush.center = hitPoint;
                char buffer[320];
                std::snprintf(buffer, sizeof(buffer),
                              "[Sculpt] %s | rebuilt=%u vertices=%u radius=%.2f "
                              "center=(%.2f, %.2f, %.2f)",
                              terrain::terrainBrushModeLabel(brush.mode),
                              result.rebuiltEntities, result.modifiedVertices,
                              brush.radius, hitPoint.x, hitPoint.y, hitPoint.z);
                state.pushLog(LogEntry::Level::Info, buffer);
                state.transformRefreshPending = true;
            }
            else
            {
                state.pushLog(LogEntry::Level::Error,
                              "[Sculpt] failed: " + errorMessage);
            }
        }
        else
        {
            state.pushLog(LogEntry::Level::Warning, "[Sculpt] ray missed terrain");
        }
        terrainBrushConsumed = true;
    }

    // ---- 点选 ----
    // 只有"手柄没吃掉这次点击"时才做拾取，否则拖拽手柄会顺手改掉选中。
    if (!gizmoConsumed && !terrainBrushConsumed && imageHovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        state.services.registry != nullptr && state.services.scene != nullptr)
    {
        glm::vec3 rayOrigin, rayDir;
        screenToRay(ImGui::GetIO().MousePos, rectMin, rectMax,
                    gizmoView.view, gizmoView.projection, rayOrigin, rayDir);

        const entt::entity picked = pickEntity(*state.services.registry, rayOrigin, rayDir);
        // Streamed chunks are runtime-derived; select their manager so delete/
        // undo/transform semantics stay stable across unload/reload.
        const entt::entity hit = picked == entt::null
            ? entt::null
            : state.services.scene->resolveTerrainChunkOwner(picked);

        // E1：Ctrl 点击把实体加入/移出选中集合；点空白仍然清空（Ctrl 时不清）。
        ImGuiIO& clickIo = ImGui::GetIO();
        if (hit == entt::null)
        {
            if (!clickIo.KeyCtrl)
                state.selection.clear();
        }
        else if (clickIo.KeyCtrl)
        {
            state.selection.toggle(hit);
        }
        else
        {
            state.selection.select(hit);
        }

        if (hit != entt::null)
        {
            const auto* name = state.services.registry->try_get<NameComponent>(hit);
            std::string message = "[Viewport] selected: " +
                (name ? name->name : std::string("entity"));
            if (picked != hit)
            {
                if (const auto* chunk =
                        state.services.registry->try_get<TerrainChunkComponent>(picked))
                    message += " (chunk " + std::to_string(chunk->chunkX) + "," +
                               std::to_string(chunk->chunkZ) + ")";
            }
            state.pushLog(LogEntry::Level::Info, message);
        }
    }

    // ---- 左上角信息 ----
    const char* modeName = (state.gizmo.mode == GizmoMode::Translate) ? "Move"
                         : (state.gizmo.mode == GizmoMode::Rotate) ? "Rotate" : "Scale";

    ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), "%ux%u  |  %s  |  W/E/R",
                       width, height, modeName);

    ImGui::End();
}

} // namespace editor
