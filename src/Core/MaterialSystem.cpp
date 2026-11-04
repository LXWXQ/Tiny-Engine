#include "MaterialSystem.h"
#include <iostream>
#include <vector>

namespace
{
	// F3-f：程序化体素图集 —— 4x4 格 x 64 像素，格内 = BlockId - 1。
	void fillVoxelAtlas(unsigned char* pixels, int size, int tile)
	{
		const unsigned char palette[9][3] = {
			{ 89, 140, 56 }, { 115, 82, 51 }, { 128, 128, 133 },
			{ 209, 194, 140 }, { 97, 64, 36 }, { 64, 122, 51 },
			{ 158, 115, 66 }, { 140, 61, 46 }, { 178, 209, 224 },
		};
		for (int y = 0; y < size; ++y)
		{
			for (int x = 0; x < size; ++x)
			{
				const int id = (y / tile) * 4 + (x / tile);
				const int lx = x % tile;
				const int ly = y % tile;
				unsigned char r = 255, g = 255, b = 255;
				if (id >= 9)
				{
					// 未使用的格子也填成草色：高 mip / 跨格采样时不会把整张图集平均成白色，
					// 否则体素方块边界会因为 LOD 尖峰出现白线。
					r = palette[0][0]; g = palette[0][1]; b = palette[0][2];
				}
				else if (id >= 0 && id < 9)
				{
					r = palette[id][0]; g = palette[id][1]; b = palette[id][2];
					const unsigned int n = (static_cast<unsigned int>(x) * 73856093u)
						^ (static_cast<unsigned int>(y) * 19349663u);
					const int speck = static_cast<int>((n >> 5) % 5);
					const int k = 82 + speck * 6;
					switch (id)
					{
					case 0: case 1: case 3: case 5:
						r = static_cast<unsigned char>(r * k / 100);
						g = static_cast<unsigned char>(g * k / 100);
						b = static_cast<unsigned char>(b * k / 100);
						break;
					case 2:
						if (((lx * 3 + ly * 5) % 29) < 2) { r = 96; g = 96; b = 100; }
						break;
					case 4:
						if ((lx % 7) < 2) { r = static_cast<unsigned char>(r * 78 / 100); g = static_cast<unsigned char>(g * 78 / 100); b = static_cast<unsigned char>(b * 78 / 100); }
						break;
					case 6:
						if ((ly % 9) < 1) { r = static_cast<unsigned char>(r * 72 / 100); g = static_cast<unsigned char>(g * 72 / 100); b = static_cast<unsigned char>(b * 72 / 100); }
						break;
					case 7:
						if ((ly % 16) < 2 || ((lx + ((ly / 16) % 2) * 8) % 16) < 2) { r = 196; g = 190; b = 180; }
						break;
					case 8:
						if (lx < 2 || ly < 2 || lx > tile - 3 || ly > tile - 3) { r = 210; g = 232; b = 240; }
						break;
					default: break;
					}
				}
				const size_t o = (static_cast<size_t>(y) * size + x) * 4;
				pixels[o + 0] = r; pixels[o + 1] = g; pixels[o + 2] = b; pixels[o + 3] = 255;
			}
		}
	}
}
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

	std::vector<unsigned char> atlasPixels(256 * 256 * 4, 255);
	fillVoxelAtlas(atlasPixels.data(), 256, 64);
	// 用 sRGB 格式：调色板是按显示色写的，采样时自动解码到线性空间。
	voxelAtlasTex = std::make_shared<Texture>(device, atlasPixels.data(), 256, 256,
		VK_FORMAT_R8G8B8A8_SRGB, commandPool);
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
	voxelAtlasTex.reset();
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
