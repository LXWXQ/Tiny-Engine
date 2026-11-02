#pragma once

#include "Device.h"
#include "Swapchain.h"
#include "Core/FrameInFlight.h"
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <memory>

class Renderer 
{
public:
    Renderer(Device& device, Swapchain& swapchain);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 核心接口：控制帧的生命周期
    VkCommandBuffer beginFrame();
    // TD-02：跨队列依赖方向来自 RenderGraph::getCrossQueueDependencies()。
    // 两个参数同时为 true 表示同一帧出现双向依赖，直接报错。
    void endFrame(bool graphicsWaitsForCompute = false,
                  bool computeWaitsForGraphics = false);

    // D-4：两个索引必须分开。
    //   frameInFlightIndex -> 本帧独占的 CPU/GPU 资源槽位（UBO、瞬时图等）
    //   currentImageIndex  -> 本次 acquire 到的交换链图像
    uint32_t getCurrentImageIndex() const { return currentImageIndex; }
    uint32_t getCurrentFrameInFlightIndex() const { return currentFrameInFlightIndex; }
    uint64_t getFrameNumber() const { return frameCounter; }
    uint64_t getLastSubmittedGraphicsFrame() const { return lastSubmittedGraphicsFrame; }

    // TD-04: graphics timeline is monotonic. A completed value X means every
    // graphics submission up to frame X has retired, which is exactly what the
    // bindless material-slot retirement queue needs.
    uint64_t getCompletedGraphicsFrame();
    void waitForGraphicsFrame(uint64_t frameNumber);

    // TD-03: acquire/present can report out-of-date or suboptimal. The app
    // rebuilds the swapchain between frames, never in the middle of recording.
    bool needsSwapchainRecreate() const { return swapchainRecreateRequested; }
    void onSwapchainRecreated();

    VkCommandPool getCommandPool() const { return commandPool; }
    VkCommandPool getComputeCommandPool() const { return computeCommandPool; }
    VkCommandBuffer getComputeCommandBuffer() const { return computeCommandBuffers[currentFrameInFlightIndex]; }

private:
    void createCommandPools();
    void createCommandBuffers();
    void createSyncObjects();
    void waitForSemaphoreValue(VkSemaphore semaphore, uint64_t value) const;
    void waitForFrameSlot(uint32_t frameInFlightIndex);

    Device& vulkanDevice;
    Swapchain& vulkanSwapchain;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandPool computeCommandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
    std::array<VkCommandBuffer, kFramesInFlight> computeCommandBuffers{};

    // WSI 的二值 semaphore。
    // imageAvailable 每帧槽位一枚：它总会被同一槽位的下一次 submit 等待，
    // 而 CPU 在复用槽位前已经等过 timeline semaphore。
    // renderFinished 每张交换链图像一枚：present 的消费完成时间不受 submit
    // fence / timeline 的完成时间约束，按图像分开才能安全复用。
    std::array<VkSemaphore, kFramesInFlight> imageAvailableSemaphores{};
    std::vector<VkSemaphore> renderFinishedSemaphores;

    // D-4：graphics timeline semaphore 负责「这个 frame slot 的 graphics 工作是否完成」。
    // D-5：compute 使用独立 timeline；两条队列即使没有互相依赖，也必须各自完成后
    // 才能复用该 frame slot 的命令缓冲 / UBO / 瞬时资源。
    VkSemaphore graphicsTimelineSemaphore = VK_NULL_HANDLE;
    VkSemaphore computeTimelineSemaphore = VK_NULL_HANDLE;
    std::array<uint64_t, kFramesInFlight> graphicsSubmissionValues{};
    std::array<uint64_t, kFramesInFlight> computeSubmissionValues{};

    uint64_t frameCounter = 0;          // 1-based while a frame is being recorded
    uint64_t lastSubmittedGraphicsFrame = 0;
    uint32_t currentImageIndex = 0;
    uint32_t currentFrameInFlightIndex = 0;
    bool swapchainRecreateRequested = false;
};
