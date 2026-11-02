#include "Swapchain.h"
#include <stdexcept>
#include <algorithm>
#include <iostream>

Swapchain::Swapchain(Device& deviceRef, VkExtent2D windowExtent)
    : device(deviceRef), windowExtent(windowExtent) 
{
    createSwapChain();
    createImageViews();
}

Swapchain::~Swapchain() 
{
    cleanup();
}

void Swapchain::cleanup()
{
    for (VkImageView imageView : swapChainImageViews)
    {
        if (imageView != VK_NULL_HANDLE)
            vkDestroyImageView(device.getDevice(), imageView, nullptr);
    }
    swapChainImageViews.clear();

    swapChainImages.clear();
    if (swapChain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device.getDevice(), swapChain, nullptr);
        swapChain = VK_NULL_HANDLE;
    }
}

void Swapchain::recreate(VkExtent2D newWindowExtent)
{
    // Caller (FirstApp) performs vkDeviceWaitIdle before calling this.
    windowExtent = newWindowExtent;

    const VkSwapchainKHR oldSwapchain = swapChain;
    swapChain = VK_NULL_HANDLE;

    // Image views belong to the old swapchain. Old
    // swapchain itself stays alive until vkCreateSwapchainKHR returns so the
    // driver can reuse its resources.
    for (VkImageView view : swapChainImageViews)
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device.getDevice(), view, nullptr);
    swapChainImageViews.clear();

    createSwapChain(oldSwapchain);
    if (oldSwapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device.getDevice(), oldSwapchain, nullptr);

    createImageViews();

    std::cout << "[Swapchain] Recreated: " << swapChainExtent.width << "x"
              << swapChainExtent.height << ", images=" << swapChainImages.size() << "\n";
}

void Swapchain::createSwapChain(VkSwapchainKHR oldSwapchain) 
{
    Device::SwapChainSupportDetails swapChainSupport = device.querySwapChainSupport(device.getPhysicalDevice());

    VkSurfaceFormatKHR surfaceFormat = swapChainSupport.formats[0];
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;//VK_PRESENT_MODE_FIFO_KHR 垂直同步  VK_PRESENT_MODE_IMMEDIATE_KHR 无垂直同步，可能会撕裂 VK_PRESENT_MODE_MAILBOX_KHR 低延迟，适合高帧率，可能会丢帧

    // Windows 下窗口被标题栏/任务栏挤占时，请求的客户区尺寸可能被系统裁小。
    // 只要 surface 给出了 currentExtent，交换链就必须严格使用它，否则验证层
    // 会报 imageExtent 越界，随后 acquire/present 全部失效。
    VkExtent2D extent = windowExtent;
    if (swapChainSupport.capabilities.currentExtent.width != UINT32_MAX)
    {
        extent = swapChainSupport.capabilities.currentExtent;
    }
    else
    {
        extent.width = std::clamp(extent.width,
            swapChainSupport.capabilities.minImageExtent.width,
            swapChainSupport.capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height,
            swapChainSupport.capabilities.minImageExtent.height,
            swapChainSupport.capabilities.maxImageExtent.height);
    }

    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) 
    {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = device.getSurface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
#ifdef SENTINEL_ENABLE_SCREENSHOT
    // P1-4：把交换链图像拷到 staging buffer 需要 TRANSFER_SRC。
    createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif

    QueueFamilyIndices indices = device.findPhysicalQueueFamilies();
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) 
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } 
    else 
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    if (vkCreateSwapchainKHR(device.getDevice(), &createInfo, nullptr, &swapChain) != VK_SUCCESS) 
    {
        throw std::runtime_error("failed to create swap chain!");
    }

    vkGetSwapchainImagesKHR(device.getDevice(), swapChain, &imageCount, nullptr);
    swapChainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device.getDevice(), swapChain, &imageCount, swapChainImages.data());

    swapChainImageFormat = surfaceFormat.format;
    swapChainExtent = extent;
}

void Swapchain::createImageViews() 
{
    swapChainImageViews.resize(swapChainImages.size());

    for (size_t i = 0; i < swapChainImages.size(); i++) 
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapChainImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapChainImageFormat;

        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device.getDevice(), &createInfo, nullptr, &swapChainImageViews[i]) != VK_SUCCESS) 
        {
            throw std::runtime_error("failed to create image views!");
        }
    }
}
