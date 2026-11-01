#include "Render/TerrainMaterial.h"
#include "Core/DescriptorManager.h"
#include "Terrain/TerrainTextureGenerator.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <stb_image.h>

namespace
{
constexpr std::array<const char*, 4> kLayerNames = { "sand", "grass", "rock", "snow" };
constexpr std::array<const char*, 4> kMapNames = { "albedo", "normal", "height", "roughness" };

std::array<std::array<std::string, 4>, 4> makeExternalPaths(const std::string& textureDir)
{
    std::array<std::array<std::string, 4>, 4> paths{};
    for (uint32_t mapIndex = 0; mapIndex < kMapNames.size(); ++mapIndex)
    {
        for (uint32_t layer = 0; layer < kLayerNames.size(); ++layer)
        {
            const std::string filename = std::string(kMapNames[mapIndex]) + "_" +
                                         kLayerNames[layer] + ".png";
            paths[mapIndex][layer] =
                (std::filesystem::path(textureDir) / filename).string();
        }
    }
    return paths;
}

bool hasCompleteExternalSet(const std::array<std::array<std::string, 4>, 4>& paths)
{
    std::error_code error;
    for (const auto& mapPaths : paths)
    {
        for (const std::string& path : mapPaths)
        {
            if (!std::filesystem::is_regular_file(path, error) || error)
                return false;
        }
    }
    return true;
}

template <typename TextureArray>
void fillImageInfos(std::array<VkDescriptorImageInfo, 4>& infos, const TextureArray& textures)
{
    for (uint32_t layer = 0; layer < textures.size(); ++layer)
    {
        infos[layer] = {
            textures[layer]->getSampler(),
            textures[layer]->getView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
    }
}

struct PackedImage
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

PackedImage packHeightRoughness(int width, int height,
                                const uint8_t* heightRGBA,
                                const uint8_t* roughnessRGBA)
{
    PackedImage packed;
    packed.width = width;
    packed.height = height;
    packed.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (size_t i = 0; i < packed.rgba.size(); i += 4u)
    {
        packed.rgba[i + 0u] = heightRGBA[i + 0u];
        packed.rgba[i + 1u] = roughnessRGBA[i + 0u];
        packed.rgba[i + 2u] = heightRGBA[i + 1u];
        packed.rgba[i + 3u] = roughnessRGBA[i + 1u];
    }
    return packed;
}

PackedImage loadExternalHeightRoughness(const std::string& heightPath,
                                        const std::string& roughnessPath)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* heightPixels = stbi_load(heightPath.c_str(), &width, &height,
                                      &channels, STBI_rgb_alpha);
    if (!heightPixels)
        throw std::runtime_error("TerrainMaterial: failed to load " + heightPath);

    int roughnessWidth = 0;
    int roughnessHeight = 0;
    int roughnessChannels = 0;
    stbi_uc* roughnessPixels = stbi_load(roughnessPath.c_str(),
                                         &roughnessWidth, &roughnessHeight,
                                         &roughnessChannels, STBI_rgb_alpha);
    if (!roughnessPixels)
    {
        stbi_image_free(heightPixels);
        throw std::runtime_error("TerrainMaterial: failed to load " + roughnessPath);
    }
    if (roughnessWidth != width || roughnessHeight != height)
    {
        stbi_image_free(heightPixels);
        stbi_image_free(roughnessPixels);
        throw std::runtime_error(
            "TerrainMaterial: height/roughness dimensions differ for " + heightPath);
    }

    PackedImage packed = packHeightRoughness(width, height, heightPixels, roughnessPixels);
    stbi_image_free(heightPixels);
    stbi_image_free(roughnessPixels);
    return packed;
}

} // namespace

static_assert(TerrainMaterial::kMaxMaterialSets == DescriptorManager::kTerrainSetCount,
              "TerrainMaterial and DescriptorManager set counts must match");

TerrainMaterial::TerrainMaterial(Device& deviceArg, VkCommandPool commandPoolArg,
                                 DescriptorManager& descriptorManagerArg,
                                 const std::string& externalTextureDir)
    : device(&deviceArg), commandPool(commandPoolArg),
      descriptorManager(&descriptorManagerArg), rootDirectory(externalTextureDir)
{
    std::string errorMessage;
    if (!ensureMaterialSet(0, errorMessage))
        throw std::runtime_error("TerrainMaterial: " + errorMessage);
}

std::string TerrainMaterial::defaultDirectoryForSet(uint32_t index) const
{
    if (index == 0)
        return rootDirectory;
    return (std::filesystem::path(rootDirectory) / ("set" + std::to_string(index))).string();
}

bool TerrainMaterial::isMaterialSetReady(uint32_t index) const
{
    return index < kMaxMaterialSets && materialSets[index].loaded;
}

bool TerrainMaterial::ensureMaterialSet(uint32_t index, std::string& errorMessage)
{
    return ensureMaterialSetFromDirectory(index, std::string(), errorMessage);
}

