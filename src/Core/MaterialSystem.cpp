#include "MaterialSystem.h"
#include <iostream>

void MaterialSystem::initDefaultTextures(Device& device, VkCommandPool commandPool)
{
	unsigned char white[4] = { 255, 255, 255, 255 };
	unsigned char black[4] = { 0, 0, 0, 255 };
	unsigned char flatNormal[4] = { 128, 128, 255, 255 };

	defaultWhiteTex = std::make_shared<Texture>(device, white, 1, 1,
		VK_FORMAT_R8G8B8A8_UNORM, commandPool);
	defaultBlackTex = std::make_shared<Texture>(device, black, 1, 1,
		VK_FORMAT_R8G8B8A8_UNORM, commandPool);
	defaultNormalTex = std::make_shared<Texture>(device, flatNormal, 1, 1,
		VK_FORMAT_R8G8B8A8_UNORM, commandPool);

	Material::defaultWhiteTextureInfo = {
		defaultWhiteTex->getSampler(), defaultWhiteTex->getView(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	};
	Material::defaultBlackTextureInfo = {
		defaultBlackTex->getSampler(), defaultBlackTex->getView(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	};
	Material::defaultNormalTextureInfo = {
		defaultNormalTex->getSampler(), defaultNormalTex->getView(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	};

	std::cout << "[MaterialSystem] Default fallback textures initialized.\n";
}

void MaterialSystem::buildMaterialDescriptor(std::shared_ptr<Material> material, DescriptorManager& descMgr)
{
	uint32_t idx = descMgr.allocateMaterialSet(*material);
	material->descriptorSetIndex = idx;
}

void MaterialSystem::cleanup()
{
	materials.clear();
	textureCache.clear();
	defaultWhiteTex.reset();
	defaultBlackTex.reset();
	defaultNormalTex.reset();
}

std::shared_ptr<Material> MaterialSystem::getOrCreateMaterial(const std::string& name)
{
	auto it = materials.find(name);
	if (it != materials.end())
		return it->second;

	auto mat = std::make_shared<Material>();
	mat->name = name;
	materials[name] = mat;
	return mat;
}

std::shared_ptr<Texture> MaterialSystem::loadTexture(const std::string& filepath, Device& device, VkCommandPool commandPool, bool srgb)
{
	// 颜色贴图必须用 sRGB 格式加载：硬件采样时自动转成线性，
	// 而且各向异性过滤是在正确的色彩空间里进行的。
	// 数据贴图必须保持 UNORM —— 对法线/粗糙度做色彩转换会直接把数据改坏。
	const VkFormat textureFormat = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

	auto it = textureCache.find(filepath);
	if (it != textureCache.end())
		return it->second;

	std::cout << "[MaterialSystem] Loading: " << filepath << std::endl;
	auto tex = std::make_shared<Texture>(device, filepath, textureFormat, commandPool);
	textureCache[filepath] = tex;
	return tex;
}

std::shared_ptr<Texture> MaterialSystem::getTexture(const std::string& filepath) const
{
	auto it = textureCache.find(filepath);
	return (it != textureCache.end()) ? it->second : nullptr;
}
