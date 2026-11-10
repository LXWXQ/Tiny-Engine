#pragma once

#include "Device.h"

#include <vulkan/vulkan.h>

#include <cstdint>

// F6-05：体积云使用的 3D 纹理。
//
// 现有 Texture 只支持 2D（材质/天空盒），而体积云需要在整个云层里连续采样
// 形状噪声。把噪声烘进 64^3 的 RGBA8 里，比每像素解析 3D Perlin/Worley 便宜
// 一个数量级，也让着色器里的 raymarch 只需要纹理采样。
class Texture3D
{
public:
    Texture3D(Device& device, const void* pixels,
              uint32_t width, uint32_t height, uint32_t depth,
              VkFormat format, VkCommandPool commandPool);
    ~Texture3D();

    Texture3D(const Texture3D&) = delete;
    Texture3D& operator=(const Texture3D&) = delete;

    VkImageView getView() const { return view; }
    VkSampler getSampler() const { return sampler; }
    VkImage getImage() const { return image; }

private:
    Device& vulkanDevice;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};
