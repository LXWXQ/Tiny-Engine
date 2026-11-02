#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "RHI/VmaConfig.h"   // 必须在 vk_mem_alloc.h 之前
#include <vk_mem_alloc.h>
#include "RHI/VmaNaming.h"   // 必须在 vk_mem_alloc.h 之后
#include <vector>
#include <optional>
#include <string>

#include "Core/Diagnostics.h"   // diag::Options（P0：调试/验证开关）

struct QueueFamilyIndices 
{
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    // D-5：优先选择独立 compute family；没有独立队列时回退到 graphics family。
    std::optional<uint32_t> computeFamily;

    bool isComplete() 
    {
        return graphicsFamily.has_value() && presentFamily.has_value() && computeFamily.has_value();
    }
};

class Device
{
public:
    // P0：options 携带验证层增强开关（--validation-features）。
    Device(GLFWwindow* window, const diag::Options& options = {});
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    
    VkDevice getDevice() { return device; }
    VkPhysicalDevice getPhysicalDevice() { return physicalDevice; }
    VkSurfaceKHR getSurface() { return surface; }
    VkQueue getGraphicsQueue() { return graphicsQueue; }
	VkQueue getPresentQueue() { return presentQueue; }
	VkQueue getComputeQueue() { return computeQueue; }
	bool hasDedicatedComputeQueue() const { return dedicatedComputeQueue; }
	bool supportsMultiDrawIndirect() const { return multiDrawIndirectSupported; }
	bool supportsDrawIndirectCount() const { return drawIndirectCountSupported; }
	VkInstance getInstance() { return instance; }

// P2-4：VK_EXT_device_fault。设备不支持时 reportDeviceFault() 返回 false。
bool isDeviceFaultSupported() const { return deviceFaultSupported; }
bool reportDeviceFault();
bool hasDeviceFaultEntryPoint() const { return pfnGetDeviceFaultInfoEXT != nullptr; }
	VmaAllocator getVmaAllocator() { return vmaAllocator; }

	// 打印 VMA 统计，并返回"还有多少个分配没释放"。
	// 为什么需要：vmaDestroyAllocator 在前者不为 0 时会直接断言崩掉，而断言
	// 只说"有分配没释放"，不说是谁。在析构链中间插一次调用，就能把范围缩到
	// "两次调用之间被销毁的那些对象"——比读代码猜快得多。
	size_t reportVmaStats(const char* stage);
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
    VkFormat findDepthFormat();
    QueueFamilyIndices findPhysicalQueueFamilies() { return findQueueFamilies(physicalDevice); }
    struct SwapChainSupportDetails 
    {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();

    bool isDeviceSuitable(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    std::vector<const char*> getRequiredExtensions();
    bool checkValidationLayerSupport();

    GLFWwindow* window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device;
	VkQueue graphicsQueue;
	VkQueue presentQueue;
	VkQueue computeQueue;
	bool dedicatedComputeQueue = false;
	bool multiDrawIndirectSupported = false;
	bool drawIndirectCountSupported = false;
bool deviceFaultSupported = false;
// P2-4：扩展函数必须经 vkGetDeviceProcAddr 获取（导入库不导出它）。
PFN_vkGetDeviceFaultInfoEXT pfnGetDeviceFaultInfoEXT = nullptr;
	VmaAllocator vmaAllocator;

    // P0-2：验证层增强（best practices / 同步校验），来自命令行 --validation-features。
    bool validationBestPractices = false;
    bool validationSync = false;
    bool validationDebugPrintf = false;
    bool validationGpuAssisted = false;

    const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
    const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    const bool enableValidationLayers = true;
#endif
};