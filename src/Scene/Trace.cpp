#include "Scene/Trace.h"

#include <cmath>
#include <iostream>

namespace scene
{
namespace
{

bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << "[Trace] selfTest FAIL: " << message << "\n";
    return false;
}

bool near(float a, float b) { return std::abs(a - b) < 1.0e-6f; }

} // namespace

bool traceContractSelfTest()
{
    bool ok = true;
    int cases = 0;

    // ---- 1) a fresh result means "no hit" ----
    {
        ++cases;
        const TraceResult result;
        ok = expect(!result.hit(), "default result reports a hit") && ok;
        ok = expect(near(result.fraction, 1.0f), "default fraction is not 1") && ok;
        ok = expect(result.surface == TraceSurface::None, "default result has a surface") && ok;
        ok = expect(result.entity.isNull(), "default result carries an entity") && ok;
        ok = expect(!result.normalValid, "default result claims a normal") && ok;
    }

    // ---- 2) grazing hits stay misses (Source DidHit + epsilon) ----
    {
        ++cases;
        TraceResult grazing;
        grazing.fraction = 1.0f - 5.0e-5f;   // inside kTraceFractionEpsilon
        ok = expect(!grazing.hit(), "a grazing fraction counted as a hit") && ok;

        TraceResult nearly;
        nearly.fraction = 1.0f - 2.0e-4f;    // outside the epsilon
        ok = expect(nearly.hit(), "a fraction past the epsilon did not count") && ok;

        TraceResult half;
        half.fraction = 0.5f;
        ok = expect(half.hit(), "a half-way fraction did not count") && ok;

        TraceResult solid;
        solid.allSolid = true;
        ok = expect(solid.hit(), "allSolid did not count as a hit") && ok;

        TraceResult stuck;
        stuck.startSolid = true;
        ok = expect(stuck.hit(), "startSolid did not count as a hit") && ok;
    }

    // ---- 3) request semantics: delta is a displacement, not a direction ----
    {
        ++cases;
        TraceRequest request;
        request.start = glm::vec3(1.0f, 2.0f, 3.0f);
        request.delta = glm::vec3(0.0f, -2.0f, 0.0f);
        ok = expect(near(request.length(), 2.0f), "request length is not |delta|") && ok;
        ok = expect(request.isRay(), "zero extents did not read as a ray") && ok;

        TraceRequest swept = request;
        swept.extents = glm::vec3(0.35f, 0.9f, 0.35f);
        ok = expect(!swept.isRay(), "non-zero extents still read as a ray") && ok;
        ok = expect(swept != request, "requests that differ only in extents compare equal") && ok;
        ok = expect(swept == swept, "a request does not equal itself") && ok;

        TraceResult hit;
        hit.fraction = 0.25f;
        ok = expect(near(hit.distance(request), 0.5f), "distance is not fraction * |delta|") && ok;
        const glm::vec3 point = hit.hitPoint(request);
        ok = expect(near(point.y, 1.5f) && near(point.x, 1.0f),
                    "hit point is not start + delta * fraction") && ok;
    }

    // ---- 4) surface masks ----
    {
        ++cases;
        ok = expect(hasSurface(TraceSurface::All, TraceSurface::Terrain), "All misses terrain") && ok;
        ok = expect(hasSurface(TraceSurface::All, TraceSurface::Voxel), "All misses voxel") && ok;
        ok = expect(hasSurface(TraceSurface::All, TraceSurface::Model), "All misses model") && ok;
        ok = expect((TraceSurface::Terrain & TraceSurface::Voxel) == TraceSurface::None,
                    "terrain and voxel masks overlap") && ok;
        ok = expect(hasSurface(TraceSurface::Terrain | TraceSurface::Voxel, TraceSurface::Voxel),
                    "union mask lost a surface") && ok;
        ok = expect(!hasSurface(TraceSurface::Terrain, TraceSurface::Model),
                    "a terrain-only mask claimed model") && ok;
    }

    // ---- 5) startSolid reports the ray origin, not a displaced point ----
    {
        ++cases;
        TraceRequest request;
        request.start = glm::vec3(4.0f, 5.0f, 6.0f);
        request.delta = glm::vec3(0.0f, -10.0f, 0.0f);
        TraceResult stuck;
        stuck.startSolid = true;
        stuck.fraction = 0.0f;
        const glm::vec3 point = stuck.hitPoint(request);
        ok = expect(near(point.x, 4.0f) && near(point.y, 5.0f) && near(point.z, 6.0f),
                    "startSolid hit point is not the ray origin") && ok;
    }

    if (ok)
        std::cout << "[Trace] selfTest PASS | cases=" << cases
                  << " epsilon=" << kTraceFractionEpsilon << "\n";
    return ok;
}

} // namespace scene