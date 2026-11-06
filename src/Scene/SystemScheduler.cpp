#include "Scene/SystemScheduler.h"

#include <algorithm>

namespace scene
{

std::vector<SystemEntry>& SystemScheduler::storage(SystemPhase phase)
{
    return phase == SystemPhase::Simulation ? simulation : postTransform;
}

const std::vector<SystemEntry>& SystemScheduler::storage(SystemPhase phase) const
{
    return phase == SystemPhase::Simulation ? simulation : postTransform;
}

bool SystemScheduler::add(std::string id, SystemPhase phase, int order,
                          std::function<void(entt::registry&, float)> update)
{
    if (id.empty() || !update)
        return false;

    for (const SystemEntry& entry : simulation)
    {
        if (entry.id == id)
            return false;
    }
    for (const SystemEntry& entry : postTransform)
    {
        if (entry.id == id)
            return false;
    }

    auto& list = storage(phase);
    SystemEntry entry;
    entry.id = std::move(id);
    entry.order = order;
    entry.update = std::move(update);
    list.push_back(std::move(entry));

    std::stable_sort(list.begin(), list.end(),
                     [](const SystemEntry& a, const SystemEntry& b)
                     {
                         return a.order < b.order;
                     });
    return true;
}

bool SystemScheduler::remove(const std::string& id)
{
    bool removed = false;
    for (std::vector<SystemEntry>* list : { &simulation, &postTransform })
    {
        const auto it = std::find_if(list->begin(), list->end(),
                                     [&id](const SystemEntry& entry)
                                     {
                                         return entry.id == id;
                                     });
        if (it != list->end())
        {
            list->erase(it);
            removed = true;
        }
    }
    return removed;
}

void SystemScheduler::run(entt::registry& registry, SystemPhase phase, float deltaSeconds)
{
    // 拷贝一份再跑：系统内部允许注册/移除其他系统，不能边迭代边改原数组。
    const std::vector<SystemEntry> entries = storage(phase);
    for (const SystemEntry& entry : entries)
    {
        if (entry.update)
            entry.update(registry, deltaSeconds);
    }
}

const std::vector<SystemEntry>& SystemScheduler::systems(SystemPhase phase) const
{
    return storage(phase);
}

} // namespace scene
