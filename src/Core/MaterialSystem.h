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

private:
	std::unordered_map<std::string, std::shared_ptr<Material>> materials;
	std::unordered_map<std::string, std::shared_ptr<Texture>> textureCache;

	std::shared_ptr<Texture> defaultWhiteTex;
	std::shared_ptr<Texture> defaultBlackTex;
	std::shared_ptr<Texture> defaultNormalTex;
};
