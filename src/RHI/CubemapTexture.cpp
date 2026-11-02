#include "RHI/CubemapTexture.h"
#include <stdexcept>
#include <iostream>

CubemapTexture::CubemapTexture(Device& device, uint32_t faceSize, uint32_t mipLevels, VkFormat format,
                               const std::string& debugName)
    : device(device), faceSize(faceSize), mipLevels(mipLevels), format(format)
{
    if (faceSize == 0 || mipLevels == 0)
        throw std::runtime_error("CubemapTexture: faceSize / mipLevels 不能为 0");

    createImage();
    createViews();
    createSampler();

    if (!debugName.empty())
        std::cout << "[Cubemap] " << debugName << "  " << faceSize << "x" << faceSize
                  << "   mips " << mipLevels << "\n";
}

CubemapTexture::~CubemapTexture()
{
    for (VkImageView v : faceViews)
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), v, nullptr);

    if (cubeView != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), cubeView, nullptr);
    if (sampler != VK_NULL_HANDLE) vkDestroySampler(device.getDevice(), sampler, nullptr);
    if (image != VK_NULL_HANDLE) vmaDestroyImage(device.getVmaAllocator(), image, allocation);
}

void CubemapTexture::createImage()
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { faceSize, faceSize, 1 };
    info.mipLevels = mipLevels;
    info.arrayLayers = 6;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC / TRANSFER_DST 是给生成 mip 链用的（vkCmdBlitImage）
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 关键：不设这个 flag 就只能当 2D 数组用，不能作为 samplerCube 采样
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(device.getVmaAllocator(), &info, &alloc, &image, &allocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("CubemapTexture: 图像创建失败");
}

void CubemapTexture::createViews()
{
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image;
    ci.format = format;

    // 1) 完整 cube view
    ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6 };
    if (vkCreateImageView(device.getDevice(), &ci, nullptr, &cubeView) != VK_SUCCESS)
        throw std::runtime_error("CubemapTexture: cube view 创建失败");

    // 2) 每个 (mip, face) 一个 2D view，用作 Framebuffer 附件
    faceViews.resize(static_cast<size_t>(mipLevels) * 6, VK_NULL_HANDLE);
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;

    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        for (uint32_t face = 0; face < 6; ++face)
        {
            ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1 };
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device.getDevice(), &ci, nullptr, &view) != VK_SUCCESS)
                throw std::runtime_error("CubemapTexture: face view 创建失败");
            faceViews[static_cast<size_t>(mip) * 6 + face] = view;
        }
    }
}

void CubemapTexture::createSampler()
{
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = static_cast<float>(mipLevels);
    si.maxAnisotropy = 1.0f;

    if (vkCreateSampler(device.getDevice(), &si, nullptr, &sampler) != VK_SUCCESS)
        throw std::runtime_error("CubemapTexture: 采样器创建失败");
}

VkImageView CubemapTexture::getFaceView(uint32_t mip, uint32_t face) const
{
    if (mip >= mipLevels || face >= 6)
        throw std::runtime_error("CubemapTexture: getFaceView 越界");
    return faceViews[static_cast<size_t>(mip) * 6 + face];
}