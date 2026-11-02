#include "Renderer.h"
#include <stdexcept>
#include <array>
#include <iostream>

Renderer::Renderer(Device& device, Swapchain& swapchain)
    : vulkanDevice(device), vulkanSwapchain(swapchain)
{
    createCommandPools();
    createCommandBuffers();
    createSyncObjects();

    std::cout << "[Renderer] D-4/D-5 active: " << kFramesInFlight
              << " frames in flight, graphics + compute timelines, "
              << (vulkanDevice.hasDedicatedComputeQueue() ? "dedicated async compute" : "compute fallback on graphics queue")
              << "\n";
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(vulkanDevice.getDevice());

    for (VkSemaphore semaphore : imageAvailableSemaphores)
        if (semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(vulkanDevice.getDevice(), semaphore, nullptr);

    for (VkSemaphore semaphore : renderFinishedSemaphores)
        if (semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(vulkanDevice.getDevice(), semaphore, nullptr);

    if (graphicsTimelineSemaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(vulkanDevice.getDevice(), graphicsTimelineSemaphore, nullptr);
    if (computeTimelineSemaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(vulkanDevice.getDevice(), computeTimelineSemaphore, nullptr);

    if (computeCommandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vulkanDevice.getDevice(), computeCommandPool, nullptr);
    vkDestroyCommandPool(vulkanDevice.getDevice(), commandPool, nullptr);
}

void Renderer::createCommandPools()
{
    QueueFamilyIndices queueFamilyIndices = vulkanDevice.findPhysicalQueueFamilies();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();
    if (vkCreateCommandPool(vulkanDevice.getDevice(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create graphics command pool!");

    // 即使 compute fallback 到同一个 family，也保留独立 pool：D-5 以后需要
    // 给 compute 命令单独重置/回收时，不会和 graphics 命令相互影响。
    poolInfo.queueFamilyIndex = queueFamilyIndices.computeFamily.value();
    if (vkCreateCommandPool(vulkanDevice.getDevice(), &poolInfo, nullptr, &computeCommandPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute command pool!");
}

void Renderer::createCommandBuffers()
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFramesInFlight;

    allocInfo.commandPool = commandPool;
    if (vkAllocateCommandBuffers(vulkanDevice.getDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate per-frame graphics command buffers!");

    allocInfo.commandPool = computeCommandPool;
    if (vkAllocateCommandBuffers(vulkanDevice.getDevice(), &allocInfo, computeCommandBuffers.data()) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate per-frame compute command buffers!");
}

void Renderer::createSyncObjects()
{
    VkSemaphoreCreateInfo binaryInfo{};
    binaryInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (VkSemaphore& semaphore : imageAvailableSemaphores)
    {
        if (vkCreateSemaphore(vulkanDevice.getDevice(), &binaryInfo, nullptr, &semaphore) != VK_SUCCESS)
            throw std::runtime_error("failed to create image-available semaphore!");
    }

    renderFinishedSemaphores.resize(vulkanSwapchain.imageCount(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : renderFinishedSemaphores)
    {
        if (vkCreateSemaphore(vulkanDevice.getDevice(), &binaryInfo, nullptr, &semaphore) != VK_SUCCESS)
            throw std::runtime_error("failed to create render-finished semaphore!");
    }

    VkSemaphoreTypeCreateInfo timelineType{};
    timelineType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 0;

    VkSemaphoreCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timelineInfo.pNext = &timelineType;

    if (vkCreateSemaphore(vulkanDevice.getDevice(), &timelineInfo, nullptr, &graphicsTimelineSemaphore) != VK_SUCCESS)
        throw std::runtime_error("failed to create graphics timeline semaphore!");
    if (vkCreateSemaphore(vulkanDevice.getDevice(), &timelineInfo, nullptr, &computeTimelineSemaphore) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute timeline semaphore!");
}

uint64_t Renderer::getCompletedGraphicsFrame()
{
    if (graphicsTimelineSemaphore == VK_NULL_HANDLE)
        return 0;

    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(vulkanDevice.getDevice(), graphicsTimelineSemaphore, &value) != VK_SUCCESS)
        throw std::runtime_error("failed to query graphics timeline semaphore!");
    return value;
}

void Renderer::waitForGraphicsFrame(uint64_t frameNumber)
{
    waitForSemaphoreValue(graphicsTimelineSemaphore, frameNumber);
}

void Renderer::waitForSemaphoreValue(VkSemaphore semaphore, uint64_t value) const
{
    if (value == 0)
        return;

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &semaphore;
    waitInfo.pValues = &value;

    if (vkWaitSemaphores(vulkanDevice.getDevice(), &waitInfo, UINT64_MAX) != VK_SUCCESS)
        throw std::runtime_error("failed to wait for timeline semaphore!");
}

void Renderer::waitForFrameSlot(uint32_t frameInFlightIndex)
{
    // 两条队列都可能访问这个 slot 的资源，所以两个 timeline 都必须完成。
    waitForSemaphoreValue(graphicsTimelineSemaphore, graphicsSubmissionValues[frameInFlightIndex]);
    waitForSemaphoreValue(computeTimelineSemaphore, computeSubmissionValues[frameInFlightIndex]);
}

VkCommandBuffer Renderer::beginFrame()
{
    currentFrameInFlightIndex = static_cast<uint32_t>(frameCounter % kFramesInFlight);
    waitForFrameSlot(currentFrameInFlightIndex);

    VkSemaphore acquireSemaphore = imageAvailableSemaphores[currentFrameInFlightIndex];
    const VkResult acquireResult = vkAcquireNextImageKHR(
        vulkanDevice.getDevice(), vulkanSwapchain.getSwapchain(), UINT64_MAX,
        acquireSemaphore, VK_NULL_HANDLE, &currentImageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // No image acquired and imageAvailableSemaphore was not signaled, so it
        // is safe to return and let FirstApp recreate the swapchain next frame.
        swapchainRecreateRequested = true;
        return VK_NULL_HANDLE;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("failed to acquire swapchain image!");

    if (acquireResult == VK_SUBOPTIMAL_KHR)
    {
        // Image is still usable. Draw/present this frame, then rebuild between
        // frames so the imageAvailable semaphore is consumed normally.
        swapchainRecreateRequested = true;
    }

    VkCommandBuffer graphicsCommandBuffer = commandBuffers[currentFrameInFlightIndex];
    VkCommandBuffer computeCommandBuffer = computeCommandBuffers[currentFrameInFlightIndex];

    if (vkResetCommandBuffer(graphicsCommandBuffer, 0) != VK_SUCCESS ||
        vkResetCommandBuffer(computeCommandBuffer, 0) != VK_SUCCESS)
        throw std::runtime_error("failed to reset command buffers!");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(graphicsCommandBuffer, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("failed to begin graphics command buffer!");
    if (vkBeginCommandBuffer(computeCommandBuffer, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("failed to begin compute command buffer!");

    ++frameCounter;   // 当前帧的 1-based 逻辑帧号，也作为两条 timeline 的 value
    return graphicsCommandBuffer;
}

void Renderer::onSwapchainRecreated()
{
    // FirstApp has already waited for the whole device to become idle.
    for (VkSemaphore& semaphore : renderFinishedSemaphores)
    {
        if (semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(vulkanDevice.getDevice(), semaphore, nullptr);
    }

    VkSemaphoreCreateInfo binaryInfo{};
    binaryInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinishedSemaphores.assign(vulkanSwapchain.imageCount(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : renderFinishedSemaphores)
    {
        if (vkCreateSemaphore(vulkanDevice.getDevice(), &binaryInfo, nullptr, &semaphore) != VK_SUCCESS)
            throw std::runtime_error("failed to recreate render-finished semaphore!");
    }

    currentImageIndex = 0;
    swapchainRecreateRequested = false;
}

void Renderer::endFrame(bool graphicsWaitsForCompute, bool computeWaitsForGraphics)
{
    if (graphicsWaitsForCompute && computeWaitsForGraphics)
        throw std::runtime_error("Renderer: 同一帧不能同时要求 graphics 等 compute 和 compute 等 graphics");

    VkCommandBuffer graphicsCommandBuffer = commandBuffers[currentFrameInFlightIndex];
    VkCommandBuffer computeCommandBuffer = computeCommandBuffers[currentFrameInFlightIndex];
    const uint64_t signalValue = frameCounter;

    if (vkEndCommandBuffer(graphicsCommandBuffer) != VK_SUCCESS ||
        vkEndCommandBuffer(computeCommandBuffer) != VK_SUCCESS)
        throw std::runtime_error("failed to end command buffers!");

    // ---- 提交函数：两条队列各自一条命令缓冲 ----
    // TD-02 只允许单向依赖。依赖方向由 RenderGraph 根据资源声明算出；
    // 需要等待时用 timeline semaphore 把两条独立 submit 串起来。
    auto submitCompute = [&](bool waitForGraphics)
    {
        uint64_t waitValue = signalValue;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        VkTimelineSemaphoreSubmitInfo timelineSubmit{};
        timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineSubmit.waitSemaphoreValueCount = waitForGraphics ? 1u : 0u;
        timelineSubmit.pWaitSemaphoreValues = waitForGraphics ? &waitValue : nullptr;
        timelineSubmit.signalSemaphoreValueCount = 1;
        timelineSubmit.pSignalSemaphoreValues = &signalValue;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = &timelineSubmit;
        if (waitForGraphics)
        {
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &graphicsTimelineSemaphore;
            submitInfo.pWaitDstStageMask = &waitStage;
        }
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &computeCommandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &computeTimelineSemaphore;

        const VkResult computeSubmit = vkQueueSubmit(vulkanDevice.getComputeQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (computeSubmit != VK_SUCCESS)
        {
            if (computeSubmit == VK_ERROR_DEVICE_LOST) vulkanDevice.reportDeviceFault();
            throw std::runtime_error("failed to submit compute command buffer!");
        }

        computeSubmissionValues[currentFrameInFlightIndex] = signalValue;
    };

    auto submitGraphics = [&](bool waitForCompute)
    {
        VkSemaphore waitSemaphores[2] = {
            imageAvailableSemaphores[currentFrameInFlightIndex],
            computeTimelineSemaphore
        };
        uint64_t waitValues[2] = { 0, signalValue };
        VkPipelineStageFlags waitStages[2] = {
            // 放宽到 ALL_COMMANDS：交换链图像可能在 COLOR_ATTACHMENT_OUTPUT 之前的
            // 阶段（如 FRAGMENT_SHADER / *_FRAGMENT_TESTS 的布局转换）被访问，
            // 只等 COLOR_ATTACHMENT_OUTPUT 会让同步校验报与 acquire 的 WRITE_AFTER_READ。
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
        };
        const uint32_t waitCount = waitForCompute ? 2u : 1u;

        VkSemaphore signalSemaphores[2] = {
            renderFinishedSemaphores[currentImageIndex],
            graphicsTimelineSemaphore
        };
        uint64_t signalValues[2] = { 0, signalValue };

        VkTimelineSemaphoreSubmitInfo timelineSubmit{};
        timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineSubmit.waitSemaphoreValueCount = waitCount;
        timelineSubmit.pWaitSemaphoreValues = waitValues;
        timelineSubmit.signalSemaphoreValueCount = 2;
        timelineSubmit.pSignalSemaphoreValues = signalValues;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = &timelineSubmit;
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &graphicsCommandBuffer;
        submitInfo.signalSemaphoreCount = 2;
        submitInfo.pSignalSemaphores = signalSemaphores;

        const VkResult graphicsSubmit = vkQueueSubmit(vulkanDevice.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (graphicsSubmit != VK_SUCCESS)
        {
            if (graphicsSubmit == VK_ERROR_DEVICE_LOST) vulkanDevice.reportDeviceFault();
            throw std::runtime_error("failed to submit draw command buffer!");
        }

        graphicsSubmissionValues[currentFrameInFlightIndex] = signalValue;
        lastSubmittedGraphicsFrame = signalValue;
    };

    if (computeWaitsForGraphics)
    {
        // graphics 先写，compute 后读/写：graphics submit 先入队，compute 等它的 timeline。
        submitGraphics(false);
        submitCompute(true);
    }
    else
    {
        // 无依赖时保持原来的并行提交；C->G 时 graphics 等 compute timeline。
        submitCompute(false);
        submitGraphics(graphicsWaitsForCompute);
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    VkSemaphore presentWaitSemaphore = renderFinishedSemaphores[currentImageIndex];
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &presentWaitSemaphore;
    VkSwapchainKHR swapChains[] = { vulkanSwapchain.getSwapchain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &currentImageIndex;

    const VkResult presentResult = vkQueuePresentKHR(vulkanDevice.getPresentQueue(), &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
    {
        swapchainRecreateRequested = true;
        return;
    }
    if (presentResult != VK_SUCCESS)
    {
        if (presentResult == VK_ERROR_DEVICE_LOST) vulkanDevice.reportDeviceFault();
        throw std::runtime_error("failed to present swapchain image!");
    }
}
