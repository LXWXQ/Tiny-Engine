#pragma once

// ============================================================================
// P1-1：Tracy CPU 剖析挂钩。
//
// 编译期由 CMake option TINY_ENABLE_TRACY 控制（默认 OFF）：
//   * OFF -> 下面所有宏展开为空，零开销、不引入任何 Tracy 头；
//   * ON  -> 需要 3rdParty/tracy（public/tracy/Tracy.hpp + TracyClient.cpp），
//            由 CMake 自动加入 include 目录与源文件。
//
// GPU 侧每 pass 的 zone 见 docs/60-debug-profiling/diagnostics.md；等 Tracy 就位后
// 在 RenderGraph 的 pass 循环里补 TracyVkZone（同样受该宏保护）。
// ============================================================================
#ifdef TINY_ENABLE_TRACY
#  include "tracy/Tracy.hpp"
#  define TINY_PROFILE_FRAME_MARK() FrameMark
#  define TINY_ZONE_SCOPED(name)    ZoneScopedN(name)
#else
#  define TINY_PROFILE_FRAME_MARK() ((void)0)
#  define TINY_ZONE_SCOPED(name)    ((void)0)
#endif