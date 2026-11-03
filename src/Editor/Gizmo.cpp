#include "Editor/Gizmo.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <algorithm>
#include <cmath>

namespace editor
{

namespace
{
// 手柄在屏幕上的目标长度（像素）。太小点不中，太大挡住物体。
constexpr float kHandlePixels = 90.0f;
// 命中阈值：鼠标离轴线的屏幕距离小于它就认为选中该轴。
constexpr float kPickPixels = 9.0f;
// 手柄线宽
constexpr float kLineThickness = 2.5f;

const ImU32 kAxisColor[3] = {
    IM_COL32(230, 70, 70, 255),    // X 红
    IM_COL32(90, 210, 90, 255),    // Y 绿
    IM_COL32(80, 140, 245, 255),   // Z 蓝
};
const ImU32 kAxisColorHot[3] = {
    IM_COL32(255, 190, 60, 255),
    IM_COL32(255, 190, 60, 255),
    IM_COL32(255, 190, 60, 255),
};

// 把 Y 轴以外的两个轴做正交化，得到旋转平面内的一组基
void buildPlaneBasis(const glm::vec3& normal, glm::vec3& outU, glm::vec3& outV)
{
    const glm::vec3 reference = (std::abs(normal.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    outU = glm::normalize(glm::cross(reference, normal));
    outV = glm::normalize(glm::cross(normal, outU));
}
} // namespace

bool Gizmo::projectToScreen(const glm::vec3& world, const GizmoView& view, ImVec2& outScreen)
{
    const glm::vec4 clip = view.projection * view.view * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-6f)
        return false;      // 在相机背后

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    const float width = view.rectMax.x - view.rectMin.x;
    const float height = view.rectMax.y - view.rectMin.y;

    outScreen.x = view.rectMin.x + (ndc.x * 0.5f + 0.5f) * width;

    // 【关键】本工程的投影矩阵把 Y 翻转了（VulkanCamera 里 projectionMatrix[1][1] *= -1），
    // 所以 NDC 的 +Y 指向**屏幕下方**，而不是通常的屏幕上方。
    // 这里必须直接用 (ndc.y*0.5+0.5)，不能再做一次 1-... 的翻转。
    //
    // 写错这一处的后果不是"报错"，而是包围盒/手柄相对真实渲染**垂直镜像**：
    // 物体往上移，白框却往下移 —— 看起来就是"两者方向相反"。
    // 而且如果 screenToRay 用了同样的错误约定，投影/反投影往返测试仍然会通过，
    // 所以只能靠"上方点必须投影到更小的屏幕 y"这条不变量才能测出来。
    outScreen.y = view.rectMin.y + (ndc.y * 0.5f + 0.5f) * height;
    return true;
}

void Gizmo::screenToRay(const ImVec2& screen, const GizmoView& view,
                        glm::vec3& outOrigin, glm::vec3& outDirection)
{
    const float width = std::max(view.rectMax.x - view.rectMin.x, 1.0f);
    const float height = std::max(view.rectMax.y - view.rectMin.y, 1.0f);

    const float ndcX = ((screen.x - view.rectMin.x) / width) * 2.0f - 1.0f;
    // 同 projectToScreen：NDC +Y 指向屏幕下方（投影矩阵翻转了 Y）
    const float ndcY = ((screen.y - view.rectMin.y) / height) * 2.0f - 1.0f;

    const glm::mat4 invVP = glm::inverse(view.projection * view.view);

    // 深度 0 / 1：工程全局启用 GLM_FORCE_DEPTH_ZERO_TO_ONE
    glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPoint  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    outOrigin = glm::vec3(nearPoint);
    outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
}

float Gizmo::closestPointOnAxis(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                const glm::vec3& axisOrigin, const glm::vec3& axisDir)
{
    // 求两条直线（射线与轴）最近点：把问题化成 2x2 线性方程组。
    const glm::vec3 w0 = axisOrigin - rayOrigin;
    const float a = glm::dot(axisDir, axisDir);   // 恒为 1（已归一化）
    const float b = glm::dot(axisDir, rayDir);
    const float c = glm::dot(rayDir, rayDir);     // 恒为 1
    const float d = glm::dot(axisDir, w0);
    const float e = glm::dot(rayDir, w0);

    const float denom = a * c - b * b;
    if (std::abs(denom) < 1e-6f)
        return 0.0f;      // 轴几乎与视线平行，退化

    // 沿轴方向的参数
    return (b * e - c * d) / denom;
}

bool Gizmo::rayPlane(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                     const glm::vec3& planePoint, const glm::vec3& planeNormal,
                     glm::vec3& outPoint)
{
    const float denom = glm::dot(rayDir, planeNormal);
    if (std::abs(denom) < 1e-6f)
        return false;

    const float t = glm::dot(planePoint - rayOrigin, planeNormal) / denom;
    if (t < 0.0f)
        return false;

    outPoint = rayOrigin + rayDir * t;
    return true;
}

float Gizmo::distanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const ImVec2 ab{ b.x - a.x, b.y - a.y };
    const ImVec2 ap{ p.x - a.x, p.y - a.y };
    const float abLenSq = ab.x * ab.x + ab.y * ab.y;
    if (abLenSq < 1e-6f)
        return std::sqrt(ap.x * ap.x + ap.y * ap.y);

