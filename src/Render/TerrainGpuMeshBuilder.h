#pragma once

#include "Core/DescriptorManager.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Scene/Model.h"
#include "Terrain/TerrainConfig.h"
#include "Terrain/TerrainHeightField.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

struct TerrainGpuMeshBuildResult
{
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;

    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    std::vector<SubMesh> subMeshes;
    std::vector<glm::vec3> pickingPositions;
    std::vector<uint32_t> pickingIndices;
    glm::vec3 boundsMin{ 0.0f };
    glm::vec3 boundsMax{ 0.0f };
    glm::vec2 uvMin{ 0.0f };
    glm::vec2 uvMax{ 0.0f };
    bool uvValid = false;
    double assemblyMs = 0.0;

    TerrainGpuMeshBuildResult() = default;
    TerrainGpuMeshBuildResult(const TerrainGpuMeshBuildResult&) = delete;
    TerrainGpuMeshBuildResult& operator=(const TerrainGpuMeshBuildResult&) = delete;

    TerrainGpuMeshBuildResult(TerrainGpuMeshBuildResult&& other) noexcept
    {
        *this = std::move(other);
    }

    TerrainGpuMeshBuildResult& operator=(TerrainGpuMeshBuildResult&& other) noexcept
    {
        if (this == &other)
            return *this;
        vertexBuffer = other.vertexBuffer;
        vertexAllocation = other.vertexAllocation;
        vertexCount = other.vertexCount;
        indexBuffer = other.indexBuffer;
        indexAllocation = other.indexAllocation;
        indexCount = other.indexCount;
        subMeshes = std::move(other.subMeshes);
        pickingPositions = std::move(other.pickingPositions);
        pickingIndices = std::move(other.pickingIndices);
        boundsMin = other.boundsMin;
        boundsMax = other.boundsMax;
        uvMin = other.uvMin;
        uvMax = other.uvMax;
        uvValid = other.uvValid;
        assemblyMs = other.assemblyMs;

        other.vertexBuffer = VK_NULL_HANDLE;
        other.vertexAllocation = VK_NULL_HANDLE;
        other.vertexCount = 0;
        other.indexBuffer = VK_NULL_HANDLE;
        other.indexAllocation = VK_NULL_HANDLE;
        other.indexCount = 0;
        other.subMeshes.clear();
        other.pickingPositions.clear();
        other.pickingIndices.clear();
        other.uvValid = false;
        other.assemblyMs = 0.0;
        return *this;
    }
};

// B-05: asynchronous single-chunk GPU mesh assembly. Multiple chunk requests
// can be submitted back-to-back; Scene polls fences each frame and installs the
// completed Model on the main thread. No submission path waits on a GPU fence.
class TerrainGpuMeshBuilder
{
public:
    static constexpr uint32_t kAsyncSlots = 4;

    enum class PollStatus
    {
        Pending,
        Completed,
        Failed
    };

    struct Stats
    {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t rejected = 0;
        double maxSubmitMs = 0.0;
        double maxPollMs = 0.0;
        double maxAssemblyMs = 0.0;
    };

    TerrainGpuMeshBuilder(Device& device, DescriptorManager& descriptorManager);
    ~TerrainGpuMeshBuilder();

    TerrainGpuMeshBuilder(const TerrainGpuMeshBuilder&) = delete;
    TerrainGpuMeshBuilder& operator=(const TerrainGpuMeshBuilder&) = delete;

    bool isAvailable() const { return available; }
    bool supports(const terrain::TerrainConfig& config) const;

    uint32_t freeSlotCount();
    bool hasFreeSlot() { return freeSlotCount() != 0u; }

    // Returns 0 when the request was rejected. A non-zero id must be consumed
    // with poll()/release() and eventually with collectFinished()/waitAll().
    uint64_t submit(const terrain::TerrainConfig& config,
                    const terrain::HeightField& heightField,
                    std::string& errorMessage);

    PollStatus poll(uint64_t requestId,
                    TerrainGpuMeshBuildResult& outResult,
                    std::string& errorMessage);

    // Cooperative cancellation: the slot is reaped after its fence signals.
    void release(uint64_t requestId);
    void collectFinished();
    void waitAll();

    const Stats& getStats() const { return stats; }

