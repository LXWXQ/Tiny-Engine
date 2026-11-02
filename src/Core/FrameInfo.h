#pragma once
#include <vulkan/vulkan.h>
#include "Scene/Components.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <cstddef>   // offsetof
#include <string>    // EngineSettings::windowTitle
#include <functional>  // FrameInfo 的配置回调
#include "Core/FrameInFlight.h"

#define MAX_POINT_LIGHTS 100
#define WIDTH 1920
#define HEIGHT 1080

class DescriptorManager;
class CameraController;

struct PointLight
{
	glm::vec4 position{};
	glm::vec4 color{};
};

struct GlobalUbo
{
	glm::mat4 projectionView{ 1.f };

	alignas(16) glm::vec4 ambientLightColor{ 1.f, 1.f, 1.f, .05f };
	alignas(16) glm::vec4 lightDirection{ 1.f, 1.f, 1.f, 0.f };
	alignas(16) glm::vec4 lightColor{ 1.f, 1.f, 1.f, 1.f };
	alignas(16) glm::vec4 cameraPos{ 0.f };

	alignas(16) int numLights = 0;
	float exposure = 1.0f;   // D-2 起光照着色器不再读它，保留字段是为了不动 UBO 布局
	alignas(16) PointLight pointLights[MAX_POINT_LIGHTS];

	// Previous rendered frame's projection * view. Present uses it to
	// reproject each pixel's world position into screen space and derive a
	// camera motion vector for motion blur.
	glm::mat4 previousProjectionView{ 1.f };
};

// ---------------------------------------------------------------------------
// 编译期校验 C++ 结构与 GLSL std140 布局严格一致。
// 偏移值必须和 shaders/*.vert|frag 中 GlobalUbo 的声明一一对应；
// 一旦有人调整成员顺序或类型，这里会直接编译失败，而不是等到运行时画面错乱。
// ---------------------------------------------------------------------------
static_assert(offsetof(GlobalUbo, projectionView)    == 0,   "GlobalUbo.projectionView 偏移错误");
static_assert(offsetof(GlobalUbo, ambientLightColor) == 64,  "GlobalUbo.ambientLightColor 偏移错误");
static_assert(offsetof(GlobalUbo, lightDirection)    == 80,  "GlobalUbo.lightDirection 偏移错误");
static_assert(offsetof(GlobalUbo, lightColor)        == 96,  "GlobalUbo.lightColor 偏移错误");
static_assert(offsetof(GlobalUbo, cameraPos)         == 112, "GlobalUbo.cameraPos 偏移错误");
static_assert(offsetof(GlobalUbo, numLights)         == 128, "GlobalUbo.numLights 偏移错误");
static_assert(offsetof(GlobalUbo, exposure)          == 132, "GlobalUbo.exposure 偏移错误");
static_assert(offsetof(GlobalUbo, pointLights)       == 144, "GlobalUbo.pointLights 偏移错误");
static_assert(sizeof(PointLight) == 32, "PointLight 应等于两个 vec4 的大小");
static_assert(offsetof(GlobalUbo, previousProjectionView) == 144 + MAX_POINT_LIGHTS * sizeof(PointLight),
	"GlobalUbo.previousProjectionView offset mismatch");
static_assert(sizeof(GlobalUbo) == 144 + MAX_POINT_LIGHTS * sizeof(PointLight) + sizeof(glm::mat4),
	"GlobalUbo size mismatch");

struct SimplePushConstantData
{
	glm::mat4 modelMatrix{ 1.f };
	glm::mat4 normalMatrix{ 1.f };

	// D-3 bindless：材质下标 + 是否有效，替代原来"每换材质重绑一次描述符集"。
	//
	// 用两个 uint32 而不是一个（+padding），是为了让内存布局和 GLSL 里的
	// `uvec2 material` 精确对上 —— std430 下 uvec2 占 8 字节、对齐 8，
	// 而 C++ 的 [mat4][mat4][uvec2] 恰好也是 128 + 8 = 136，两边一致。
	// 用 uint 而不是 int：着色器里直接拿去索引数组，省一次转换。
	uint32_t materialId = 0;      // 材质槽位下标（kInvalidMaterialSlot 时取 0）
	uint32_t materialValid = 0;   // 0 = 天空盒/网格这类不需要材质的物体
	float alphaCutoff = 0.5f;
	uint32_t alphaMasked = 0;

