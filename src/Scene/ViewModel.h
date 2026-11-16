#pragma once

// D3：第一人称手持物（viewmodel）的相机空间参数与独立投影。
//
// 为什么不能直接复用场景相机：武器挂在相机前方几十厘米处，贴墙时它已经
// 陷进墙体，用场景的近平面/深度缓冲会被裁掉半截。所以 viewmodel 有自己的
// 近远平面（0.01..10 米）和自己的深度缓冲，位置永远相对相机，深度永远压在
// 世界几何之上——这就是"贴墙不裁切"的实现方式。

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>

namespace scene
{

struct ViewModelSettings
{
    float fovDegrees = 54.0f;
    float nearPlane = 0.01f;
    float farPlane = 10.0f;
    glm::vec3 offset{ 0.28f, -0.24f, 0.55f };    // 相机空间位置（米，+z 朝前）
    glm::vec3 rotation{ 0.0f, -14.0f, 0.0f };    // 欧拉角（度，Y→X→Z）
    glm::vec3 scale{ 0.055f };
    glm::vec3 color{ 0.42f, 0.44f, 0.48f };
};

// 与 VulkanCamera 同一套约定：右手视图空间、Vulkan 的 0..1 深度、Y 轴翻转。
glm::mat4 viewModelProjection(float fovDegrees, float aspectRatio, float nearPlane,
                              float farPlane);

// 相机空间里的 TRS：先平移、再 Y→X→Z 旋转、最后缩放。
glm::mat4 viewModelOffsetMatrix(const ViewModelSettings& settings);

bool viewModelSelfTest();

} // namespace scene