bool TerrainMaterial::ensureMaterialSetFromDirectory(uint32_t index,
                                                     const std::string& directoryOverride,
                                                     std::string& errorMessage)
{
    errorMessage.clear();
    if (index >= kMaxMaterialSets)
    {
        errorMessage = "material set index " + std::to_string(index) +
                       " exceeds the bounded maximum " + std::to_string(kMaxMaterialSets);
        return false;
    }
    if (materialSets[index].loaded)
        return true;

    ++stats.loadRequests;
    const std::string directory = directoryOverride.empty()
        ? defaultDirectoryForSet(index)
        : directoryOverride;
    return uploadMaterialSet(index, directory, errorMessage);
}

bool TerrainMaterial::uploadMaterialSet(uint32_t index, const std::string& directory,
                                        std::string& errorMessage)
{
    constexpr uint32_t kTextureResolution = 256;
    const auto externalPaths = makeExternalPaths(directory);
    bool externalComplete = hasCompleteExternalSet(externalPaths);

    MaterialSet set;
    set.directory = directory;

    if (externalComplete)
    {
        try
        {
            for (uint32_t layer = 0; layer < 4; ++layer)
            {
                set.albedoTextures[layer] = std::make_shared<Texture>(
                    *device, externalPaths[0][layer], VK_FORMAT_R8G8B8A8_SRGB, commandPool);
                set.normalTextures[layer] = std::make_shared<Texture>(
                    *device, externalPaths[1][layer], VK_FORMAT_R8G8B8A8_UNORM, commandPool);
                const PackedImage packed = loadExternalHeightRoughness(
                    externalPaths[2][layer], externalPaths[3][layer]);
                set.heightRoughnessTextures[layer] = std::make_shared<Texture>(
                    *device, packed.rgba.data(), packed.width, packed.height,
                    VK_FORMAT_R8G8B8A8_UNORM, commandPool);
                set.heightRoughnessTextures[layer]->setPath(
                    "external://terrain/set" + std::to_string(index) + "/layer" +
                    std::to_string(layer) + "/height_roughness");
            }
            set.external = true;
        }
        catch (const std::exception& ex)
        {
            std::cout << "[TerrainMaterial] set " << index
                      << " external load failed; procedural fallback | " << ex.what() << std::endl;
            set = MaterialSet{};
            set.directory = directory;
            externalComplete = false;
        }
    }

    if (!externalComplete)
    {
        terrain::TerrainTextureSet generated;
        if (!terrain::generateTerrainTextureSet(generated, kTextureResolution, errorMessage))
        {
            errorMessage = "procedural set " + std::to_string(index) + ": " + errorMessage;
            return false;
        }

        for (uint32_t layer = 0; layer < 4; ++layer)
        {
            const std::string prefix = "procedural://terrain/set" + std::to_string(index) +
                                       "/layer" + std::to_string(layer);
            set.albedoTextures[layer] = std::make_shared<Texture>(
                *device, generated.albedoRGBA[layer].data(), generated.width, generated.height,
                VK_FORMAT_R8G8B8A8_SRGB, commandPool);
            set.albedoTextures[layer]->setPath(prefix + "/albedo");

            set.normalTextures[layer] = std::make_shared<Texture>(
                *device, generated.normalRGBA[layer].data(), generated.width, generated.height,
                VK_FORMAT_R8G8B8A8_UNORM, commandPool);
            set.normalTextures[layer]->setPath(prefix + "/normal");

            const PackedImage packed = packHeightRoughness(
                static_cast<int>(generated.width), static_cast<int>(generated.height),
                generated.heightRGBA[layer].data(), generated.roughnessRGBA[layer].data());
            set.heightRoughnessTextures[layer] = std::make_shared<Texture>(
                *device, packed.rgba.data(), packed.width, packed.height,
                VK_FORMAT_R8G8B8A8_UNORM, commandPool);
            set.heightRoughnessTextures[layer]->setPath(prefix + "/height_roughness");
        }
        set.external = false;
    }

    std::array<VkDescriptorImageInfo, 4> albedoInfos{};
    std::array<VkDescriptorImageInfo, 4> normalInfos{};
    std::array<VkDescriptorImageInfo, 4> packedInfos{};
    fillImageInfos(albedoInfos, set.albedoTextures);
    fillImageInfos(normalInfos, set.normalTextures);
    fillImageInfos(packedInfos, set.heightRoughnessTextures);
    descriptorManager->updateTerrainTextures(index, albedoInfos, normalInfos, packedInfos);

    set.loaded = true;
    materialSets[index] = std::move(set);
    ++stats.residentSets;
    if (materialSets[index].external)
        ++stats.externalSets;
    else
        ++stats.proceduralSets;

    std::cout << "[TerrainMaterial] set " << index << " ready | "
              << (materialSets[index].external ? "external 16 PNG" : "procedural fallback")
              << " | " << materialSets[index].directory << std::endl;
    return true;
}
