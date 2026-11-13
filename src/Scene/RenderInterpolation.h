#pragma once
#include <glm/glm.hpp>

namespace scene
{
    // M1-C1c: render interpolation.
    //
    // Simulation advances at a fixed 60Hz while rendering follows the monitor
    // refresh rate (at 144Hz a frame often advances zero or one tick). Feeding
    // the raw current-tick world matrix to the renderer makes the same pose
    // repeat for two or three frames, which reads as stepping. Here the
    // previous tick matrix and the current one are blended by
    // SimulationClock::alpha(), so the renderer draws a continuous pose that
    // lags the simulation by at most one tick.
    //
    // Why decompose TRS instead of a per-element lerp: a per-element lerp of
    // two rotation matrices shrinks the basis vectors (the length of a linear
    // combination of two orthonormal vectors is below one), so a turning model
    // visibly collapses. Interpolating translation/scale with lerp and the
    // rotation with slerp keeps rigid bodies rigid.
    //
    // alpha semantics: <= 0 or >= 1 returns current unchanged; a failed
    // decomposition (degenerate or non-finite matrix) also returns current.
    // Interpolation is a purely visual optimisation and must never move
    // geometry to a wrong place.
    glm::mat4 interpolateTransform(const glm::mat4& previous, const glm::mat4& current,
                                   float alpha);

    // Contract self test. Needs neither Vulkan nor a window.
    bool renderInterpolationSelfTest();
}
