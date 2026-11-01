#pragma once

#include "RHI/Device.h"
#include "RHI/Texture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class DescriptorManager;

// B-08: bounded, lazily loaded terrain material library.
//
// Set 0 is the default root (Resources/Texture/Terrain/). Sets 1..3 are loaded
// on demand from sibling directories set1/, set2/, set3/. Each set owns the
// same 12 Set2 textures (4 albedo + 4 normal + 4 packed height/roughness).
// Incomplete directories fall back to deterministic procedural tiles, so an
// artist can stream a set in without making startup depend on all 16 PNGs.
class TerrainMaterial
{
public:
    static constexpr uint32_t kMaxMaterialSets = 4;

    struct Stats
    {
        uint32_t residentSets = 0;
        uint32_t externalSets = 0;
        uint32_t proceduralSets = 0;
        uint32_t loadRequests = 0;
        uint32_t maxSets = kMaxMaterialSets;
    };

    TerrainMaterial(Device& device, VkCommandPool commandPool,
                    DescriptorManager& descriptorManager,
                    const std::string& externalTextureDir);
    ~TerrainMaterial() = default;

    TerrainMaterial(const TerrainMaterial&) = delete;
    TerrainMaterial& operator=(const TerrainMaterial&) = delete;

    // Loads a set on first use. Empty directoryOverride selects the standard
    // root (set 0) or root/setN (N > 0).
    bool ensureMaterialSet(uint32_t index, std::string& errorMessage);
    bool ensureMaterialSetFromDirectory(uint32_t index, const std::string& directoryOverride,
                                        std::string& errorMessage);

    bool isMaterialSetReady(uint32_t index) const;
    const Stats& getStats() const { return stats; }
    const std::string& getRootDirectory() const { return rootDirectory; }

private:
    struct MaterialSet
    {
        std::array<std::shared_ptr<Texture>, 4> albedoTextures;
        std::array<std::shared_ptr<Texture>, 4> normalTextures;
        std::array<std::shared_ptr<Texture>, 4> heightRoughnessTextures;
        std::string directory;
        bool loaded = false;
        bool external = false;
    };

    std::string defaultDirectoryForSet(uint32_t index) const;
    bool uploadMaterialSet(uint32_t index, const std::string& directory,
                           std::string& errorMessage);

    Device* device = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;
    std::string rootDirectory;
    std::array<MaterialSet, kMaxMaterialSets> materialSets{};
    Stats stats{};
};
