#pragma once
#include "RHI/Device.h"
#include "Scene/Components.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include "Core/FrameInfo.h"

class ImGuiSystem
{
public:
	// D-1(b) 起改用动态渲染：不再需要 VkRenderPass，只需颜色附件格式
	ImGuiSystem(GLFWwindow* window, Device& device, VkFormat swapchainFormat, uint32_t imageCount, VkCommandPool commandPool);
	~ImGuiSystem();
	void newFrame();
	ImGuiSystem(const ImGuiSystem&) = delete;
	ImGuiSystem& operator=(const ImGuiSystem&) = delete;

	// D-2 起拆成两步：
	//   prepareDrawData() 只构建 UI 并生成绘制列表（不含任何 GPU 命令），
	//                     必须在后效链的最后一步之前调用，否则面板里改的值
	//                     要等到下一帧才生效（会出现一帧延迟的滑块手感）；
	//   renderDrawData()  才是真正的 vkCmdDraw，由 PresentPass 在自己的
	//                     动态渲染作用域里调用，保证 UI 画在最终图像之上。
	void prepareDrawData(FrameInfo& frameInfo);
	void renderDrawData(VkCommandBuffer commandBuffer);
	// Called when acquire fails before this ImGui frame can be rendered.
	void discardFrame();

private:
	void renderPerformancePanel(float dt, const RenderTelemetry& telemetry, EngineSettings& settings);
	void renderSSAOPanel(EngineSettings& settings);
	void renderLightingPanel(EngineSettings& settings);
	void renderBloomPanel(EngineSettings& settings);
	void renderMotionBlurPanel(EngineSettings& settings);
	void renderConfigPanel(FrameInfo& frameInfo);
	void renderCameraPanel(TransformComponent& cameraTransform, class CameraController* controller);

	Device& vulkanDevice;
	VkDescriptorPool imguiPool;
	// ImGui 后端会保存这个指针，所以必须由我们持有到 shutdown
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
};
