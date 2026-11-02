#pragma once

#include "Core/DescriptorManager.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Terrain/TerrainHeightField.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

// GPU compute path for the terrain heightfield. Multiple requests can be in
// flight; the caller submits and polls fences on the main thread, then runs the
// CPU mesh builder only after readback has completed.
class TerrainComputeGenerator
{
public:
    enum class PollStatus
    {
        Pending,
        Completed,
        Failed
    };

    TerrainComputeGenerator(Device& device, DescriptorManager& descriptorManager);
    ~TerrainComputeGenerator();

    TerrainComputeGenerator(const TerrainComputeGenerator&) = delete;
    TerrainComputeGenerator& operator=(const TerrainComputeGenerator&) = delete;

    bool isAvailable() const { return available; }
    uint32_t inFlightCount() const;

    // Returns a non-zero request ID on success. The compute submission is
    // asynchronous; the caller owns polling until Completed/Failed.
    uint64_t submit(const terrain::TerrainConfig& config, std::string& errorMessage);
    PollStatus poll(uint64_t requestId, terrain::HeightField& outHeightField,
                    std::string& errorMessage);

    // Mark a request as abandoned. Its slot is reclaimed by collectFinished()
    // after the GPU fence signals.
    void release(uint64_t requestId);
    void collectFinished();
    void waitAll();

    // Synchronous helper for scene loading / pure build helpers.
    bool generate(const terrain::TerrainConfig& config,
                  terrain::HeightField& outHeightField,
                  std::string& errorMessage);

private:
    struct PushConstants
    {
        uint32_t resolutionX;
        uint32_t resolutionZ;
        float sizeX;
        float sizeZ;
        float sampleOriginX;
        float sampleOriginZ;
        uint32_t seed;
        uint32_t octaveCount;
        float noiseFrequency;
        float persistence;
        float lacunarity;
        float ridgeWeight;
        float detailWeight;
        float edgeFalloff;
        float heightCurve;
        float islandWeight;
        float islandRadius;
        float ridgeSharpness;
        float detailFrequency;
    };

    struct Slot
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer outputBuffer = VK_NULL_HANDLE;
        VmaAllocation outputAllocation = VK_NULL_HANDLE;
        VkBuffer readbackBuffer = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        void* readbackMapped = nullptr;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

        bool inFlight = false;
        bool abandoned = false;
        uint64_t requestId = 0;
        terrain::TerrainConfig config;
    };

    static constexpr uint32_t kSlotCount = 8;
    static constexpr uint32_t kMaxResolution = terrain::TerrainConfig::kMaxResolution;
    static constexpr VkDeviceSize kMaxHeightBytes =
        static_cast<VkDeviceSize>(kMaxResolution) * kMaxResolution * sizeof(float);

    void createCommandPool();
    void createBuffers();
    void createDescriptorSets();
    void createPipeline();
    void destroyResources();

    bool recordAndSubmit(Slot& slot, const terrain::TerrainConfig& config,
                         std::string& errorMessage);
    void resetSlot(Slot& slot);
    Slot* findRequest(uint64_t requestId);

    Device& device;
    DescriptorManager& descriptorManager;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> pipeline;
    std::array<Slot, kSlotCount> slots{};
    uint64_t nextRequestId = 1;
    bool available = false;
};
