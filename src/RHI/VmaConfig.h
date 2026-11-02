#pragma once

// ===========================================================================
// VMA 的编译期配置。
//
// **必须在这个头里、在 vk_mem_alloc.h 之前定义。**
// VMA 的不少结构体布局会随这些宏变化，所以每个包含 vk_mem_alloc.h 的翻译
// 单元都得看到一致的配置（不一致 = ODR 违规 = 极难查的诡异崩溃）。
// 因此统一从 Device.h 进来，不要让各处自己 define。
//
// 给分配起名字的辅助在包含 vk_mem_alloc.h **之后**，见 VmaNaming.h。
//
// 排查内存泄漏时打开（默认关，它会明显变慢）：
//     cmake -S . -B build -DSENTINEL_VMA_LEAK_CHECK=ON
// ===========================================================================

#ifdef SENTINEL_VMA_LEAK_CHECK
#  include <cstdio>

// vmaDestroyAllocator 发现"有分配没释放"时默认只丢一句断言，不说是谁。
// 换成我们自己的输出，把每条未释放的分配（含名字）打出来。
#  define VMA_LEAK_LOG_FORMAT(format, ...) \
     do { std::printf("[VMA 泄漏] " format "\n", __VA_ARGS__); } while (0)
#endif
