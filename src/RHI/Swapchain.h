#pragma once

#include "Device.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

class Swapchain 
{
    public:
        Swapchain(Device& deviceRef, VkExtent2D windowExtent);
        ~Swapchain();

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;

        VkSwapchainKHR getSwapchain() const { return swapChain; }
        VkFormat getSwapChainImageFormat() const { return swapChainImageFormat; }
        VkExtent2D getSwapChainExtent() const { return swapChainExtent; }
        uint32_t imageCount() const { return static_cast<uint32_t>(swapChainImages.size()); }
        VkImageView getImageView(int index) { return swapChainImageViews[index]; }
        VkImage     getImage(int index) { return swapChainImages[index]; }

        // TD-03: recreate the swapchain in place so Renderer/Passes keep their
        // reference to this object. Caller must ensure the GPU is idle.
        void recreate(VkExtent2D newWindowExtent);

    private:
        void createSwapChain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
        void createImageViews();
        void cleanup();
        
        Device& device;
        VkExtent2D windowExtent;

        VkSwapchainKHR swapChain = VK_NULL_HANDLE;
        std::vector<VkImage> swapChainImages;
        std::vector<VkImageView> swapChainImageViews;
        VkFormat swapChainImageFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D swapChainExtent{};
};