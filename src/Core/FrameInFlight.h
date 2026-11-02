#pragma once

#include <cstdint>

// 同时在飞的帧数。
//
// 2 帧足以让 CPU 录制第 N+1 帧时 GPU 还在执行第 N 帧；再往上加会线性增加
// 所有瞬时渲染目标（G-Buffer / SSAO / HDR / Bloom）的显存占用。
// D-4 先固定为 2，等有 GPU 计时数据后再决定是否升到 3。
inline constexpr uint32_t kFramesInFlight = 2;