	// 1 = 材质没有 albedo 贴图，着色器改画程序化棋盘格。
	// 为什么需要它：编辑器里新建的实体用的是占位材质（没有任何贴图），
	// 采样默认白纹理会渲染成一团纯白，看不出形状与朝向。
	uint32_t albedoFallback = 0;

// 棋盘格密度：把局部坐标乘以它，使"最大尺寸上正好 N 格"。
// 必须由 CPU 按模型包围盒算出来 —— 不同模型的局部尺寸能差上百倍，
// 写死一个常数会导致大模型糊成一片灰、小模型只有一格。
float checkerScale = 1.0f;

	// 有 UV 时的 UV 归一化：p = (uv - zw) * xy。
	// 不同模型的 UV 可能只占 [0,1] 的一小块（Quad-Sphere 是 0.05~0.43），
	// 不归一化的话 8 格棋盘在球上只会显示两三个大格子。
	//
	// alignas(16)：GLSL std430 的 vec4 必须 16 字节对齐；而 C++ 的 glm::vec4
	// 默认只有 4 字节对齐。不加这个会悄悄错开 8 字节，材质字段全乱。
	alignas(16) glm::vec4 checkerUvTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

	// 1 = 顶点色程序化地形。该分支在 G-Buffer 法线写入之后、材质采样之前生效。
	uint32_t terrainMode = 0;

	// Stage 6 terrain material parameters fill the 32-byte aligned tail.
	float terrainMinY = 0.0f;
	float terrainHeightRange = 1.0f;
	float terrainDetailStrength = 0.65f;
	float terrainUvTiling = 1.0f;
	uint32_t terrainMaterialStyle = 0;
	// Dither-based LOD transition: blend is incoming coverage. Mode 0 = off,
	// 1 = outgoing LOD, 2 = incoming LOD.
	float terrainLodBlend = 1.0f;
	uint32_t terrainLodBlendMode = 0;

	// F1：非 0 表示这帧的几何由 SceneGpuBatch 的间接命令提交，
	// 顶点/片元着色器据此改用实例缓冲里的矩阵与材质参数。
	uint32_t gpuScene = 0;

	// 偏移必须在编译期钉死，否则"某个字段悄悄移位"会导致材质错乱，
	// 而且这类错乱看起来像"贴图加载错了"，极难往回追。
	static constexpr size_t kMaterialOffset = 2 * sizeof(glm::mat4);
};
static_assert(offsetof(SimplePushConstantData, materialId)    == SimplePushConstantData::kMaterialOffset,     "materialId 偏移必须紧跟两个 mat4");
static_assert(offsetof(SimplePushConstantData, materialValid) == SimplePushConstantData::kMaterialOffset + 4, "materialValid 必须紧跟 materialId");
static_assert(offsetof(SimplePushConstantData, alphaCutoff) == SimplePushConstantData::kMaterialOffset + 8,
	"alphaCutoff must follow the material uvec2");
static_assert(offsetof(SimplePushConstantData, alphaMasked) == SimplePushConstantData::kMaterialOffset + 12,
	"alphaMasked must follow alphaCutoff");
static_assert(offsetof(SimplePushConstantData, albedoFallback) == SimplePushConstantData::kMaterialOffset + 16,
	"albedoFallback must follow alphaMasked");
static_assert(offsetof(SimplePushConstantData, checkerScale) == SimplePushConstantData::kMaterialOffset + 20,
"checkerScale must follow albedoFallback");
static_assert(offsetof(SimplePushConstantData, checkerUvTransform) == SimplePushConstantData::kMaterialOffset + 32,
	"checkerUvTransform must be 16-byte aligned after checkerScale");
static_assert(offsetof(SimplePushConstantData, terrainMode) == SimplePushConstantData::kMaterialOffset + 48,
"terrainMode must be at the end of the material push block");
static_assert(offsetof(SimplePushConstantData, terrainMinY) == SimplePushConstantData::kMaterialOffset + 52,
"terrainMinY must follow terrainMode");
static_assert(offsetof(SimplePushConstantData, terrainHeightRange) == SimplePushConstantData::kMaterialOffset + 56,
"terrainHeightRange must follow terrainMinY");
static_assert(offsetof(SimplePushConstantData, terrainDetailStrength) == SimplePushConstantData::kMaterialOffset + 60,
"terrainDetailStrength must follow terrainHeightRange");
static_assert(offsetof(SimplePushConstantData, terrainUvTiling) == SimplePushConstantData::kMaterialOffset + 64,
"terrainUvTiling must follow terrainDetailStrength");
static_assert(offsetof(SimplePushConstantData, terrainMaterialStyle) == SimplePushConstantData::kMaterialOffset + 68,
"terrainMaterialStyle must follow terrainUvTiling");
static_assert(offsetof(SimplePushConstantData, terrainLodBlend) == SimplePushConstantData::kMaterialOffset + 72,
"terrainLodBlend must follow terrainMaterialStyle");
static_assert(offsetof(SimplePushConstantData, terrainLodBlendMode) == SimplePushConstantData::kMaterialOffset + 76,
"terrainLodBlendMode must follow terrainLodBlend");
static_assert(offsetof(SimplePushConstantData, gpuScene) == SimplePushConstantData::kMaterialOffset + 80,
	"gpuScene must follow terrainLodBlendMode");
