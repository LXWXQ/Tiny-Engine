#pragma once

// ===========================================================================
// 视口内的三轴操作手柄（Gizmo）
//
// 三件事：
//   1. 把选中实体的世界原点投影到屏幕，画出 X/Y/Z 三个手柄；
//   2. 命中测试（鼠标离哪根轴最近）；
//   3. 拖拽时把鼠标位移换算成世界空间的变换增量，并通过命令栈提交（可撤销）。
//
// 为什么自己写而不是接 ImGuizmo：
//   本项目没有把 ImGuizmo 放进 3rdParty，而且它的数学是围绕 ImGui 的
//   矩阵约定写的，接进来要额外做一次坐标系转换。这里的交互模型很简单
//   （单轴约束），自己实现反而更可控、更好调试。
//
// 坐标系约定（重要）：
//   * 平移：手柄画在**世界轴**上，位移按世界空间算，再折算回父节点的局部空间；
//   * 旋转 / 缩放：手柄画在**实体的局部轴**上（列向量取自世界矩阵），
//     因为 TransformComponent 的 rotation/scale 本身就是局部的，
//     用局部轴才不会出现"拖 X 轴结果物体斜着转"的割裂感。
// ===========================================================================

#include <imgui.h>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

namespace editor
{

struct EditorState;

enum class GizmoMode
{
    Translate,
    Rotate,
    Scale
};

// 视口投影所需的只读视图信息。由 ViewportPanel 每帧填好。
struct GizmoView
{
    ImVec2 rectMin{ 0.0f, 0.0f };   // 视口内容区左上角（屏幕像素）
    ImVec2 rectMax{ 0.0f, 0.0f };   // 右下角
    glm::mat4 view{ 1.0f };
    glm::mat4 projection{ 1.0f };
    glm::vec3 cameraPosition{ 0.0f };
};

class Gizmo
{
public:
    GizmoMode mode = GizmoMode::Translate;

    // 处理命中测试与拖拽（会修改 transform 并立刻刷新世界变换）。
    // 返回 true 表示这一帧"鼠标交互被手柄吃掉"（调用方不应再去做点选）。
    //
    // 为什么和 draw() 拆开：
    //   包围盒线框必须在**交互之后**画（那时包围盒才是最新的），
    //   而手柄要压在包围盒上面。所以调用方按 interact -> 画框 -> draw 的顺序走。
    //   如果合成一个 manipulate()，就必然有一个先画、一个用旧数据。
    bool interact(EditorState& state, const GizmoView& view);

    // 画手柄。必须在 interact() 之后调用（它用的是 interact 缓存下来的锚点）。
    void draw(const GizmoView& view) const;

    // 正在拖拽（用于禁用相机输入 / 显示提示）
    bool isDragging() const { return dragging; }

    // 纯数学自检：投影/反投影往返、射线与轴最近点、射线与平面求交。
    // 这些换算错一个符号，表现就是"点不中物体"或"拖反方向"，
    // 而两者都不会有任何运行时错误 —— 只能靠断言钉住。
    static bool selfTest();

private:
    enum class Axis { None, X, Y, Z };

    // 世界空间 → 屏幕像素。w<=0 表示在相机背后。
    static bool projectToScreen(const glm::vec3& world, const GizmoView& view, ImVec2& outScreen);

    // 屏幕像素 → 世界射线
    static void screenToRay(const ImVec2& screen, const GizmoView& view,
                            glm::vec3& outOrigin, glm::vec3& outDirection);

    // 射线与直线最近点的参数（沿 axis 的距离）
    static float closestPointOnAxis(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                    const glm::vec3& axisOrigin, const glm::vec3& axisDir);

    // 射线与平面求交
    static bool rayPlane(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                         const glm::vec3& planePoint, const glm::vec3& planeNormal,
                         glm::vec3& outPoint);

    // 鼠标到线段的屏幕距离
    static float distanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b);

    void drawTranslate(ImDrawList* drawList, const GizmoView& view, const glm::vec3& origin,
                       const glm::vec3 axes[3], float length, Axis hovered) const;
    void drawRotate(ImDrawList* drawList, const GizmoView& view, const glm::vec3& origin,
                    const glm::vec3 axes[3], float radius, Axis hovered) const;
    void drawScale(ImDrawList* drawList, const GizmoView& view, const glm::vec3& origin,
                   const glm::vec3 axes[3], float length, Axis hovered) const;

    // ---- interact() 缓存给 draw() 用的绘制状态 ----
    glm::vec3 drawOrigin{ 0.0f };
    glm::vec3 drawAxes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    float drawLength = 1.0f;
    Axis drawHovered = Axis::None;
    bool drawValid = false;

    // ---- 拖拽状态 ----
    bool dragging = false;
    Axis activeAxis = Axis::None;
    entt::entity dragEntity = entt::null;

    // 拖拽开始时的完整变换（撤销时要回到这里）
    glm::vec3 startTranslation{ 0.0f };
    glm::vec3 startRotation{ 0.0f };
    glm::vec3 startScale{ 1.0f };

    // 拖拽开始时的参考量
    float startAxisParam = 0.0f;    // 平移 / 缩放：沿轴的距离
    float startAngle = 0.0f;        // 旋转：起始角度
    glm::vec3 dragOrigin{ 0.0f };   // 拖拽开始时的实体世界原点
    glm::vec3 dragAxisDir{ 0.0f };  // 拖拽轴（世界空间，已归一化）
    glm::vec3 dragPlaneNormal{ 0.0f };
    glm::vec3 dragPlaneU{ 0.0f };   // 旋转平面内的正交基
    glm::vec3 dragPlaneV{ 0.0f };
};

} // namespace editor
