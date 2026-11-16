#pragma once
#include <cstdint>
#include <glm/glm.hpp>

#include "Scene/Components.h"

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

    // M1-C1c：这个实体的世界矩阵该不该插值。
    //
    // 只有"在刚结束的那个 tick 里真的动过"（changedTick == renderTick）才插值。
    // 之前只看 alpha > 0，结果静态实体拿着一个陈旧的 previousMatrix 一直插值：
    // 地形 chunk 生成时 setTranslation(chunkCenter) 之后就再也不脏，渲染出来
    // 被放到 alpha * chunkCenter 上，每个 chunk 比例还不同 —— 运行模式下整片
    // 地形朝原点收缩又弹回，看着就是闪烁。
    bool shouldInterpolateTransform(const WorldTransformComponent& transform,
                                    float alpha, uint64_t renderTick);

    // Contract self test. Needs neither Vulkan nor a window.
    bool renderInterpolationSelfTest();
}
