#include "Scene/EntityHandle.h"

#include <iostream>
#include <vector>

namespace scene
{
namespace
{

// Local probe component: the self test must not depend on the scene's component
// set so it can run before any Scene (and before Vulkan) exists.
struct HandleProbeComponent
{
    int value = 0;
};

bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << "[Handle] selfTest FAIL: " << message << "\n";
    return false;
}

} // namespace

bool entityHandleSelfTest()
{
    bool ok = true;
    int cases = 0;

    // ---- 1) a default handle is null and never alive ----
    {
        ++cases;
        const EntityHandle nullHandle;
        entt::registry registry;
        ok = expect(nullHandle.isNull(), "default-constructed handle is not null") && ok;
        ok = expect(nullHandle.entity() == entt::null, "default-constructed handle is not entt::null") && ok;
        ok = expect(!nullHandle.alive(registry), "null handle reports alive") && ok;
        ok = expect(nullHandle.tryGet<HandleProbeComponent>(registry) == nullptr,
                    "null handle resolved a component") && ok;
    }

    // ---- 2) 1000 create/destroy: every stale handle must stay dead ----
    {
        ++cases;
        constexpr int kCount = 1000;
        entt::registry registry;
        std::vector<EntityHandle> handles;
        handles.reserve(kCount);
        for (int i = 0; i < kCount; ++i)
        {
            const entt::entity entity = registry.create();
            registry.emplace<HandleProbeComponent>(entity).value = i;
            handles.emplace_back(entity);
        }

        for (const EntityHandle& handle : handles)
            ok = expect(handle.alive(registry), "handle died while its entity is alive") && ok;
        ok = expect(handles.front().tryGet<HandleProbeComponent>(registry) != nullptr,
                    "living handle did not resolve its component") && ok;

        for (const EntityHandle& handle : handles)
            registry.destroy(handle.entity());

        for (const EntityHandle& handle : handles)
        {
            ok = expect(!handle.alive(registry), "stale handle still reports alive") && ok;
            ok = expect(handle.tryGet<HandleProbeComponent>(registry) == nullptr,
                        "stale handle resolved a component") && ok;
        }
    }

    // ---- 3) index reuse bumps the version: the old handle must not alias ----
    {
        ++cases;
        entt::registry registry;
        const EntityHandle stale(registry.create());
        const uint32_t recycledIndex = stale.index();
        registry.destroy(stale.entity());

        const EntityHandle fresh(registry.create());
        ok = expect(fresh.index() == recycledIndex,
                    "entity index was not recycled (self test precondition)") && ok;
        ok = expect(fresh.version() != stale.version(),
                    "recycled index kept the same version") && ok;
        ok = expect(!stale.alive(registry), "stale handle aliased the recycled entity") && ok;
        ok = expect(stale != fresh, "stale and recycled handles compare equal") && ok;
        ok = expect(fresh.alive(registry), "recycled handle is not alive") && ok;
    }

    // ---- 4) copy / equality semantics ----
    {
        ++cases;
        entt::registry registry;
        const EntityHandle a(registry.create());
        const EntityHandle b = a;
        const EntityHandle c(registry.create());
        ok = expect(a == b, "copied handle lost its identity") && ok;
        ok = expect(a != c, "distinct handles compare equal") && ok;
        ok = expect(a.raw() == b.raw(), "copied handle changed its raw value") && ok;
    }

    // ---- 5) handle -> entity round trip matches entt's own decoding ----
    {
        ++cases;
        entt::registry registry;
        const entt::entity entity = registry.create();
        const EntityHandle handle(entity);
        ok = expect(handle.entity() == entity, "handle did not round-trip to its entity") && ok;
        ok = expect(handle.raw() == static_cast<uint32_t>(entt::to_integral(entity)),
                    "handle raw value does not match entt::to_integral") && ok;
    }

    if (ok)
        std::cout << "[Handle] selfTest PASS | cases=" << cases
                  << " handles=1000 version-recycle=ok\n";
    return ok;
}

} // namespace scene