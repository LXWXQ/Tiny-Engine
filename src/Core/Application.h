#pragma once
#include "RHI/Device.h"
#include "RHI/Swapchain.h"
#include "RHI/Renderer.h"
#include "RHI/GBuffer.h"
#include "RHI/Texture.h"
#include "Core/DescriptorManager.h"
#include "Core/ImGuiSystem.h"
#include "Core/EngineConfig.h"
#include "Core/JobSystem.h"
#include "Render/RenderPipeline.h"
#include "Render/IBLBaker.h"
#include "Render/RenderResources.h"
#include "RHI/RenderGraph.h"
#include "RHI/GpuTimer.h"
#include "RHI/ScreenshotCapture.h"
#include "RHI/RenderDocHook.h"
#include "RHI/GpuProfiler.h"
#include "Scene/Scene.h"
#include "Editor/EditorLayer.h"
#include <memory>
#include <chrono>
#include <array>

class ComputeProbePass;
class CrossQueueProbePass;
class PresentPass;
class UiCompositePass;
class EditorOverlayPass;
class SSAOPass;
class ShadowPass;
class TAAPass;
class SubresourceProbePass;
class GeometryPass;
class ReflectionPass;

class FirstApp
{
public:
	explicit FirstApp(const diag::Options& options);
	~FirstApp();

	FirstApp(const FirstApp&) = delete;
	FirstApp& operator=(const FirstApp&) = delete;

	void run();

	// ---- 无人值守自检（D-2 新增）----
	// 为什么不只在 UI 上勾选：ImGui 的复选框没法在无人值守的验证流程里点。
	// 但"关掉某个 Pass"恰恰是最容易出验证层错误的分支 —— 中间目标整帧
	// 没人写（布局一直停在 UNDEFINED），后面的 Pass 却仍然声明要采样它。
	// 所以把开关和固定帧数的跑法暴露出来，由命令行参数驱动，
	// 这样这些分支也能进"零验证层输出"的回归检查。
	void setBloomEnabled(bool enabled);
	void setSSAOEnabled(bool enabled);
	void setGpuSceneEnabled(bool enabled);
	void setGpuSceneForced(bool forced);
	void setGpuMergedBuffers(bool enabled);
	void setHiZEnabled(bool enabled);
	// F6：关掉级联阴影（排查 / 对比截图用）。
	void setShadowsEnabled(bool enabled);
	// TAA：无人值守自检里对比开关效果。
	void setTAAEnabled(bool enabled);
	void setWaterEnabled(bool enabled);
	void setTerrainBatchEnabled(bool enabled);
	void setGpuSceneMinScreenPixels(float pixels);
	void setGpuSceneExternalDebugCount(uint32_t count);
	void setStreamingWorldEnabled(bool enabled, int32_t radius);
	void setStreamingFly(bool enabled, float speed);

	void setStreamingFlyRadius(float radius);
	void setStreamingBudget(float milliseconds);
	// --update-profile：把分阶段计时开关转给 Scene。
	void setUpdateProfiling(bool enabled);
	void setOriginRebaseEnabled(bool enabled);
	void setOriginRebaseThreshold(float threshold);
	void setCrossQueueTestEnabled(bool enabled);
	void setResizeTestEnabled(bool enabled);
	void setMaterialStressEnabled(bool enabled);
	void setSubresourceTestEnabled(bool enabled);
	void setComputeProbeEnabled(bool enabled);
	int  runSubresourceErrorSelfTest();
	int  runDeviceFaultSelfTest();
	// TD-03 dynamic-resolution interface: change intermediate render targets
	// without recreating pipelines or the swapchain.
	void setRenderScale(float scale);
	// UX-19：按显示器工作区收敛窗口尺寸。工作区是逻辑像素（150% 缩放时
	// 1920x1080 的屏幕在 GLFW 里就是 1280x720），所以固定用 1920x1080 会把
	// 窗口撑出屏幕，右下角的面板和状态栏永远点不到。
	static void computeWindowSize(int workWidth, int workHeight, int& width, int& height);
	int  runHeadlessSelfTest(int frames);


	// 场景编辑器（--editor）。默认关闭；关闭时引擎行为与改造前完全一致。
	void setEditorEnabled(bool enabled) { editorRequested = enabled; }
	bool isEditorActive() const { return editorRequested; }

	// W1：配置存取。ImGui 面板上的 Save/Load 按钮走这里 ——
	// 让"面板改的参数"和"启动时读的参数"共用同一条代码路径，
	// 避免出现"UI 保存的格式和启动读的格式不一致"这种低级但难查的问题。
	void saveConfig();
	void loadConfig();

private:
	void initWindow(const std::string& title);
	void loadAllPBRTextures();
	void createUniformBuffers();
	void initMaterials();

	void update(float dt);
	void render(float dt);
	bool handleWindowMaintenance();
	void recreateSwapchain(VkExtent2D framebufferExtent);
	void recreateRenderTargets(VkExtent2D renderExtent);
	void rebuildGlobalDescriptors();
	VkExtent2D getDesiredRenderExtent() const;
	void materialStressStep();
	void finalizeDiagnostics();

