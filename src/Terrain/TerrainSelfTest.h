#pragma once

namespace terrain
{

// CPU-only regression coverage for config validation, deterministic noise,
// height-field bounds, mesh topology, normals, winding and non-square spacing.
bool selfTest();

} // namespace terrain