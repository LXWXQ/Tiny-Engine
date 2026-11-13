#pragma once

// ---------------------------------------------------------------------------
// M1-A1: entity handle contract.
//
// Why this exists (docs/source-migration/10-core/01-entity-handles.md):
// the Source engine packs a 16 bit entry index and a 16 bit serial into one
// uint32 so a stale handle can be detected after the slot is reused. EnTT's
// entt::entity already stores exactly that pair, so the value type is not the
// problem we have - ownership and invalidation are. This is therefore a thin
// typed wrapper, not a second store:
//   * EntityHandle owns nothing and destroys nothing;
//   * Scene::destroyEntity stays the single teardown exit;
//   * the wrapper makes "reference held across frames" a visible type in
//     signatures (undo stacks, editor selection, async bake results) and
//     forces call sites to say whether they mean "not null" or "still alive".
//
// Not serializable on purpose: index+version is per-session state. Persistence
// references entities by name / uuid, never by handle.
// ---------------------------------------------------------------------------

#include <entt/entt.hpp>

#include <cstdint>

namespace scene
{

class EntityHandle
{
public:
    constexpr EntityHandle() noexcept = default;
    constexpr explicit EntityHandle(entt::entity entity) noexcept : id(entity) {}

    [[nodiscard]] constexpr entt::entity entity() const noexcept { return id; }

    // "was never set" - not "still alive". Two very different questions.
    [[nodiscard]] constexpr bool isNull() const noexcept { return id == entt::null; }

    // "still refers to the same living entity". A recycled index answers false
    // because EnTT bumps the version when the slot is reused.
    [[nodiscard]] bool alive(const entt::registry& registry) const noexcept
    {
        return id != entt::null && registry.valid(id);
    }

    template <typename T>
    [[nodiscard]] const T* tryGet(const entt::registry& registry) const
    {
        return alive(registry) ? registry.try_get<T>(id) : nullptr;
    }

    // Diagnostics only (logs, undo payloads, crash dumps). Never persist this
    // value: index+version from another session means nothing here.
    [[nodiscard]] constexpr uint32_t raw() const noexcept
    {
        return static_cast<uint32_t>(entt::to_integral(id));
    }
    [[nodiscard]] constexpr uint32_t index() const noexcept
    {
        return static_cast<uint32_t>(entt::to_entity(id));
    }
    [[nodiscard]] constexpr uint32_t version() const noexcept
    {
        return static_cast<uint32_t>(entt::to_version(id));
    }

    friend constexpr bool operator==(EntityHandle lhs, EntityHandle rhs) noexcept
    {
        return lhs.id == rhs.id;
    }
    friend constexpr bool operator!=(EntityHandle lhs, EntityHandle rhs) noexcept
    {
        return lhs.id != rhs.id;
    }

private:
    entt::entity id = entt::null;
};

// Contract self test, no Vulkan / no window: TinyEngine.exe --handle-test
bool entityHandleSelfTest();

} // namespace scene