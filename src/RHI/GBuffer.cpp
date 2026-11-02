#include "GBuffer.h"
#include <iostream>

GBuffer::GBuffer(Device& device, VkExtent2D extent)
    : device(device), currentExtent(extent) 
{
    std::cout << "[Sentinel 通报] 开始在显存中开辟 G-Buffer 防区...\n";

    createAttachment(GBufferFormat::Position, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &position);
    createAttachment(GBufferFormat::Normal,   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &normal);
    createAttachment(GBufferFormat::Albedo,   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &albedo);
    createAttachment(GBufferFormat::Pbr,      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &pbr);
    createAttachment(device.findDepthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth);

    // 创建 G-Buffer 专属采样器
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST; 
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    
    if (vkCreateSampler(device.getDevice(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) 
    {
        throw std::runtime_error("[致命错误] G-Buffer 采样器创建失败！");
    }
    std::cout << "[Sentinel 通报] G-Buffer 锻造完成！5 张底片已就绪。\n";
}

GBuffer::~GBuffer() 
{
    destroyAttachment(position);
    destroyAttachment(normal);
    destroyAttachment(albedo);
    destroyAttachment(pbr);
    destroyAttachment(depth);

    if (sampler != VK_NULL_HANDLE) 
    {
        vkDestroySampler(device.getDevice(), sampler, nullptr);
    }
}

void GBuffer::resize(VkExtent2D extent)
{
    if (extent.width == currentExtent.width && extent.height == currentExtent.height)
        return;
    if (extent.width == 0 || extent.height == 0)
        throw std::runtime_error("[GBuffer] resize received zero extent");

    destroyAttachment(position);
    destroyAttachment(normal);
    destroyAttachment(albedo);
    destroyAttachment(pbr);
    destroyAttachment(depth);

    currentExtent = extent;
    createAttachment(GBufferFormat::Position, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &position);
    createAttachment(GBufferFormat::Normal,   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &normal);
    createAttachment(GBufferFormat::Albedo,   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &albedo);
    createAttachment(GBufferFormat::Pbr,      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &pbr);
    createAttachment(device.findDepthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth);
}

void GBuffer::createAttachment(VkFormat format, VkImageUsageFlags usage, FrameBufferAttachment* attachment) 
{
    attachment->format = format;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = currentExtent.width;
    imageInfo.extent.height = currentExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(device.getVmaAllocator(), &imageInfo, &allocInfo,
        &attachment->image, &attachment->allocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("[致命错误] G-Buffer 附件创建失败！");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    viewInfo.image = attachment->image;

    if (vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &attachment->view) != VK_SUCCESS)
        throw std::runtime_error("[致命错误] G-Buffer ImageView 创建失败！");
}

void GBuffer::destroyAttachment(FrameBufferAttachment& attachment) 
{
    if (attachment.view != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), attachment.view, nullptr);
    if (attachment.image != VK_NULL_HANDLE) vmaDestroyImage(device.getVmaAllocator(), attachment.image, attachment.allocation);
    attachment = {};
}