#pragma once
#include <string>
#include <memory>
#include <unordered_map>
#include "Material.h"
#include "RHI/Device.h"
#include "Core/DescriptorManager.h"

class MaterialSystem
{
public:
	void initDefaultTextures(Device& device, VkCommandPool commandPool);
	void buildMaterialDescriptor(std::shared_ptr<Material> material, DescriptorManager& descMgr);
	void cleanup();

	std::shared_ptr<Material> getOrCreateMaterial(const std::string& name);
	// srgb = true：颜色贴图（albedo / emissive），用 VK_FORMAT_*_SRGB 加载；
	// srgb = false：数据贴图（normal / metallic-roughness / AO），必须保持 UNORM。
	std::shared_ptr<Texture> loadTexture(const std::string& filepath, Device& device, VkCommandPool commandPool, bool srgb = false);
	std::shared_ptr<Texture> getTexture(const std::string& filepath) const;
	// F3：体素网格用顶点色，材质需要一张白贴图占位（否则走棋盘格 fallback）。
	std::shared_ptr<Texture> getDefaultWhiteTexture() const { return defaultWhiteTex; }
	// F3-f：程序化体素图集（4x4 格 x 64px，格内 = BlockId - 1）。
	std::shared_ptr<Texture> getVoxelAtlasTexture() const { return voxelAtlasTex; }

private:
	std::unordered_map<std::string, std::shared_ptr<Material>> materials;
	std::unordered_map<std::string, std::shared_ptr<Texture>> textureCache;

	std::shared_ptr<Texture> defaultWhiteTex;
	std::shared_ptr<Texture> voxelAtlasTex;
	std::shared_ptr<Texture> defaultBlackTex;
	std::shared_ptr<Texture> defaultNormalTex;
};