// 结构体按 glm::mat4 的 16 字节对齐补齐：GLSL 侧块是 212 字节，C++ 侧 sizeof 是 224。
// push range 用 sizeof 是合法的，多出的 12 字节着色器不读。
static_assert(sizeof(SimplePushConstantData) == SimplePushConstantData::kMaterialOffset + 96,
	"push constant size must match GLSL layout (with padding)");

struct EngineSettings
{
	// W1：窗口标题进了配置。把它放在这里是刻意的 —— 调试时经常要按场景/质量档
	// 起好几个实例同时看，标题是区分它们唯一的可见标识，而它又必须能改。
	std::string windowTitle = "Tiny Engine";

	// F1：GPU 驱动提交（实例缓冲 + compute 剔除 + indirect draw）。
	// 关掉退回 CPU 逐实体逐子网格的旧路径，用来对比与排查。
	bool gpuSceneEnabled = true;
	// 实例数低于该阈值时不走 GPU 批：几十个间接绘制比直接绘制还慢。
	uint32_t gpuSceneMinInstances = 256;
	// 调试/验证用：忽略阈值，强制走 GPU 路径（缩略场景也能覆盖这条路径）。
	bool gpuSceneForce = false;
	// F1-b：把可批模型合并进一份全局顶点/索引缓冲，一次 multidraw 提交。
	// 关掉退回 F1-a 的「每模型一次 bind + 一组建 indirect 命令」。
	bool gpuMergedBuffers = true;

	bool ssaoEnabled = true;
	float ssaoRadius = 0.5f;
	float ssaoBias = 0.025f;
	int ssaoKernelSize = 16;

	bool ssrEnabled = false;
	bool shadowEnabled = false;
	// B-03: compare GPU-selected LOD/visibility against a CPU mirror. Enabled
	// only by --terrain-test; production frames skip the mirror entirely.
	bool terrainGpuSelectionSelfTest = false;

	glm::vec4 directionalLightDir{ -1.0f, -1.0f, 1.0f, 0.0f };
	glm::vec4 directionalLightColor{ 1.0f, 1.0f, 1.0f, 1.0f };

	// 曝光。1.0 在这里是"物理正确"的值：
	// split-sum IBL 的能量已经归一化（irradiance 存的是 E/PI，
	// specular 走 prefiltered x BRDF-LUT），所以 1.0 直接给到合理画面。
	// 实测：老 IBL 在 1.0 下 95.9% 像素过曝，换成 split-sum 后降到 7.1%。
	// D-2 起曝光在 PresentPass 里生效（在 Bloom 之后、tonemap 之前）——
	// 放在光照通道里会让 Bloom 的阈值随曝光一起变，调曝光时辉光强度会乱跳。
	float exposure = 1.0f;

	// TD-03：动态分辨率。只改变 G-Buffer / SSAO / HDR / Bloom 中间目标，
	// 交换链和管线保持不变；Present 负责把低分辨率 HDR 上采样到窗口。
	float renderScale = 1.0f;

	// ---- Bloom（D-2）----
	// 阈值作用在线性 HDR 上，1.0 ≈ 刚好"纯白"的亮度。
	//
	// 这几个默认值是实测调出来的（同一机位截屏 + 逐像素对照，见 D-2 提交说明）：
	//   阈值 1.0 / 强度 0.04：变化超过 8 的像素只有 17%，肉眼基本看不出辉光；
	//   阈值 1.0 / 强度 0.25：整片天空都在阈值之上，画面像蒙了一层灰白雾；
	//   阈值 1.8 / 强度 0.30：终于能看出辉光，但把天窗那块过曝墙面整片糊白了；
	//   阈值 0.2 / 强度 0.8 ：极端值（60.9% 像素过曝）—— 只用来自检链路是否接通。
	// 最终 2.2 / 0.5 / 0.12：辉光收敛成"围绕强光的一圈柔和外溢"，
	// 全屏均值与基线只差 1.2（183.5 -> 182.3），画面结构完全保留。
	bool  bloomEnabled = true;
	float bloomThreshold = 2.2f;
	float bloomKnee = 0.5f;      // 软膝盖宽度，避免阈值边界出现硬边
	float bloomIntensity = 0.12f;