    float t = (ap.x * ab.x + ap.y * ab.y) / abLenSq;
    t = std::clamp(t, 0.0f, 1.0f);

    const ImVec2 closest{ a.x + ab.x * t, a.y + ab.y * t };
    const float dx = p.x - closest.x;
    const float dy = p.y - closest.y;
    return std::sqrt(dx * dx + dy * dy);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void Gizmo::drawTranslate(ImDrawList* dl, const GizmoView& view, const glm::vec3& origin,
                          const glm::vec3 axes[3], float length, Axis hovered) const
{
    ImVec2 center;
    if (!projectToScreen(origin, view, center))
        return;

    for (int i = 0; i < 3; ++i)
    {
        ImVec2 tip;
        if (!projectToScreen(origin + axes[i] * length, view, tip))
            continue;

        const bool hot = (hovered == (Axis)(i + 1)) || (dragging && activeAxis == (Axis)(i + 1));
        const ImU32 color = hot ? kAxisColorHot[i] : kAxisColor[i];

        dl->AddLine(center, tip, color, kLineThickness);
        // 箭头：一个实心圆足够指示方向，且不受透视影响
        dl->AddCircleFilled(tip, kLineThickness * 2.6f, color);
    }
}

void Gizmo::drawScale(ImDrawList* dl, const GizmoView& view, const glm::vec3& origin,
                      const glm::vec3 axes[3], float length, Axis hovered) const
{
    ImVec2 center;
    if (!projectToScreen(origin, view, center))
        return;

    for (int i = 0; i < 3; ++i)
    {
        ImVec2 tip;
        if (!projectToScreen(origin + axes[i] * length, view, tip))
            continue;

        const bool hot = (hovered == (Axis)(i + 1)) || (dragging && activeAxis == (Axis)(i + 1));
        const ImU32 color = hot ? kAxisColorHot[i] : kAxisColor[i];

        dl->AddLine(center, tip, color, kLineThickness);
        // 方块端点，与平移的圆点区分开
        dl->AddRectFilled(ImVec2(tip.x - 4.0f, tip.y - 4.0f), ImVec2(tip.x + 4.0f, tip.y + 4.0f), color);
    }
}

void Gizmo::drawRotate(ImDrawList* dl, const GizmoView& view, const glm::vec3& origin,
                       const glm::vec3 axes[3], float radius, Axis hovered) const
{
    constexpr int kSegments = 64;

    for (int i = 0; i < 3; ++i)
    {
        const bool hot = (hovered == (Axis)(i + 1)) || (dragging && activeAxis == (Axis)(i + 1));
        const ImU32 color = hot ? kAxisColorHot[i] : kAxisColor[i];

        glm::vec3 u, v;
        buildPlaneBasis(axes[i], u, v);

        ImVec2 previous;
        bool hasPrevious = false;

        for (int s = 0; s <= kSegments; ++s)
        {
            const float angle = (float)s / kSegments * 6.28318530718f;
            const glm::vec3 point = origin + (u * std::cos(angle) + v * std::sin(angle)) * radius;

            ImVec2 screen;
            if (!projectToScreen(point, view, screen))
            {
                hasPrevious = false;
                continue;
            }
            if (hasPrevious)
                dl->AddLine(previous, screen, color, kLineThickness);
            previous = screen;
            hasPrevious = true;
        }
    }
}

// ---------------------------------------------------------------------------
// 数学自检
// ---------------------------------------------------------------------------
float Gizmo::snapValue(float value, float step)
{
    if (step <= 0.0f)
        return value;
    return std::round(value / step) * step;
}

bool Gizmo::selfTest()
{
    // 构造一个可预测的视图：相机在 (0,0,5) 朝 -Z 看，60 度 FOV，16:9。
    GizmoView view;
    view.rectMin = ImVec2(0.0f, 0.0f);
    view.rectMax = ImVec2(1600.0f, 900.0f);
    view.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f),
                            glm::vec3(0.0f, 0.0f, 0.0f),
                            glm::vec3(0.0f, 1.0f, 0.0f));
    view.projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    // 【必须和 VulkanCamera::setPerspectiveProjection 一致：Y 翻转】
    // 用未翻转的 glm::perspective 做自检，会掩盖"屏幕 Y 方向写反"的 bug ——
    // 因为投影和反投影一起错时，往返测试照样通过。
    view.projection[1][1] *= -1.0f;
    view.cameraPosition = glm::vec3(0.0f, 0.0f, 5.0f);

