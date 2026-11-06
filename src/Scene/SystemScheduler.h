#pragma once

// ===========================================================================
// SystemScheduler：按阶段 / 顺序运行的 ECS 系统
//
// 系统只拿 entt::registry 和 dt，不依赖 Scene。这样做有两个好处：
//   1. 自检可以直接跑系统，不需要 Vulkan 设备；
//   2. 以后把系统拆到独立线程或 JobSystem 时，接口不用改。
//
// 阶段语义（由 Scene::update 调用）：
//   Simulation    -> updateWorldTransforms() 之前，用于旋转/环绕这类局部变换
//   PostTransform -> 世界变换刷新之后，用于依赖世界坐标的逻辑
// ===========================================================================

#include <entt/entt.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace scene
{

enum class SystemPhase
{
    Simulation,
    PostTransform,
};

struct SystemEntry
{
    std::string id;
    int order = 0;
    std::function<void(entt::registry&, float)> update;
};

class SystemScheduler
{
public:
    // order 越小越先运行；同一 order 保持注册顺序（稳定排序）。
    bool add(std::string id, SystemPhase phase, int order,
             std::function<void(entt::registry&, float)> update);

    bool remove(const std::string& id);

    void run(entt::registry& registry, SystemPhase phase, float deltaSeconds);

    const std::vector<SystemEntry>& systems(SystemPhase phase) const;
    std::size_t systemCount() const { return simulation.size() + postTransform.size(); }

private:
    std::vector<SystemEntry>& storage(SystemPhase phase);
    const std::vector<SystemEntry>& storage(SystemPhase phase) const;

    std::vector<SystemEntry> simulation;
    std::vector<SystemEntry> postTransform;
};

} // namespace scene
