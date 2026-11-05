#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include "Core/FrameInFlight.h"
#include "RHI/Device.h"
#include "RHI/GBuffer.h"
#include "Core/MaterialSlot.h"

#include <glm/glm.hpp>

class Material;

// E3: glTF 的 baseColor / metallic / roughness factor。
//
// bindless 材质 set（set 1）只有 4 个纹理数组，factor 没地方放，而 shader 之前
// 压根没读它们 —— 编辑器里的滑杆就是个安慰剂。这里用一个 32 字节一项的 SSBO
// 挂到全局 set（set 0, binding 10），按材质槽位索引。
// GLSL 侧是 std430 的两个 vec4，所以 C++ 必须也是两个 vec4（stride 32）。
struct MaterialFactorRecord
{
    glm::vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 metalRough{ 1.0f, 1.0f, 0.0f, 0.0f };
};

class DescriptorManager
{
public:
	void init(Device& device, uint32_t maxMaterials = 256);
	void cleanup();

	VkDescriptorSetLayout getGlobalSetLayout() const { return globalSetLayout; }
	VkDescriptorSetLayout getMaterialSetLayout() const { return materialSetLayout; }
	// Terrain stage 6: fixed 12-sampler PBR texture set (set 2).
	// B-08: four bounded material sets; the renderer binds one per terrain batch.
	static constexpr uint32_t kTerrainSetCount = 4;
	VkDescriptorSetLayout getTerrainSetLayout() const { return terrainSetLayout; }
	VkDescriptorSet getTerrainSet(uint32_t index = 0) const
	{
		return index < kTerrainSetCount ? terrainSets[index] : terrainSets[0];
	}

	VkDescriptorSetLayout createSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings);

	// 全局描述符集（D-4 起每个 frame slot 一份，但 layout 相同）：
	//  0=UBO  1=equirect 环境贴图(天空盒用)  2~5=G-Buffer  6=SSAO
	//  7=irradiance cubemap  8=prefiltered cubemap  9=BRDF LUT
	// 注意：D-2 之后光照通道的输出（HDR 中间目标）**不在**这里 ——
	// 它只在后效链内部流转，用各 Pass 自己的局部 set，不占用全局集。
	// D-4：每个 frame slot 一份全局描述符集，各自指向该帧专属的
	// UBO / G-Buffer / SSAO 图像。环境贴图与 IBL 是跨帧只读资源，共享同一组。
	void buildGlobalSet(const std::array<VkBuffer, kFramesInFlight>& uboBuffers,
		const VkDescriptorImageInfo& envInfo,
		const std::array<const GBuffer*, kFramesInFlight>& gBuffers,
		VkSampler ssaoSampler, const std::array<VkImageView, kFramesInFlight>& ssaoViews,
		const VkDescriptorImageInfo& irradianceInfo,
		const VkDescriptorImageInfo& prefilteredInfo,
		const VkDescriptorImageInfo& brdfLutInfo,
		// F6：3 级联阴影图（sampler2DArray，每帧槽位一份）。没有阴影时传空句柄，
		// 着色器那边用 shadowParams.w 关掉采样，不会读到未定义内容。
		VkSampler shadowSampler, const std::array<VkImageView, kFramesInFlight>& shadowViews,
		// C1：binding 12 = 平面反射颜色（水面片元用它替换环境贴图反射）。
		// 关闭平面反射时传环境贴图作为占位，水面着色器不会采样。
		VkSampler reflectionSampler, const std::array<VkImageView, kFramesInFlight>& reflectionViews);
	VkDescriptorSet getGlobalSet(uint32_t frameInFlightIndex) const
	{
		return globalSets[frameInFlightIndex];
	}

	// ---------------- 材质（D-3 起是 bindless 的）----------------
	//
	// 材质不再是"一个材质一个 VkDescriptorSet"，而是**一个 set 里放 4 个纹理数组**：
	//
	//     set 1, binding 0: sampler2D textures[capacity]   // albedo / normal /
	//     set 1, binding 1: sampler2D textures[capacity]   // metallicRoughness / ...
	//
	// 等等 —— 实际是 4 个独立数组，各自按同一个槽位下标索引：
	//     binding 0 = albedo 数组，binding 1 = normal，binding 2 = metallic，3 = roughness
	// 着色器用材质下标去采：  texture(albedoMaps[matId], uv)
	//
	// 为什么不用"每个材质一个 set"：那样每换一次材质就要 vkCmdBindDescriptorSets，
	// 场景里 104 个子网格 = 104 次绑定。bindless 之后**整帧只绑一次 set 1**，
	// 切材质只是改一个 push constant 里的整数。
	//
	// 槽位管理（分配/归还）保持不变 —— 仍然是"一次性预分配 + 空闲栈"，
	// 只是现在改的是数组里某个下标的内容，而不是某个独立 set 的内容。
	uint32_t allocateMaterialSet(const VkDescriptorImageInfo& albedo,
		const VkDescriptorImageInfo& normal,
		const VkDescriptorImageInfo& metallic,
		const VkDescriptorImageInfo& roughness);
	uint32_t allocateMaterialSet(const Material& material);

	// TD-04: defer slot reuse until the graphics timeline proves GPU use is done.
	void setFrameTimelineCallbacks(std::function<uint64_t()> completed,
		std::function<void(uint64_t)> waitUntil,
		std::function<uint64_t()> lastSubmitted);
	void collectMaterialGarbage();
	uint32_t getPendingMaterialReleaseCount() const { return static_cast<uint32_t>(deferredMaterialReleases.size()); }

	// 归还槽位。越界或重复归还都会被安全忽略。不会阻塞设备。
	// 槽位先进入 deferredMaterialReleases，等 graphics timeline 确认相关提交退休。
	void releaseMaterialSet(uint32_t index);

	// bindless 之后材质只有一个 set（不再是每个材质一个）。
	// 参数已经被忽略，保留这个重载只是因为调用点还写着 getMaterialSet(slot) ——
	// 想识别"哪些地方还在按材质逐个绑定"时，它是个好线索（现在是 0 个）。
	VkDescriptorSet getMaterialSet() const { return materialSet; }
	VkDescriptorSet getMaterialSet(uint32_t /*index*/) const { return materialSet; }

	void updateTerrainTextures(
		uint32_t setIndex,
		const std::array<VkDescriptorImageInfo, 4>& albedoInfos,
		const std::array<VkDescriptorImageInfo, 4>& normalInfos,
		const std::array<VkDescriptorImageInfo, 4>& heightRoughnessInfos);

	// 槽位是否真的被绑定过内容。数组尾部允许部分未绑定
	//（descriptorBindingPartiallyBound），着色器那边靠 materialSet.alpha
	// 的守卫跳过——这里只用于 CPU 侧自检。
	bool isMaterialSlotWritten(uint32_t index) const
	{
		return index < slotInUse.size() && slotInUse[index];
	}

	uint32_t getMaterialSlotCapacity() const { return static_cast<uint32_t>(slotInUse.size()); }
	uint32_t getMaterialSlotUsed() const
	{
		return getMaterialSlotCapacity() - static_cast<uint32_t>(freeSlots.size());
	}

	VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);
	void writeSet(VkDescriptorSet set, const std::vector<VkWriteDescriptorSet>& writes);

	VkDescriptorPool getPool() const { return pool; }

	// C1：平面反射的全局 set 也要绑定同一张材质 factor 表。
	VkBuffer getMaterialFactorBuffer() const { return materialFactorBuffer; }

	// E3: 写某个材质槽位的 baseColor / metallic / roughness factor。
	// 越界或缓冲未建时静默返回（自检/无设备路径也会调到这里）。
	void updateMaterialFactors(uint32_t slot, const glm::vec4& baseColor, float metallic,
		float roughness, float shaderFlags = 0.0f);

	// E6: 材质贴图被替换后刷新该槽位的四个纹理数组项。
	// 编辑器要能"换贴图并立刻看到效果"，但 writeMaterialSet 是内部细节，
	// 所以这里只暴露"按材质对象刷新"这一个动作。
	void refreshMaterialTextures(uint32_t slot, const Material& material);