    // Synchronous convenience wrapper for scene loading and self-tests.
    // It still goes through submit()/poll(); production frames use the async API.
    bool build(const terrain::TerrainConfig& config,
               const terrain::HeightField& heightField,
               TerrainGpuMeshBuildResult& outResult,
               std::string& errorMessage);

    bool selfTest(std::string& errorMessage);

private:
    static constexpr uint32_t kMaxLods = 3;
    static constexpr uint32_t kVertexFloats = 11; // Vertex is pos3/color3/normal3/uv2.
    static constexpr uint32_t kMaxResolution = terrain::TerrainConfig::kMaxResolution;

    struct PushConstants
    {
        uint32_t mode = 0;      // 0=vertices, 1=indices
        uint32_t itemCount = 0;
    };

    struct GpuMeshParams
    {
        glm::uvec4 sizes{ 0u };          // resX,resZ,lodCount,pad
        glm::vec4 world{ 0.0f };         // sizeX,sizeZ,baseHeight,heightScale
        glm::vec4 colorParams{ 0.0f };   // min,max,skirtDepth,uvTiling
        glm::uvec4 flags{ 0u };          // skirts,localColorRange,chunkCellsX,chunkCellsZ
        glm::uvec4 lodVertexOffset{ 0u };
        glm::uvec4 lodVertexCount{ 0u };
        glm::uvec4 lodIndexOffset{ 0u };
        glm::uvec4 lodIndexCount{ 0u };
        glm::uvec4 lodSurfaceIndexCount{ 0u };
    };

    struct LodLayout
    {
        bool canStitch = false;
        uint32_t step = 1;
        uint32_t innerSegX = 0;
        uint32_t innerSegZ = 0;
        uint32_t innerVertexCount = 0;
        uint32_t outerRingCount = 0;
        uint32_t surfaceVertexCount = 0;
        uint32_t surfaceIndexCount = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t vertexOffset = 0;
        uint32_t indexOffset = 0;
    };

    struct Layout
    {
        std::array<LodLayout, kMaxLods> lods{};
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t chunkCellsX = 0;
        uint32_t chunkCellsZ = 0;
        float skirtDepth = 0.0f;
    };

    struct Slot
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

        VkBuffer heightBuffer = VK_NULL_HANDLE;
        VmaAllocation heightAllocation = VK_NULL_HANDLE;
        void* heightMapped = nullptr;
        VkBuffer paramsBuffer = VK_NULL_HANDLE;
        VmaAllocation paramsAllocation = VK_NULL_HANDLE;
        GpuMeshParams* paramsMapped = nullptr;

        uint64_t requestId = 0;
        bool active = false;
        bool abandoned = false;
        bool submitted = false;
        Layout layout;
        TerrainGpuMeshBuildResult result;

        std::chrono::steady_clock::time_point startTime;
    };

    void createResources();
    void destroyResources();
    bool createPersistentBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                bool hostVisible, VkBuffer& buffer,
                                VmaAllocation& allocation, void** mapped);
    bool createOutputBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer& buffer, VmaAllocation& allocation);
    void destroyBuffer(VkBuffer& buffer, VmaAllocation& allocation);
    bool prepareBuild(const terrain::TerrainConfig& config,
                      const terrain::HeightField& heightField,
                      TerrainGpuMeshBuildResult& outResult,
                      Layout& outLayout,
                      std::string& errorMessage);
    bool updateDescriptors(Slot& slot, std::string& errorMessage);
    bool recordAndSubmit(Slot& slot,
                         const terrain::TerrainConfig& config,
                         const terrain::HeightField& heightField,
                         std::string& errorMessage);
    bool readbackBuffers(const TerrainGpuMeshBuildResult& result,
                         std::vector<float>& outVertexFloats,
                         std::vector<uint32_t>& outIndices,
                         std::string& errorMessage);
    Layout computeLayout(const terrain::TerrainConfig& config) const;
    void destroyBuildResult(TerrainGpuMeshBuildResult& result);
    void clearSlot(Slot& slot);
    void destroySlotResult(Slot& slot);

    Device& device;
    DescriptorManager& descriptorManager;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> pipeline;

    std::array<Slot, kAsyncSlots> slots{};
    VkCommandBuffer readbackCommandBuffer = VK_NULL_HANDLE;
    VkFence readbackFence = VK_NULL_HANDLE;

    uint64_t nextRequestId = 1;
    Stats stats;
    bool available = false;
};