    // ---- 0) 屏幕方向不变量（这条才是真正能抓 Y 翻转错误的）----
    // 世界 +Y 更高的点，必须投影到**更小**的屏幕 y（更靠上）。
    {
        ImVec2 lower, higher;
        if (!projectToScreen(glm::vec3(0.0f, -1.0f, 0.0f), view, lower) ||
            !projectToScreen(glm::vec3(0.0f, 1.0f, 0.0f), view, higher))
        {
            std::cerr << "[Gizmo] selfTest FAIL: projection failed in up/down check\n";
            return false;
        }
        if (!(higher.y < lower.y))
        {
            std::cerr << "[Gizmo] selfTest FAIL: screen Y is flipped (world +Y projected to "
                      << higher.y << ", world -Y to " << lower.y
                      << "); expected +Y to be smaller (higher on screen)\n";
            return false;
        }
    }

    // 同理，世界 +X 更高的点必须投影到更大的屏幕 x
    {
        ImVec2 left, right;
        if (!projectToScreen(glm::vec3(-1.0f, 0.0f, 0.0f), view, left) ||
            !projectToScreen(glm::vec3(1.0f, 0.0f, 0.0f), view, right))
        {
            std::cerr << "[Gizmo] selfTest FAIL: projection failed in left/right check\n";
            return false;
        }
        if (!(right.x > left.x))
        {
            std::cerr << "[Gizmo] selfTest FAIL: screen X is flipped\n";
            return false;
        }
    }

    // ---- 1) 投影 -> 反投影往返 ----
    // 屏幕上的一点必须反投影出一条"经过原世界点"的射线。
    {
        const glm::vec3 points[] = {
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 2.0f, 0.0f },
            { -1.5f, -1.0f, 1.0f },
        };

