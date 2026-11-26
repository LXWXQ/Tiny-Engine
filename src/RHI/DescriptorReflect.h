#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// P3-5 第二步：把描述符的「类型」也交给构建期反射。
//
// tools/SpirvReflect.cpp 给每个描述符额外写一行 `k<Name>Kind = N`，N 是下面这组
// 稳定编号。引擎侧用它换出 VkDescriptorType，于是「GLSL 写的是什么类型」这件事
// 只有一个来源；调用点再用 static_assert 声明自己的期望，两边不一致就编译失败。
//
// 编号表与 SpirvReflect.cpp 的 descriptorKindCode() 一一对应，改一边必须改另一边。
// ---------------------------------------------------------------------------
namespace tiny::reflect
{
inline constexpr uint32_t kKindUnmapped = 0;                // other / buffer / unknown
inline constexpr uint32_t kKindSampler = 1;
inline constexpr uint32_t kKindCombinedImageSampler = 2;
inline constexpr uint32_t kKindImage = 3;                   // 可能是 storage 也可能是 sampled
inline constexpr uint32_t kKindUniformBuffer = 4;
inline constexpr uint32_t kKindStorageBuffer = 5;
inline constexpr uint32_t kKindUnknownKind = 6;

constexpr VkDescriptorType descriptorTypeOf(uint32_t kind)
{
switch (kind)
{
case kKindSampler:
return VK_DESCRIPTOR_TYPE_SAMPLER;
case kKindCombinedImageSampler:
return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
case kKindUniformBuffer:
return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
case kKindStorageBuffer:
return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
default:
return VK_DESCRIPTOR_TYPE_MAX_ENUM;  // 未映射：调用点的 static_assert 会挡住
}
}

// 直接当常量用：kDescriptorType<lighting_frag::kUboKind>
template <uint32_t Kind>
inline constexpr VkDescriptorType kDescriptorType = descriptorTypeOf(Kind);
}
