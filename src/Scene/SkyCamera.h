#pragma once

// ---------------------------------------------------------------------------
// 3D 天空盒（sky camera）
//
// 2D 天空盒是一张贴图，贴在所有东西后面：玩家走多远，山都在同一个角上，
// 完全没有视差。Source 的做法是把远处的地形建筑按 1/16 缩小摆进地图一角，
// 再放一台 sky_camera；渲染时把这块微缩世界放大回去，并且跟着玩家走，
// 于是它看起来无限远，但山与山之间仍有真实的视差和光照。
//
// 这里用同样的思路，只是把相机锁定的部分写成一次矩阵乘法：
//
//     skyMatrix = translate(cameraPos) * scale(1/s) * translate(-origin)
//
// 顶点 X 落在 sky 世界里的 origin 处时，渲染位置正好是玩家相机的位置；
// 离 origin 一个单位的山，会被放到 1/s 个单位外（默认 16 倍），所以：
//   - 整块天空随玩家平移（看起来无限远，不会走近）；
//   - 天空内部的山与山之间仍有视差（旋转视角时错位是真实的）。
//
// 几何本身是普通实体（MeshComponent + RenderTagComponent::isSkyGeometry），
// 走独立管线画在远平面（gl_Position.z = w），不写深度、不参与阴影。
// ---------------------------------------------------------------------------

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace scene
{

struct SkyCameraSettings
{
    bool enabled = true;
    // 微缩世界里对应玩家所在位置的那个点（通常摆在山脚下）。
    glm::vec3 origin{ 0.0f, 0.0f, 0.0f };
    // 微缩比例：0.0625 = 1/16，与 Source 的 sky_camera 默认一致。
    float scale = 0.0625f;
};
// 天空世界 -> 相机锁定世界的变换（不含实体自身的世界矩阵）。
glm::mat4 skyCameraMatrix(const SkyCameraSettings& settings, const glm::vec3& cameraPosition);

// 天空实体的最终模型矩阵（已经包含相机锁定，着色器里不需要再加 cameraPos）。
glm::mat4 skyEntityMatrix(const SkyCameraSettings& settings, const glm::vec3& cameraPosition,
                          const glm::mat4& worldMatrix);

// 契约自检：不需要 Vulkan / 窗口。
bool skyCameraSelfTest();

} // namespace scene
