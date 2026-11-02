#pragma once

#include "Device.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

// 立方体贴图。
// 用于 IBL 的 irradiance map / prefiltered environment map，
// 以及把等距柱状 HDR 转成的源立方体。
//
// 内部同时维护两类视图：
//   * 一个完整的 VK_IMAGE_VIEW_TYPE_CUBE，供着色器用 samplerCube 采样
//   * 每个 (mip, face) 一个 VK_IMAGE_VIEW_TYPE_2D，供 Framebuffer 做渲染目标
class CubemapTexture
{
public:
    CubemapTexture(Device& device, uint32_t faceSize, uint32_t mipLevels, VkFormat format,
                   const std::string& debugName = "");
    ~CubemapTexture();

    CubemapTexture(const CubemapTexture&) = delete;
    CubemapTexture& operator=(const CubemapTexture&) = delete;

    VkImage getImage() const { return image; }
    VkImageView getView() const { return cubeView; }                 // 完整 cube view
    VkImageView getFaceView(uint32_t mip, uint32_t face) const;      // 渲染目标用的单面 view
    VkSampler getSampler() const { return sampler; }

    uint32_t getFaceSize() const { return faceSize; }
    uint32_t getMipLevels() const { return mipLevels; }
    uint32_t getFaceSizeAtMip(uint32_t mip) const { return faceSize >> mip; }

private:
    void createImage();
    void createViews();
    void createSampler();

    Device& device;
    uint32_t faceSize = 0;
    uint32_t mipLevels = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView cubeView = VK_NULL_HANDLE;

    // 下标：mip * 6 + face
    std::vector<VkImageView> faceViews;
    VkSampler sampler = VK_NULL_HANDLE;
};