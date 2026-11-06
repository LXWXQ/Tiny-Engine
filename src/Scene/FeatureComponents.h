#pragma once

// ===========================================================================
// 内置示例特性组件
//
// 这两个组件是 ECS 框架的“样板”：
//   - 任意实体挂 SpinComponent 就绕自身轴旋转；
//   - 任意实体挂 OrbitComponent 就沿水平圆环绕中心公转。
//
// 它们不写任何渲染代码，只改 Transform；其余系统（世界变换、空间索引、
// 可见性、提交）照旧工作。以后加“漂浮 / 脉冲 / 生命周期 / 触发器”等特性，
// 复制这一对“组件 + 系统 + 注册函数”即可。
// ===========================================================================

#include "Scene/ComponentRegistry.h"
#include "Scene/SystemScheduler.h"

#include <glm/glm.hpp>

namespace scene
{

struct SpinComponent
{
    glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
    float degreesPerSecond = 45.0f;
    bool enabled = true;
};

struct OrbitComponent
{
    glm::vec3 center{ 0.0f };
    float radius = 3.0f;
    float degreesPerSecond = 30.0f;
    // 当前相位（度）。它也是属性，用户可以在 Inspector 里设起始角度。
    float angle = 0.0f;
    bool enabled = true;
};

// 注册 Spin / Orbit 的元数据。重复调用是安全的：ComponentRegistry::add
// 会拒绝同 id 的第二个描述。
void registerFeatureComponents(ComponentRegistry& registry);

// 注册 effects.spin / effects.orbit 到 Simulation 阶段。
void registerFeatureSystems(SystemScheduler& scheduler);

} // namespace scene
