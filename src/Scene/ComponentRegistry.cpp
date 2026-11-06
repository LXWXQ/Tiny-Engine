#include "Scene/ComponentRegistry.h"

#include <algorithm>

namespace scene
{

bool ComponentRegistry::add(ComponentDescriptor descriptor)
{
    if (descriptor.type.empty() || descriptor.label.empty())
        return false;
    if (!descriptor.addDefault || !descriptor.has || !descriptor.remove ||
        !descriptor.read || !descriptor.write || !descriptor.defaults)
        return false;

    const auto existing = std::find_if(
        registered.begin(), registered.end(),
        [&descriptor](const ComponentDescriptor& candidate)
        {
            return candidate.type == descriptor.type;
        });
    if (existing != registered.end())
        return false;

    registered.push_back(std::move(descriptor));
    return true;
}

const ComponentDescriptor* ComponentRegistry::find(const std::string& type) const
{
    for (const ComponentDescriptor& descriptor : registered)
    {
        if (descriptor.type == type)
            return &descriptor;
    }
    return nullptr;
}

} // namespace scene
