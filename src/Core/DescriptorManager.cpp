#include "DescriptorManager.h"
#include "Material.h"
#include "FrameInfo.h"
#include <stdexcept>
#include <iostream>
#include <string>
#include <utility>

void DescriptorManager::init(Device& dev, uint32_t maxMaterials)
{
	device = &dev;
	materialCapacity = maxMaterials;

	// 池容量按 maxMaterials 推算，不再硬编码 —— 否则把 maxMaterials 调大时，
	// 池会在 vkCreateDescriptorPool 阶段直接失败。
	// 除材质外还要容纳（D-4 起全部 × kFramesInFlight）：全局 set、SSAO local、
	// Bloom 7 个（1 亮部提取 + 3 降采样 + 3 上采样）、Present —— 后效链
	// 每个 Pass 都有自己的 set，所以这里的余量必须跟着帧数和 Pass 数一起涨。
	//
	// D-3 变化：材质从"每材质 4 个独立 descriptor"变成"4 个数组各 maxMaterials 项"，
	// 描述符总数不变（还是 maxMaterials * 4），但 **set 数从 maxMaterials 降到 1**。
	std::vector<VkDescriptorPoolSize> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         maxMaterials + 16 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxMaterials * 4 + 144 },
		// D-5：compute pass 的 SSBO。数量很小，但要显式计入，否则
		// ComputeProbePass 在 vkAllocateDescriptorSets 时会直接失败。
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         64 },
		// TD-02：graphics/compute 跨队列探针会读写 storage image。
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          16 }
	};

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	// TD-04: required for descriptor arrays marked UPDATE_AFTER_BIND.
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	poolInfo.maxSets = maxMaterials + 72;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	if (vkCreateDescriptorPool(device->getDevice(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: pool creation failed!");

	createGlobalLayout();
	createMaterialLayout(maxMaterials);
	createTerrainLayout();

	// E3: 材质 factor 表。CPU 侧常驻映射（CPU_TO_GPU），改动时只写 32 字节 +
	// flush，不需要重建描述符；初值把 factor 全设成 1（等于「不改变贴图采样」），
	// 这样没写过 factor 的材质行为和以前一致。
	{
		VkBufferCreateInfo factorInfo{};
		factorInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		factorInfo.size = static_cast<VkDeviceSize>(maxMaterials) * sizeof(MaterialFactorRecord);
		factorInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

		VmaAllocationCreateInfo factorAlloc{};
		factorAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		factorAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VmaAllocationInfo factorAllocInfo{};
		if (vmaCreateBuffer(device->getVmaAllocator(), &factorInfo, &factorAlloc, &materialFactorBuffer,
			&materialFactorAllocation, &factorAllocInfo) != VK_SUCCESS)
			throw std::runtime_error("DescriptorManager: 材质 factor 缓冲创建失败");

		materialFactorMapped = factorAllocInfo.pMappedData;
		materialFactorCount = maxMaterials;
		if (materialFactorMapped)
		{
			MaterialFactorRecord* records = static_cast<MaterialFactorRecord*>(materialFactorMapped);
			for (uint32_t i = 0; i < maxMaterials; ++i)
				records[i] = MaterialFactorRecord{};
			vmaFlushAllocation(device->getVmaAllocator(), materialFactorAllocation, 0, factorInfo.size);
		}
	}

	// ---- D-3：材质 set 只申请一个，长度 = 材质容量 ----
	//
	// 用 VARIABLE_DESCRIPTOR_COUNT 让数组长度在**分配时**决定，而不是写死在布局里。
	// 好处：想调材质容量只需改 init 的 maxMaterials，布局不用动，
	// 也不用为不同容量维护多套布局。
	slotInUse.assign(maxMaterials, false);
	slotPendingRelease.assign(maxMaterials, false);
	freeSlots.reserve(maxMaterials);

	VkDescriptorSetVariableDescriptorCountAllocateInfo variableCount{};
	variableCount.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
	variableCount.descriptorSetCount = 1;
	variableCount.pDescriptorCounts = &maxMaterials;

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &materialSetLayout;
	allocInfo.pNext = &variableCount;

	if (vkAllocateDescriptorSets(device->getDevice(), &allocInfo, &materialSet) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: bindless 材质 set 分配失败");

	// Terrain sets: four bounded sets, each with 12 sampler bindings
	// (4 albedo + 4 normal + 4 packed height/roughness). B-08 loads them lazily.
	for (uint32_t i = 0; i < kTerrainSetCount; ++i)
		terrainSets[i] = allocateSet(terrainSetLayout);

	// 倒序压栈：这样第一次 allocate 拿到的是槽位 0，便于调试时对照
	for (uint32_t i = maxMaterials; i > 0; --i)
		freeSlots.push_back(i - 1);

	std::cout << "[DescriptorManager] Bindless arena ready: 1 set, "
		<< maxMaterials << " material slots ("
		<< (maxMaterials * 4) << " sampler descriptors in 4 arrays).\n";
	std::cout << "[DescriptorManager] Terrain layer sets ready: 4 sets x 12 fixed sampler bindings.\n";
}

void DescriptorManager::cleanup()
{
	// VkDescriptorSet 会随描述符池一起销毁，这里只需要清掉索引表
	slotInUse.clear();
	slotPendingRelease.clear();
	freeSlots.clear();
	deferredMaterialReleases.clear();
	completedFrameQuery = nullptr;
	frameWait = nullptr;
	lastSubmittedFrameQuery = nullptr;
	globalSets.clear();
	materialSet = VK_NULL_HANDLE;
	terrainSets.fill(VK_NULL_HANDLE);
	materialCapacity = 0;

	if (globalSetLayout) vkDestroyDescriptorSetLayout(device->getDevice(), globalSetLayout, nullptr);
	if (materialSetLayout) vkDestroyDescriptorSetLayout(device->getDevice(), materialSetLayout, nullptr);
	if (terrainSetLayout) vkDestroyDescriptorSetLayout(device->getDevice(), terrainSetLayout, nullptr);
	if (materialFactorBuffer)
	{
		vmaDestroyBuffer(device->getVmaAllocator(), materialFactorBuffer, materialFactorAllocation);
		materialFactorBuffer = VK_NULL_HANDLE;
		materialFactorAllocation = nullptr;
		materialFactorMapped = nullptr;
		materialFactorCount = 0;
	}
	if (pool) vkDestroyDescriptorPool(device->getDevice(), pool, nullptr);
}

void DescriptorManager::createGlobalLayout()
{
	// binding 0 = UBO，binding 1~9 = 9 张贴图：
	//   1 equirect 环境贴图（天空盒）
	//   2~5 G-Buffer（位置/法线/albedo/PBR）
	//   6 SSAO
	//   7 irradiance cubemap（漫反射 IBL）
	//   8 prefiltered cubemap（镜面 IBL）
	//   9 BRDF 积分表
	// F6：binding 11 = 3 级联阴影图（sampler2DArray）。
	// C1：binding 12 = 平面反射颜色（半分辨率）。
	std::vector<VkDescriptorSetLayoutBinding> bindings(13);

	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	// E3: binding 10 = 材质 factor 表（SSBO，按材质槽位索引）。
	bindings[10].binding = 10;
	bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[10].descriptorCount = 1;
	bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	bindings[11].binding = 11;
	bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[11].descriptorCount = 1;
	bindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	bindings[12].binding = 12;
	bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[12].descriptorCount = 1;
	bindings[12].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	for (uint32_t i = 1; i <= 9; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();

	if (vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &globalSetLayout) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: global layout failed!");
}

void DescriptorManager::createMaterialLayout(uint32_t capacity)
{
	// D-3 bindless：4 个**变长纹理数组**，各自被同一个材质下标索引。
	//
	// 描述符数组 + 运行时索引是 bindless 的核心：着色器里写
	//     texture(albedoMaps[matId], uv)
	// 其中 matId 来自 push constant，同一个 draw call 里不同像素可以用不同下标
	//（这就是为什么必须开 shaderSampledImageArrayNonUniformIndexing）。
	//
	// descriptorCount 在这里填 0 是不行的（规范要求至少 1）；
	// 真实长度交给 VkDescriptorSetVariableDescriptorCountAllocateInfo 在分配时指定。
	//
	// ⚠️ VARIABLE_DESCRIPTOR_COUNT_BIT 只能加在**编号最大的那个 binding** 上
	//（VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004）。
	// 所以这里不能四个数组都加 —— 加了会直接被验证层拒绝。
	// 四个数组用同一个材质下标、长度自然也相同，只在最后一个 binding 声明即可。
	std::vector<VkDescriptorSetLayoutBinding> bindings(4);

	// 数组里可能有"还没写过内容"的下标（材质槽位是复用池），
	// 允许部分绑定，避免必须把所有槽位先填一张占位图。
	// 这个标志是"每个 binding 各自算"的，所以四个都要加。
	std::vector<VkDescriptorBindingFlags> flags(bindings.size(),
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	flags.back() |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
	flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	flagsInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	flagsInfo.pBindingFlags = flags.data();

	// descriptorCount 这里是**上限**，不是最终长度：
	// 分配时再用 VkDescriptorSetVariableDescriptorCountAllocateInfo 指定实际长度，
	// 而规范要求"实际长度 <= 布局里声明的值"（VUID-...-pSetLayouts-03046）。
	// 所以上限就写 capacity —— 反正描述符池已经按它开好了。
	for (uint32_t i = 0; i < 4; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = capacity;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.pNext = &flagsInfo;
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

	if (vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &materialSetLayout) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: bindless 材质布局创建失败");
}

void DescriptorManager::createTerrainLayout()
{
	// Set 2 is deliberately separate from the bindless material arena. Terrain
	// needs four different albedo+normal pairs in one draw call; packing them
	// into one material slot would require a texture array that Texture does not
	// expose. Fixed bindings keep the normal material path untouched.
	//
	// TD-04 / 运行时切换材质集：TerrainMaterial::uploadMaterialSet() 会在帧进行
	// 中调用 updateTerrainTextures() 重写这些 binding，所以布局必须以
	// UPDATE_AFTER_BIND 方式创建（binding flag + layout flag + pool flag 三者
	// 缺一不可），否则 vkUpdateDescriptorSets 违反
	// VUID-vkUpdateDescriptorSets-None-03047。
	std::array<VkDescriptorSetLayoutBinding, 16> bindings{};
	for (uint32_t i = 0; i < bindings.size(); ++i)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	// 12 个 binding 实际被 mrt.frag 使用，12-15 目前空闲；PARTIALLY_BOUND 让
	// 这些槽位不必先写占位图就能直接使用描述符集。
	std::array<VkDescriptorBindingFlags, 16> bindingFlags{};
	for (auto& flag : bindingFlags)
	{
		flag = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
		       VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
	}

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
	flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
	flagsInfo.pBindingFlags = bindingFlags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.pNext = &flagsInfo;
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	if (vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &terrainSetLayout) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: terrain layer layout failed!");
}

void DescriptorManager::updateTerrainTextures(
uint32_t setIndex,
const std::array<VkDescriptorImageInfo, 4>& albedoInfos,
const std::array<VkDescriptorImageInfo, 4>& normalInfos,
const std::array<VkDescriptorImageInfo, 4>& heightRoughnessInfos)
{
if (setIndex >= kTerrainSetCount || terrainSets[setIndex] == VK_NULL_HANDLE)
throw std::runtime_error("DescriptorManager: terrain set is not allocated");
const VkDescriptorSet terrainSet = terrainSets[setIndex];

std::array<VkDescriptorImageInfo, 12> infos{};
for (uint32_t i = 0; i < 4; ++i)
{
infos[i] = albedoInfos[i];
infos[i + 4] = normalInfos[i];
infos[i + 8] = heightRoughnessInfos[i];
}

std::array<VkWriteDescriptorSet, 12> writes{};
for (uint32_t i = 0; i < writes.size(); ++i)
{
writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
writes[i].dstSet = terrainSet;
writes[i].dstBinding = i;
writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
writes[i].descriptorCount = 1;
writes[i].pImageInfo = &infos[i];
}
vkUpdateDescriptorSets(device->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

VkDescriptorSetLayout DescriptorManager::createSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();

	VkDescriptorSetLayout layout;
	if (vkCreateDescriptorSetLayout(device->getDevice(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: custom layout failed!");
	return layout;
}

void DescriptorManager::buildGlobalSet(
	const std::array<VkBuffer, kFramesInFlight>& uboBuffers,
	const VkDescriptorImageInfo& envInfo,
	const std::array<const GBuffer*, kFramesInFlight>& gBuffers,
	VkSampler ssaoSampler, const std::array<VkImageView, kFramesInFlight>& ssaoViews,
	const VkDescriptorImageInfo& irradianceInfo,
	const VkDescriptorImageInfo& prefilteredInfo,
	const VkDescriptorImageInfo& brdfLutInfo,
	VkSampler shadowSampler, const std::array<VkImageView, kFramesInFlight>& shadowViews,
	VkSampler reflectionSampler, const std::array<VkImageView, kFramesInFlight>& reflectionViews)
{
	// TD-03: rebuild in place so repeated resizes do not exhaust the descriptor pool.
	if (globalSets.size() != kFramesInFlight)
	{
		globalSets.clear();
		globalSets.resize(kFramesInFlight, VK_NULL_HANDLE);
	}
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		if (globalSets[frame] == VK_NULL_HANDLE)
			globalSets[frame] = allocateSet(globalSetLayout);

		VkDescriptorBufferInfo uboInfo{ uboBuffers[frame], 0, sizeof(GlobalUbo) };
		const GBuffer& gBuffer = *gBuffers[frame];

		VkDescriptorImageInfo posInfo{ gBuffer.getSampler(), gBuffer.getPosition().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo normInfo{ gBuffer.getSampler(), gBuffer.getNormal().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo albInfo{ gBuffer.getSampler(), gBuffer.getAlbedo().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo pbrInfo{ gBuffer.getSampler(), gBuffer.getPbr().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo ssaoInfo{ ssaoSampler, ssaoViews[frame], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

		VkDescriptorImageInfo envCopy = envInfo;
		VkDescriptorImageInfo irradianceCopy = irradianceInfo;
		VkDescriptorImageInfo prefilteredCopy = prefilteredInfo;
		VkDescriptorImageInfo brdfLutCopy = brdfLutInfo;

		// 顺序必须和 createGlobalLayout 里的 binding 号一一对应
		VkDescriptorImageInfo imageInfos[] = {
			envCopy, posInfo, normInfo, albInfo, pbrInfo, ssaoInfo,
			irradianceCopy, prefilteredCopy, brdfLutCopy
		};

		std::array<VkWriteDescriptorSet, 13> writes{};

		VkDescriptorImageInfo shadowInfo{ shadowSampler, shadowViews[frame],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[11].dstSet = globalSets[frame];
		writes[11].dstBinding = 11;
		writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[11].descriptorCount = 1;
		writes[11].pImageInfo = &shadowInfo;

		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalSets[frame];
		writes[0].dstBinding = 0;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &uboInfo;

		VkDescriptorImageInfo reflectionInfo{ reflectionSampler, reflectionViews[frame],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[12].dstSet = globalSets[frame];
		writes[12].dstBinding = 12;
		writes[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[12].descriptorCount = 1;
		writes[12].pImageInfo = &reflectionInfo;

		VkDescriptorBufferInfo factorBufferInfo{ materialFactorBuffer, 0, VK_WHOLE_SIZE };
		writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[10].dstSet = globalSets[frame];
		writes[10].dstBinding = 10;
		writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[10].descriptorCount = 1;
		writes[10].pBufferInfo = &factorBufferInfo;

		for (uint32_t i = 0; i < 9; ++i)
		{
			writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i + 1].dstSet = globalSets[frame];
			writes[i + 1].dstBinding = i + 1;
			writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i + 1].descriptorCount = 1;
			writes[i + 1].pImageInfo = &imageInfos[i];
		}

		vkUpdateDescriptorSets(device->getDevice(),
			static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}
}

void DescriptorManager::setFrameTimelineCallbacks(std::function<uint64_t()> completed,
	std::function<void(uint64_t)> waitUntil,
	std::function<uint64_t()> lastSubmitted)
{
	completedFrameQuery = std::move(completed);
	frameWait = std::move(waitUntil);
	lastSubmittedFrameQuery = std::move(lastSubmitted);
}

void DescriptorManager::reclaimReleasedMaterialSlots()
{
	if (deferredMaterialReleases.empty())
		return;

	const uint64_t completed = completedFrameQuery ? completedFrameQuery() : 0;
	while (!deferredMaterialReleases.empty() &&
		deferredMaterialReleases.front().safeAfterFrame <= completed)
	{
		const uint32_t slot = deferredMaterialReleases.front().slot;
		deferredMaterialReleases.pop_front();
		slotInUse[slot] = false;
		slotPendingRelease[slot] = false;
		freeSlots.push_back(slot);
	}
}

void DescriptorManager::collectMaterialGarbage()
{
	reclaimReleasedMaterialSlots();
}

void DescriptorManager::updateMaterialFactors(uint32_t slot, const glm::vec4& baseColor,
	float metallic, float roughness, float shaderFlags)
{
	if (materialFactorMapped == nullptr || slot >= materialFactorCount)
		return;

	MaterialFactorRecord* records = static_cast<MaterialFactorRecord*>(materialFactorMapped);
	records[slot].baseColor = baseColor;
	records[slot].metalRough = glm::vec4(metallic, roughness, shaderFlags, 0.0f);
	vmaFlushAllocation(device->getVmaAllocator(), materialFactorAllocation,
		static_cast<VkDeviceSize>(slot) * sizeof(MaterialFactorRecord), sizeof(MaterialFactorRecord));
}

uint32_t DescriptorManager::allocateMaterialSet(const VkDescriptorImageInfo& albedo,
	const VkDescriptorImageInfo& normal,
	const VkDescriptorImageInfo& metallic,
	const VkDescriptorImageInfo& roughness)
{
	reclaimReleasedMaterialSlots();

	// If every slot is quarantined, wait only for the oldest graphics submission
	// that could still reference them. This is much narrower than a device wait.
	if (freeSlots.empty() && !deferredMaterialReleases.empty())
	{
		if (frameWait)
			frameWait(deferredMaterialReleases.front().safeAfterFrame);
		reclaimReleasedMaterialSlots();
	}

	if (freeSlots.empty())
		throw std::runtime_error("DescriptorManager: material slots exhausted! capacity = "
		+ std::to_string(materialCapacity));

	const uint32_t slot = freeSlots.back();
	freeSlots.pop_back();
	slotInUse[slot] = true;
	slotPendingRelease[slot] = false;

	writeMaterialSet(slot, albedo, normal, metallic, roughness);
	return slot;
}

void DescriptorManager::releaseMaterialSet(uint32_t index)
{
	if (index >= slotInUse.size() || !slotInUse[index] || slotPendingRelease[index])
		return;   // out of range, duplicate release, or already queued

	slotPendingRelease[index] = true;
	const uint64_t safeAfter = lastSubmittedFrameQuery ? lastSubmittedFrameQuery() : 0;
	deferredMaterialReleases.push_back({ index, safeAfter });

	// In boot/init paths there may be no submitted work at all; reclaim immediately.
	reclaimReleasedMaterialSlots();
}



void DescriptorManager::writeMaterialSet(uint32_t slot,
	const VkDescriptorImageInfo& albedo,
	const VkDescriptorImageInfo& normal,
	const VkDescriptorImageInfo& metallic,
	const VkDescriptorImageInfo& roughness)
{
	// bindless 之后"写材质"= 更新数组里下标 slot 那一项。
	// 注意 dstArrayElement：它指定写数组的第几个元素。
	const VkDescriptorImageInfo* infos[4] = { &albedo, &normal, &metallic, &roughness };

	std::vector<VkWriteDescriptorSet> writes(4);
	for (int j = 0; j < 4; j++)
	{
		writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[j].dstSet = materialSet;
		writes[j].dstBinding = static_cast<uint32_t>(j);
		writes[j].dstArrayElement = slot;
		writes[j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[j].descriptorCount = 1;
		writes[j].pImageInfo = infos[j];
	}

	vkUpdateDescriptorSets(device->getDevice(), 4, writes.data(), 0, nullptr);
}

uint32_t DescriptorManager::allocateMaterialSet(const Material& material)
{
	// metallicRoughness 贴图同时写进 metallic / roughness 两个数组：
	// mrt.frag 从 metallicMaps[id].b 取金属度、从 roughnessMaps[id].g 取粗糙度，
	// 两者本来就是同一张 glTF 打包贴图。
	const uint32_t slot = allocateMaterialSet(
		material.getAlbedoInfo(),
		material.getNormalInfo(),
		material.getMetallicRoughnessInfo(),
		material.getMetallicRoughnessInfo());

	// E3: 槽位刚分配（可能是回收复用的槽位），把该材质的 factor 写进去。
	updateMaterialFactors(slot, material.baseColorFactor, material.metallicFactor,
		material.roughnessFactor, material.shaderFlags);
	return slot;
}

void DescriptorManager::refreshMaterialTextures(uint32_t slot, const Material& material)
{
	writeMaterialSet(slot, material.getAlbedoInfo(), material.getNormalInfo(),
		material.getMetallicRoughnessInfo(), material.getMetallicRoughnessInfo());
}

VkDescriptorSet DescriptorManager::allocateSet(VkDescriptorSetLayout layout)
{
	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet set;
	if (vkAllocateDescriptorSets(device->getDevice(), &allocInfo, &set) != VK_SUCCESS)
		throw std::runtime_error("DescriptorManager: set allocation failed!");
	return set;
}

void DescriptorManager::writeSet(VkDescriptorSet set, const std::vector<VkWriteDescriptorSet>& writes)
{
	for (auto& w : const_cast<std::vector<VkWriteDescriptorSet>&>(writes))
		w.dstSet = set;
	vkUpdateDescriptorSets(device->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}
