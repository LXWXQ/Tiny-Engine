#pragma once

// ---------------------------------------------------------------------------
// M1-A3: trace contract.
//
// Source answers every "what does this ray hit" question with one type:
// CGameTrace (public/gametrace.h:30). The engine has four separate answers today -
// raycastVoxelWorld, raycastTerrain/raycastTerrainWorld and Model::intersectRay -
// and each one returns a different slice of the same information (block coords,
// hit point + distance, or distance only). This header is the shared vocabulary
// they converge on; call sites keep their current entry points until a consumer
// actually needs the unified one.
//
// Deliberate differences from Source (see docs/source-migration/20-geometry/):
//   * no m_StartOffset: the engine already rebases world origin, offsetting the
//     ray on top of that would double-correct;
//   * the hit entity is an EntityHandle, not a raw CBaseEntity*;
//   * hitbox is not folded into a single int with a second meaning.
//
// C++17 (CMakeLists.txt:4), so operator== is written out instead of defaulted.
// ---------------------------------------------------------------------------

#include <glm/glm.hpp>

#include <cstdint>

#include "Scene/EntityHandle.h"

namespace scene
{

// What a trace is allowed to hit. Source calls this the contents mask; it lives
// on the request instead of on a #define table so it cannot drift out of sync.
enum class TraceSurface : uint32_t
{
    None = 0u,
    Terrain = 1u << 0,
    Voxel = 1u << 1,
    Model = 1u << 2,
    All = Terrain | Voxel | Model,
};

constexpr TraceSurface operator|(TraceSurface a, TraceSurface b) noexcept
{
    return static_cast<TraceSurface>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr TraceSurface operator&(TraceSurface a, TraceSurface b) noexcept
{
    return static_cast<TraceSurface>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr bool hasSurface(TraceSurface set, TraceSurface bit) noexcept
{
    return (set & bit) != TraceSurface::None;
}

// Source DIST_EPSILON (public/coordsize.h:35) is 0.03125 source units. This engine
// works in metres, so the equivalent "close enough to count as grazing" distance
// is 1e-4 m instead of a full 1/32 of a unit.
inline constexpr float kTraceFractionEpsilon = 1.0e-4f;

// glm::vec3 operator== returns a bvec3, not a bool, so equality is written out.
inline bool sameVector(const glm::vec3& a, const glm::vec3& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Input of one trace. `delta` is the FULL displacement, not a unit vector
// (Source Ray_t.m_Delta), so |delta| is the range and the result can report both
// fraction and distance without a second length parameter.
struct TraceRequest
{
    glm::vec3 start{0.0f};
    glm::vec3 delta{0.0f};
    // Half extents of the swept box around the segment; zero means a thin ray.
    // Non-zero extents are what a character controller or a camera wants.
    glm::vec3 extents{0.0f};
    TraceSurface surfaces = TraceSurface::All;

    bool isRay() const noexcept
    {
        return extents.x == 0.0f && extents.y == 0.0f && extents.z == 0.0f;
    }
    float length() const noexcept { return glm::length(delta); }

    bool operator==(const TraceRequest& other) const noexcept
    {
        return sameVector(start, other.start) && sameVector(delta, other.delta) &&
               sameVector(extents, other.extents) && surfaces == other.surfaces;
    }
    bool operator!=(const TraceRequest& other) const noexcept { return !(*this == other); }
};

// Output of one trace - the CGameTrace equivalent.
struct TraceResult
{
    // 1 = went the whole way without hitting anything.
    float fraction = 1.0f;
    glm::vec3 endPos{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    // false while a backend cannot derive a surface normal (terrain: height field
    // gradient is not wired up yet). Consumers must check this instead of assuming
    // `normal` means something.
    bool normalValid = false;
    float planeDist = 0.0f;
    bool allSolid = false;
    bool startSolid = false;
    TraceSurface surface = TraceSurface::None;
    EntityHandle entity{};

    // Source CGameTrace::DidHit (public/gametrace.h:88-90) without the epsilon gap:
    // a fraction of 0.99999 must not count as a hit.
    bool hit() const noexcept
    {
        return fraction < 1.0f - kTraceFractionEpsilon || allSolid || startSolid;
    }
    float distance(const TraceRequest& request) const noexcept
    {
        return fraction * request.length();
    }
    glm::vec3 hitPoint(const TraceRequest& request) const noexcept
    {
        return startSolid ? request.start : request.start + request.delta * fraction;
    }
};

// Contract self test, no Vulkan / no window: TinyEngine.exe --trace-test
bool traceContractSelfTest();

} // namespace scene