	GLFWwindow* window = nullptr;
	std::unique_ptr<Device> vulkanDevice;
	std::unique_ptr<Swapchain> vulkanSwapchain;
	std::unique_ptr<Renderer> renderer;
	std::array<std::unique_ptr<GBuffer>, kFramesInFlight> gBuffers;
	std::unique_ptr<DescriptorManager> descriptorManager;
	std::unique_ptr<Scene> scene;
	std::unique_ptr<JobSystem> jobSystem;
	std::unique_ptr<ImGuiSystem> imguiSystem;
	std::unique_ptr<IBLBaker> iblBaker;
	ComputeProbePass* computeProbePassPtr = nullptr;
	CrossQueueProbePass* crossQueueProbePassPtr = nullptr;
	SubresourceProbePass* subresourceProbePassPtr = nullptr;
	PresentPass* presentPassPtr = nullptr;
	UiCompositePass* uiCompositePassPtr = nullptr;
	EditorOverlayPass* editorOverlayPassPtr = nullptr;
	bool editorOverlayInitialized = false;
	SSAOPass* ssaoPassPtr = nullptr;
	// F6：级联阴影。指针不拥有对象（unique_ptr 在 init 里，随渲染管线走）。
	ShadowPass* shadowPassPtr = nullptr;
	// TAA：Glass 之后、Bloom 之前，负责抖动重建与历史混合。
	TAAPass* taaPassPtr = nullptr;
	// C1：半分辨率平面反射（Lighting 之后、Glass/Water 之前）。
	ReflectionPass* reflectionPassPtr = nullptr;
	GeometryPass* geometryPassPtr = nullptr;
	bool reflectionDescriptorsEnabled = false;


	RenderPipeline renderPipeline;

	// D-1(b)：渲染图 + 帧内资源登记表
	std::unique_ptr<RenderGraph> renderGraph;

	// P0：诊断基础设施（随用随开）。
	diag::Options diagnosticsOptions;
	GpuTimer gpuTimer;
	RenderDocHook renderDoc;
	GpuProfiler gpuProfiler;
	ScreenshotCapture screenshotCapture;
	bool screenshotAutoTarget = false;
	bool diagnosticsFinalized = false;
	std::unique_ptr<RenderResources> renderResources;

	std::array<VkBuffer, kFramesInFlight> globalUboBuffers{};
	std::array<VmaAllocation, kFramesInFlight> globalUboAllocations{};
	std::array<void*, kFramesInFlight> uboMapped{};

	std::unique_ptr<Texture> environmentTex;

	// Last successfully rendered camera view-projection. PresentPass uses it
	// to compute screen-space camera motion vectors.
	glm::mat4 previousViewProjection{ 1.0f };
	bool hasPreviousViewProjection = false;

	std::chrono::high_resolution_clock::time_point currentTime;
	std::chrono::high_resolution_clock::time_point lastPerfLog;
	uint32_t framesSincePerfLog = 0;
	double updateMsAccum = 0.0;
	double renderMsAccum = 0.0;
	uint32_t profiledFrames = 0;

	// 自检会故意把 bloom/ssao 关掉，如果照常自动保存，一次回归跑下来
	// 就把用户调好的配置改成了"全关"。所以自检模式下禁掉退出自动保存。
	// ---- 场景编辑器 ----
	std::unique_ptr<editor::EditorLayer> editorLayer;
	bool editorRequested = false;
	void initEditor();
	void shutdownEditor();
	// TD-09：编辑器离屏视口的资源生命周期。
	// 由编辑器报尺寸、引擎在帧首统一应用 —— 不能在 UI 构建期重建纹理，
	// 那时命令缓冲已经在录制，vkDeviceWaitIdle 会直接死锁。
	void createEditorViewportTextures(VkExtent2D extent);
	void destroyEditorViewportTextures();
	void removeEditorViewportImGuiTextures();

	VkSampler editorViewportSampler = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, kFramesInFlight> editorViewportImGuiSets{};
	bool editorViewportTexturesCreated = false;

	// 编辑器请求的视口尺寸。这个值**粘住**不清零：编辑器只在尺寸变化时才上报，
	// 引擎靠"它连续若干帧没再变"判断拖拽已经停下。
	VkExtent2D pendingEditorViewportExtent{ 0, 0 };
	// 上一次上报发生在第几帧。用于视口尺寸防抖（见 update() 里的说明）。
	uint64_t viewportRequestFrame = 0;
	// 每帧递增的计数器，仅用于视口尺寸防抖。
	uint64_t viewportDebounceClock = 0;

	bool suppressAutoSave = false;
	// F2-a：流送世界。streamingWorldEntity 是 TerrainStreamingComponent 的持有者，
	// 换成坐标后 Scene::refreshStreamingTerrain 会按它增删 chunk 实体。
	bool streamingWorldEnabled = false;
	int32_t streamingWorldRadius = 3;
	bool streamingFlyEnabled = false;
	float streamingFlySpeed = 12.0f;
	float streamingFlyAngle = 0.0f;
	float streamingFlyRadius = 220.0f;
	double streamingFlyTravel = 0.0;
	entt::entity streamingWorldEntity = entt::null;
	// F3：体素世界管理器实体（--voxel-world）。
	entt::entity voxelWorldEntity = entt::null;
	bool resizeTestEnabled = false;
	bool materialStressEnabled = false;
	uint32_t materialStressPendingSlot = kInvalidMaterialSlot;
	uint32_t materialStressCycles = 0;
	uint32_t materialStressBaselineUsed = 0;
	uint32_t materialStressMaxPending = 0;
	VkExtent2D currentRenderExtent{};
};