private:
	void createGlobalLayout();
	void createMaterialLayout(uint32_t capacity);
	void createTerrainLayout();
	void writeMaterialSet(uint32_t slot,
		const VkDescriptorImageInfo& albedo,
		const VkDescriptorImageInfo& normal,
		const VkDescriptorImageInfo& metallic,
		const VkDescriptorImageInfo& roughness);

	void reclaimReleasedMaterialSlots();

	struct DeferredMaterialRelease
	{
		uint32_t slot = 0;
		uint64_t safeAfterFrame = 0;
	};

	Device* device = nullptr;
	VkDescriptorPool pool = VK_NULL_HANDLE;

	VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout terrainSetLayout = VK_NULL_HANDLE;

	std::vector<VkDescriptorSet> globalSets;

	// D-3：材质只剩这一个 set，里面是 4 个纹理数组。
	// 数组长度（材质容量）在分配时通过 variable descriptor count 指定，
	// 所以布局里不写死 —— 想调容量只需改 init 的 maxMaterials。
	VkDescriptorSet materialSet = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, kTerrainSetCount> terrainSets{};
	uint32_t materialCapacity = 0;

	std::vector<bool> slotInUse;                 // 槽位占用标记（含等待回收）
	std::vector<bool> slotPendingRelease;        // 已归还但还没到安全帧
	std::vector<uint32_t> freeSlots;             // 空闲槽位栈，后进先出 -> O(1) 分配/归还
	std::deque<DeferredMaterialRelease> deferredMaterialReleases;

	std::function<uint64_t()> completedFrameQuery;
	std::function<void(uint64_t)> frameWait;
	std::function<uint64_t()> lastSubmittedFrameQuery;
	// E3: 材质 factor 表（见 MaterialFactorRecord）。常驻映射，按槽位索引。
	VkBuffer materialFactorBuffer = VK_NULL_HANDLE;
	VmaAllocation materialFactorAllocation = nullptr;
	void* materialFactorMapped = nullptr;
	uint32_t materialFactorCount = 0;

};
