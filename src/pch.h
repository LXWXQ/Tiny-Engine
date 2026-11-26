#pragma once

// ---------------------------------------------------------------------------
// 预编译头
//
// 只放两类东西：
//   1) 标准库头 —— <vector> 被 95 个 TU 包含，<string> 83 个，<memory> 49 个；
//   2) 体积大、内容稳定、没有宏开关的第三方头 —— glm 被 52 个 TU 包含，
//      vulkan.h 36 个。这几个头每次解析都要几万到十几万行，是编译时间的大头。
//
// 刻意排除的（不要加进来）：
//   * stb_image.h    —— Application.cpp 用它展开 STB_IMAGE_IMPLEMENTATION
//   * tiny_gltf.h    —— GLTFLoader.cpp 用它展开 TINYGLTF_IMPLEMENTATION
//   * vk_mem_alloc.h —— 需要 VMA_IMPLEMENTATION，且对 Vulkan 头的包含顺序敏感
// 这三个头一旦进 PCH，实现体会在 140 个 TU 里各展开一次，直接重复符号。
//
// 想验证某个头该不该进 PCH，最快的方法是看它被多少个 TU 包含：
//   grep -rl "include.*glm/glm.hpp" src | wc -l
// 低于 10 个的通常不值得，因为它会把 PCH 撑大，而收益只落在那几个文件上。
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <functional>
#include <algorithm>
#include <utility>
#include <numeric>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>

// glm 的两个开关目前散落在 Camera.h / Model.h / ViewModel.h 里各自 #define。
// PCH 先于这些头展开，所以这里必须先设好，否则 glm 会按"深度 -1..1"实例化，
// 与工程其余部分的 0..1 约定冲突（症状是投影矩阵、阴影深度整体错位）。
#ifndef GLM_FORCE_RADIANS
#  define GLM_FORCE_RADIANS
#endif
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#  define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