	// Camera motion blur. Screen-space velocity comes from reprojecting the
	// current world position with the previous frame's view-projection matrix.
	bool  motionBlurEnabled = true;
	float motionBlurStrength = 1.0f;
	float motionBlurMaxRadius = 0.035f;
};

struct RenderTelemetry
{
	uint32_t drawCalls = 0;
	uint32_t triangles = 0;
	uint32_t vertices = 0;
	uint32_t subMeshesCulled = 0;
	uint32_t lodSubMeshesSkipped = 0;
	uint32_t terrainLod0Chunks = 0;
	uint32_t terrainLod1Chunks = 0;
	uint32_t terrainLod2Chunks = 0;
	uint32_t terrainLodBlends = 0;
	uint32_t terrainLodSwitches = 0;
	uint32_t terrainIndirectGroups = 0;
	uint32_t terrainMaterialBatches = 0;
	uint32_t terrainMaterialSwitches = 0;
	uint32_t terrainMaterialFallbacks = 0;

	// D-3：描述符集绑定次数。
	// 单列一个计数器是因为 bindless 的收益**只体现在这个数字上** ——
	// draw call 数不会变（还是 104），变的是一次绑定管多少绘制。
	// 没有它，就没法证明"整帧只绑一次"这件事真的生效了。
	uint32_t descriptorBinds = 0;

	void reset()
	{
		drawCalls = 0;
		triangles = 0;
		vertices = 0;
		subMeshesCulled = 0;
		lodSubMeshesSkipped = 0;
		terrainLod0Chunks = 0;
		terrainLod1Chunks = 0;
		terrainLod2Chunks = 0;
		terrainLodBlends = 0;
		terrainLodSwitches = 0;
		terrainIndirectGroups = 0;
		terrainMaterialBatches = 0;
		terrainMaterialSwitches = 0;
		terrainMaterialFallbacks = 0;
		descriptorBinds = 0;
	}
};

struct FrameInfo
{
// 每个在飞帧槽位对应的索引（0..kFramesInFlight-1）。
// 用来选择该帧专属的 UBO、全局描述符集和瞬时渲染目标。
uint32_t frameInFlightIndex;

// 本次 acquire 到的交换链图像索引。它与 frameInFlightIndex 语义完全不同：
// 交换链图像可能每帧变化，而 frame slot 是固定循环复用的。
uint32_t swapchainImageIndex;

// 从启动开始的逻辑帧号，用于日志 / 调试与 timeline semaphore 对照。
uint64_t frameNumber;

float frameTime;
VkCommandBuffer commandBuffer;

	EngineSettings& settings;
	RenderTelemetry& telemetry;
	// ImGui is built before the render graph executes, so performance counters
	// shown in the UI come from the last fully recorded frame.
	const RenderTelemetry& displayTelemetry;
	entt::registry& registry;
	const std::vector<entt::entity>& visibleEntities;

	glm::mat4 projectionMatrix;
	glm::mat4 viewMatrix;

	VkBuffer globalUboBuffer;
	DescriptorManager* descriptorManager = nullptr;
	TransformComponent* cameraTransform = nullptr;
	CameraController* cameraController = nullptr;

	// W1：配置存取的入口。
	// 用回调而不是让 ImGuiSystem 直接持有 FirstApp，是为了不让 UI 层依赖引擎总控 ——
	// 数据总线（FrameInfo）本来就承担"每帧把东西递给 Pass/UI"的职责，这里沿用同一套。
	// 为空表示"当前上下文不支持配置操作"（例如还没接上）。
	std::function<void()> onSaveConfig;
	std::function<void()> onLoadConfig;

	// 编辑器接管 UI 时的绘制回调。
	// 为空 -> ImGuiSystem 画内置面板（运行时模式）；
	// 非空 -> 由编辑器画自己的菜单栏与面板，内置面板不再绘制。
	// 用回调而不是让 ImGuiSystem 认识 EditorLayer：保持 "Core 不依赖 Editor" 的边界。
	std::function<void()> onBuildEditorUI;
	std::function<bool()> isEditorMode;

	// 编辑器当前选中的实体（entt::null = 没选）。渲染侧用它画"选中包围盒"。
	// 【必须放在结构体末尾】FrameInfo 是聚合初始化（位置对应），
	// 插在中间会让后面所有字段整体错位，编译期会报一堆莫名其妙的类型转换错误。
	entt::entity editorSelection = entt::null;
};