        for (const glm::vec3& world : points)
        {
            ImVec2 screen;
            if (!projectToScreen(world, view, screen))
            {
                std::cerr << "[Gizmo] selfTest FAIL: project returned false\n";
                return false;
            }

            glm::vec3 rayOrigin, rayDir;
            screenToRay(screen, view, rayOrigin, rayDir);

            // 世界点到射线的距离
            const glm::vec3 toPoint = world - rayOrigin;
            const float along = glm::dot(toPoint, rayDir);
            const glm::vec3 closest = rayOrigin + rayDir * along;
            const float distance = glm::length(world - closest);

            if (distance > 1e-3f)
            {
                std::cerr << "[Gizmo] selfTest FAIL: project/unproject round trip drift="
                          << distance << "\n";
                return false;
            }
        }
    }

    // ---- 2) 射线与轴的最近点 ----
    // 轴沿 X 过原点；射线从 (2,5,0) 垂直向下，最近点应在 t=2。
    {
        const float t = closestPointOnAxis(glm::vec3(2.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
                                           glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        if (std::abs(t - 2.0f) > 1e-4f)
        {
            std::cerr << "[Gizmo] selfTest FAIL: closestPointOnAxis t=" << t << " expected 2\n";
            return false;
        }
    }

    // ---- 3) 射线与平面求交 ----
    {
        glm::vec3 hit;
        if (!rayPlane(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                      glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), hit))
        {
            std::cerr << "[Gizmo] selfTest FAIL: rayPlane returned false\n";
            return false;
        }
        if (glm::length(hit) > 1e-4f)
        {
            std::cerr << "[Gizmo] selfTest FAIL: rayPlane hit off origin\n";
            return false;
        }
    }

    // 旋转的符号约定：buildPlaneBasis 必须给右手系，这样正的极角增量
    // 就是绕 normal 的正向右手旋转，和 angleAxis(delta, localAxis) 一致。
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::vec3 normal(0.0f);
        normal[axis] = 1.0f;
        glm::vec3 u(0.0f);
        glm::vec3 v(0.0f);
        buildPlaneBasis(normal, u, v);
        if (glm::dot(glm::cross(u, v), normal) < 0.9999f)
        {
            std::cerr << "[Gizmo] selfTest FAIL: rotation plane basis is not right-handed\n";
            return false;
        }
    }
    // E5：吸附步长。step <= 0 必须是恒等（等于关闭），否则按最近刻度取整。
    {
        if (snapValue(0.4f, 0.0f) != 0.4f)
        {
            std::cerr << "[Gizmo] selfTest FAIL: snapValue with zero step must be identity" << std::endl;
            return false;
        }
        if (std::abs(snapValue(0.4f, 0.5f) - 0.5f) > 1e-5f ||
            std::abs(snapValue(1.2f, 0.5f) - 1.0f) > 1e-5f ||
            std::abs(snapValue(-0.7f, 0.5f) + 0.5f) > 1e-5f)
        {
            std::cerr << "[Gizmo] selfTest FAIL: snapValue rounding is wrong" << std::endl;
            return false;
        }
        std::cout << "[Gizmo] snap selfTest PASS | steps 0.5 / 15 deg / 0.1" << std::endl;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------
bool Gizmo::interact(EditorState& state, const GizmoView& view)
{
    if (!state.services.registry || !state.services.scene)
        return false;

    entt::registry& registry = *state.services.registry;
    const entt::entity entity = state.selection.primary();
    if (entity == entt::null || !registry.valid(entity))
    {
        dragging = false;
        activeAxis = Axis::None;
        drawValid = false;
        return false;
    }

    const auto* transform = registry.try_get<TransformComponent>(entity);
    const auto* world = registry.try_get<WorldTransformComponent>(entity);
    if (!transform || !world)
    {
        drawValid = false;
        return false;
    }

    // 手柄中心：优先用**局部包围盒中心经过世界矩阵变换**（OBB 中心），
    // 没有局部包围盒时退回世界 AABB 中心，再退回物体原点。
    //
    // 不能用世界 AABB 中心：物体旋转后 AABB 会被"包大"，中心也会漂移，
    // 手柄看起来就不在物体上了。局部中心 × 世界矩阵会跟着旋转/缩放/平移。
    glm::vec3 origin = glm::vec3(world->matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    bool usedLocalCenter = false;
    if (const auto* localBounds = registry.try_get<BoundsComponent>(entity))
    {
        if (localBounds->getLocal().valid)
        {
            origin = glm::vec3(world->matrix * glm::vec4(localBounds->getLocal().center(), 1.0f));
            usedLocalCenter = true;
        }
    }
    if (!usedLocalCenter)
    {
        if (const auto* worldBounds = registry.try_get<WorldBoundsComponent>(entity))
        {
            if (worldBounds->world.valid)
                origin = worldBounds->world.center();
        }
    }

    // 局部轴（列向量；缩放会污染长度，所以归一化）
    glm::vec3 localAxes[3] = {
        glm::normalize(glm::vec3(world->matrix[0])),
        glm::normalize(glm::vec3(world->matrix[1])),
        glm::normalize(glm::vec3(world->matrix[2])),
    };
    // 平移用世界轴：直觉上"往上拖就是世界往上"
    glm::vec3 worldAxes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };

    const glm::vec3* axes = (mode == GizmoMode::Translate) ? worldAxes : localAxes;

    // 手柄长度：让它在屏幕上恒定 ~90px。
    // 用相机距离 + 投影矩阵反推，而不是投影两次 —— 后者在轴几乎正对相机时
    // 会算出接近 0 的像素长度，导致手柄长度爆炸。
    const float distance = glm::length(origin - view.cameraPosition);
    // 注意：Vulkan 的投影矩阵把 Y 轴翻转（NDC Y 向下），所以 projection[1][1]
    // 是**负数**。直接取倒数会得到负的 tanHalfFov，手柄长度算出来是负值，
    // 被 std::max 钳成 1e-4 —— 表现就是"三个轴箭头全叠在中心一点"。
    // 这里必须取绝对值。
    const float tanHalfFov = (std::abs(view.projection[1][1]) > 1e-6f)
        ? std::abs(1.0f / view.projection[1][1])
        : 0.577f;
    const float viewportHeight = std::max(view.rectMax.y - view.rectMin.y, 1.0f);
    const float worldPerPixel = 2.0f * tanHalfFov * std::max(distance, 0.01f) / viewportHeight;
    const float handleLength = std::max(kHandlePixels * worldPerPixel, 1e-4f);


    // ---- 命中测试 ----
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool mouseInside = mouse.x >= view.rectMin.x && mouse.x <= view.rectMax.x &&
                             mouse.y >= view.rectMin.y && mouse.y <= view.rectMax.y;

    Axis hovered = Axis::None;
    if (!dragging && mouseInside)
    {
        float best = kPickPixels;
        ImVec2 center;
        if (projectToScreen(origin, view, center))
        {
            for (int i = 0; i < 3; ++i)
            {
                float d = 1e9f;
                if (mode == GizmoMode::Rotate)
                {
                    // 旋转手柄是圆环：用"鼠标到圆环采样点"的最近距离近似
                    glm::vec3 u, v;
                    buildPlaneBasis(axes[i], u, v);
                    constexpr int kSegments = 64;
                    ImVec2 previous;
                    bool hasPrevious = false;
                    for (int s = 0; s <= kSegments; ++s)
                    {
                        const float angle = (float)s / kSegments * 6.28318530718f;
                        const glm::vec3 point = origin + (u * std::cos(angle) + v * std::sin(angle)) * handleLength;
                        ImVec2 screen;
                        if (!projectToScreen(point, view, screen)) { hasPrevious = false; continue; }
                        if (hasPrevious)
                            d = std::min(d, distanceToSegment(mouse, previous, screen));
                        previous = screen;
                        hasPrevious = true;
                    }
                }
                else
                {
                    ImVec2 tip;
                    if (projectToScreen(origin + axes[i] * handleLength, view, tip))
                        d = distanceToSegment(mouse, center, tip);
                }

                if (d < best)
                {
                    best = d;
                    hovered = (Axis)(i + 1);
                }
            }
        }
    }

    // ---- 拖拽开始 ----
    // E5：按住 Shift 临时吸附，工具栏的开关让它常驻。两边都要看，
    // 因为用户可能只是临时对个整数位置，不想去点开关。
    const bool snapping = snapEnabled || ImGui::GetIO().KeyShift;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool consumed = (hovered != Axis::None);

    if (clicked && hovered != Axis::None && mouseInside)
    {
        glm::vec3 rayOrigin, rayDir;
        screenToRay(mouse, view, rayOrigin, rayDir);

        dragging = true;
        activeAxis = hovered;
        dragEntity = entity;
        startTranslation = transform->getTranslation();
        startRotation = transform->getEuler();
        startScale = transform->getScale();
        startOrientation = transform->getOrientation();
        dragOrigin = origin;

        // ---- E2：把整个选中集合的快照下来 ----
        dragEntries.clear();
        auto captureEntry = [&](entt::entity candidate)
        {
            if (candidate == entt::null || !registry.valid(candidate))
                return;
            if (registry.any_of<TerrainChunkComponent>(candidate) ||
                registry.any_of<TerrainStreamingComponent>(candidate) ||
                registry.any_of<CameraComponent>(candidate))
                return;
            const auto* candidateTransform = registry.try_get<TransformComponent>(candidate);
            const auto* candidateWorld = registry.try_get<WorldTransformComponent>(candidate);
            if (candidateTransform == nullptr || candidateWorld == nullptr)
                return;
            for (const DragEntry& existing : dragEntries)
                if (existing.entity == candidate)
                    return;

            DragEntry entry;
            entry.entity = candidate;
            entry.startTranslation = candidateTransform->getTranslation();
            entry.startRotation = candidateTransform->getEuler();
            entry.startScale = candidateTransform->getScale();
            entry.startOrientation = candidateTransform->getOrientation();
            entry.startWorld = candidateWorld->matrix;
            entry.parentWorldInverse = glm::mat4(1.0f);
            if (const auto* candidateHierarchy = registry.try_get<HierarchyComponent>(candidate))
            {
                if (candidateHierarchy->parent != entt::null &&
                    registry.valid(candidateHierarchy->parent))
                {
                    if (const auto* parentWorld =
                            registry.try_get<WorldTransformComponent>(candidateHierarchy->parent))
                        entry.parentWorldInverse = glm::inverse(parentWorld->matrix);
                }
            }
            dragEntries.push_back(entry);
        };
        captureEntry(entity);
        if (state.selection.size() > 1)
        {
            for (entt::entity selected : state.selection.entities)
                captureEntry(selected);
        }
        dragPrimaryParentWorld = glm::inverse(dragEntries.front().parentWorldInverse);
        dragPrimaryStartWorld = dragEntries.front().startWorld;

        const int axisIndex = (int)hovered - 1;
        dragAxisDir = axes[axisIndex];

        if (mode == GizmoMode::Rotate)
        {
            dragPlaneNormal = dragAxisDir;
            buildPlaneBasis(dragPlaneNormal, dragPlaneU, dragPlaneV);

            glm::vec3 hit;
            if (rayPlane(rayOrigin, rayDir, dragOrigin, dragPlaneNormal, hit))
            {
                const glm::vec3 v = hit - dragOrigin;
                startAngle = std::atan2(glm::dot(v, dragPlaneV), glm::dot(v, dragPlaneU));
            }
            else
            {
                startAngle = 0.0f;
            }
        }
        else
        {
            startAxisParam = closestPointOnAxis(rayOrigin, rayDir, dragOrigin, dragAxisDir);
        }

        consumed = true;
    }

    // ---- 拖拽中 ----
    if (dragging)
    {
        consumed = true;

        // 实体可能在拖拽中被删除/撤销
        if (dragEntity == entt::null || !registry.valid(dragEntity))
        {
            dragging = false;
            activeAxis = Axis::None;
            return false;
        }

        glm::vec3 rayOrigin, rayDir;
        screenToRay(mouse, view, rayOrigin, rayDir);

        auto* liveTransform = registry.try_get<TransformComponent>(dragEntity);
        if (!liveTransform)
        {
            dragging = false;
            return false;
        }

        glm::vec3 newTranslation = startTranslation;
        glm::vec3 newRotation = startRotation;
        glm::vec3 newScale = startScale;
        glm::quat newOrientation = startOrientation;

        const int axisIndex = (int)activeAxis - 1;

        if (mode == GizmoMode::Translate)
        {
            const float param = closestPointOnAxis(rayOrigin, rayDir, dragOrigin, dragAxisDir);
            // E5：吸附作用在"沿轴的位移量"上，而不是最终坐标 —— 这样物体之间的
            // 相对位置保持不变，多个物体一起拖时仍然互相平行地跳格。
            const float snapDelta = snapping
                ? snapValue(param - startAxisParam, snapTranslate)
                : (param - startAxisParam);
            const glm::vec3 worldDelta = dragAxisDir * snapDelta;

            // 世界位移 → 父节点局部空间。
            // 不做这一步的话，父节点带旋转时"往世界 X 拖"会变成"往父节点的 X 拖"。
            glm::vec3 localDelta = worldDelta;
            if (const auto* hierarchy = registry.try_get<HierarchyComponent>(dragEntity))
            {
                if (hierarchy->parent != entt::null && registry.valid(hierarchy->parent))
                {
                    if (const auto* parentWorld = registry.try_get<WorldTransformComponent>(hierarchy->parent))
                    {
                        const glm::mat4 invParent = glm::inverse(parentWorld->matrix);
                        localDelta = glm::vec3(invParent * glm::vec4(worldDelta, 0.0f));
                    }
                }
            }
            newTranslation = startTranslation + localDelta;
        }
        else if (mode == GizmoMode::Rotate)
        {
            glm::vec3 hit;
            if (rayPlane(rayOrigin, rayDir, dragOrigin, dragPlaneNormal, hit))
            {
                const glm::vec3 v = hit - dragOrigin;
                const float angle = std::atan2(glm::dot(v, dragPlaneV), glm::dot(v, dragPlaneU));
                float delta = angle - startAngle;

                // 折到 [-PI, PI]，避免绕圈时突然跳 2PI
                while (delta > 3.14159265359f) delta -= 6.28318530718f;
                while (delta < -3.14159265359f) delta += 6.28318530718f;

                // E5：角度吸附到 15 度的整数倍（弧度换算后取最近刻度）。
                if (snapping)
                    delta = snapValue(delta, glm::radians(snapRotateDegrees));

                // 在四元数空间绕**局部轴**复合：
                //   新朝向 = 旧朝向 * angleAxis(delta, 局部单位轴)
                // 旧实现直接改欧拉分量，在 T*Ry*Rx*Rz 的顺序下拖 X 轴并不等于
                // 绕当前局部 X 轴转，而且 pitch 到 ±90° 会万向节死锁。
                const glm::vec3 localAxis = (axisIndex == 0) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                           : (axisIndex == 1) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                              : glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::quat deltaRotation = glm::angleAxis(delta, localAxis);
                newOrientation = glm::normalize(startOrientation * deltaRotation);
                newRotation = TransformComponent::eulerFromQuat(newOrientation);
            }
        }
        else // Scale
        {
            const float param = closestPointOnAxis(rayOrigin, rayDir, dragOrigin, dragAxisDir);
            const float delta = param - startAxisParam;
            // 把手柄长度当作"1 倍"的参考尺度：拖出一个手柄长度 ≈ 放大 1 倍
            const float factor = 1.0f + delta / std::max(handleLength, 1e-4f);
            newScale[axisIndex] = std::max(startScale[axisIndex] * factor, 0.001f);
            // E5：缩放吸附到 0.1 的整数倍（缩放是乘性的，按绝对值吸更直观）。
            if (snapping)
                newScale[axisIndex] = std::max(snapValue(newScale[axisIndex], snapScale), 0.001f);
        }

        // E2：把 primary 的世界增量施加到整组选中实体，一条命令提交。
        std::vector<TransformPose> poses;
        poses.reserve(dragEntries.size());

        TransformPose primaryPose;
        primaryPose.entity = dragEntity;
        primaryPose.oldTranslation = startTranslation;
        primaryPose.newTranslation = newTranslation;
        primaryPose.oldRotation = startRotation;
        primaryPose.newRotation = newRotation;
        primaryPose.oldScale = startScale;
        primaryPose.newScale = newScale;
        primaryPose.oldOrientation = startOrientation;
        primaryPose.newOrientation = newOrientation;
        poses.push_back(primaryPose);

        if (dragEntries.size() > 1)
        {
            const glm::mat4 primaryLocalNew =
                glm::translate(glm::mat4(1.0f), newTranslation) *
                glm::mat4_cast(glm::normalize(newOrientation)) *
                glm::scale(glm::mat4(1.0f), newScale);
            const glm::mat4 primaryWorldNew = dragPrimaryParentWorld * primaryLocalNew;
            const glm::mat4 deltaWorld = primaryWorldNew * glm::inverse(dragPrimaryStartWorld);

            for (size_t i = 1; i < dragEntries.size(); ++i)
            {
                const DragEntry& entry = dragEntries[i];
                auto* otherTransform = registry.try_get<TransformComponent>(entry.entity);
                if (otherTransform == nullptr)
                    continue;

                const glm::mat4 localMatrix = entry.parentWorldInverse * (deltaWorld * entry.startWorld);

                glm::vec3 otherTranslation = glm::vec3(localMatrix[3]);
                glm::vec3 otherScale{
                    glm::length(glm::vec3(localMatrix[0])),
                    glm::length(glm::vec3(localMatrix[1])),
                    glm::length(glm::vec3(localMatrix[2])),
                };
                glm::mat3 rotationMatrix{
                    glm::vec3(localMatrix[0]) / std::max(otherScale.x, 1e-6f),
                    glm::vec3(localMatrix[1]) / std::max(otherScale.y, 1e-6f),
                    glm::vec3(localMatrix[2]) / std::max(otherScale.z, 1e-6f),
                };
                glm::quat otherOrientation = glm::normalize(glm::quat_cast(rotationMatrix));

                TransformPose pose;
                pose.entity = entry.entity;
                pose.oldTranslation = entry.startTranslation;
                pose.newTranslation = otherTranslation;
                pose.oldRotation = entry.startRotation;
                pose.newRotation = TransformComponent::eulerFromQuat(otherOrientation);
                pose.oldScale = entry.startScale;
                pose.newScale = otherScale;
                pose.oldOrientation = entry.startOrientation;
                pose.newOrientation = otherOrientation;
                poses.push_back(pose);
            }
        }

        // 通过命令栈提交。CommandStack 的合并窗口会把整个拖拽过程
        // 压成一条命令 —— 撤销一次就回到拖拽前，而不是退回一帧。
        state.commands.execute(std::make_unique<SetTransformsCommand>(
            registry, std::move(poses), state.services.scene));
        state.markDirty();

        // 立刻重算世界变换与包围盒，否则本帧画出来的包围盒/手柄还是旧位置
        // （UI 在 render() 里构建，而 Scene::update() 早就跑完了）。
        //
        // 这里必须**当场**刷新并重算锚点，而不是把刷新推迟到帧末：
        // 推迟的话本帧的绘制仍然用的是旧包围盒，画面照样差一帧。
        state.transformRefreshPending = false;
        if (state.services.scene)
        {
            state.services.scene->refreshTransformsAndVisibility();

            if (const auto* refreshedWorld = registry.try_get<WorldTransformComponent>(dragEntity))
            {
                origin = glm::vec3(refreshedWorld->matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                bool refreshedLocalCenter = false;
                if (const auto* refreshedLocalBounds = registry.try_get<BoundsComponent>(dragEntity))
                {
                    if (refreshedLocalBounds->getLocal().valid)
                    {
                        origin = glm::vec3(refreshedWorld->matrix *
                                           glm::vec4(refreshedLocalBounds->getLocal().center(), 1.0f));
                        refreshedLocalCenter = true;
                    }
                }
                if (!refreshedLocalCenter)
                {
                    if (const auto* refreshedBounds = registry.try_get<WorldBoundsComponent>(dragEntity))
                    {
                        if (refreshedBounds->world.valid)
                            origin = refreshedBounds->world.center();
                    }
                }

                // 旋转会改变局部轴的方向，手柄必须跟着转
                localAxes[0] = glm::normalize(glm::vec3(refreshedWorld->matrix[0]));
                localAxes[1] = glm::normalize(glm::vec3(refreshedWorld->matrix[1]));
                localAxes[2] = glm::normalize(glm::vec3(refreshedWorld->matrix[2]));
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            dragging = false;
            activeAxis = Axis::None;
        }
    }

    // ---- 缓存绘制状态，交给 draw() ----
    // 这里不直接画：调用方需要先画包围盒（用刚刷新过的包围盒），再画手柄。
    drawOrigin = origin;
    drawAxes[0] = axes[0];
    drawAxes[1] = axes[1];
    drawAxes[2] = axes[2];
    drawLength = handleLength;
    drawHovered = hovered;
    drawValid = true;

    return consumed;
}

void Gizmo::draw(const GizmoView& view) const
{
    if (!drawValid)
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(view.rectMin, view.rectMax, true);

    if (mode == GizmoMode::Rotate)
        drawRotate(dl, view, drawOrigin, drawAxes, drawLength, drawHovered);
    else if (mode == GizmoMode::Scale)
        drawScale(dl, view, drawOrigin, drawAxes, drawLength, drawHovered);
    else
        drawTranslate(dl, view, drawOrigin, drawAxes, drawLength, drawHovered);

    // 中心小圆点：视觉锚点
    ImVec2 center;
    if (projectToScreen(drawOrigin, view, center))
        dl->AddCircleFilled(center, 3.0f, IM_COL32(230, 230, 230, 200));

    dl->PopClipRect();
}

} // namespace editor
