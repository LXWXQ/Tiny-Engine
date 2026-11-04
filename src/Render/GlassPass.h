#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"
#include "Scene/Model.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// V-01：体素玻璃的前向混合通道。
//
// 为什么单独一个 Pass：G-Buffer 延迟管线里没有透明层，玻璃如果留在
// Geometry 里会把深度写死，后面画的玻璃被自己挡住；而且延迟光照阶段
// 只读 G-Buffer，玻璃的混合需要按面从远到近排序，只能在光照之后做。
//
// 几何来源：Scene 为每个 voxel chunk 额外保留了 glassMesh（世界变换前的
// 每个玻璃外表面一个 quad）。本 Pass 把这些顶点变换到世界空间收进一个
// 共享池，再把索引按「面质心到相机距离」降序重排后一次画完。
struct GlassPushConstantData
{
    uint32_t materialId = 0xFFFFFFFFu;
    float alpha = 0.45f;
};

class GlassPass : public IRenderPass
{
public:
    GlassPass(Device& device, RenderGraph& graph, RenderResources& resources,
              VkDescriptorSetLayout globalSetLayout, VkDescriptorSetLayout materialSetLayout,
              DescriptorManager* descriptorManager, VkFormat depthFormat);
    ~GlassPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Glass"; }

private:
    // 每个玻璃面 = 4 个顶点；baseVertex 指向顶点池里的第一个顶点。
    struct FaceRecord
    {
        uint32_t baseVertex = 0;
        glm::vec3 centroid{ 0.0f };
    };

    // 用来判断「玻璃几何是否需要重建」：chunk 的网格版本 + 世界变换版本。
    struct ChunkState
    {
        uint64_t meshRevision = 0;
        uint64_t transformRevision = 0;
    };

    void createBuffers();
    void destroyBuffers();
    void createPipeline();
    void refreshChunkStatesAndRebuild(FrameInfo& frameInfo);
    void rebuildVertexPool(FrameInfo& frameInfo);
    uint32_t writeSortedIndices(uint32_t frame, const glm::vec3& cameraPosition);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> glassPipeline;

    // 顶点池：所有玻璃面共用一份，按 chunk 版本重建。用主机可见内存，
    // 因为重建发生在编辑体素时，量级是「数千个 quad」，不值得再走一次暂存拷贝。
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    void* vertexMapped = nullptr;

    // 索引缓冲按帧各一份：排序结果每帧都变，两帧在飞时共用一个缓冲
    // 会让上一帧还在读的索引被这一帧覆写。
    std::array<VkBuffer, kFramesInFlight> indexBuffers{};
    std::array<VmaAllocation, kFramesInFlight> indexAllocations{};
    std::array<void*, kFramesInFlight> indexMapped{};

    // 固定容量：运行期不重建缓冲，只在容量内截断。65536 面 ≈ 8 MB 顶点 + 1.5 MB 索引。
    static constexpr uint32_t kMaxGlassFaces = 65536;
    uint32_t vertexCount = 0;
    uint32_t faceCount = 0;
    uint32_t materialId = 0xFFFFFFFFu;

    std::vector<FaceRecord> faces;
    std::vector<uint32_t> sortedFaces;
    std::unordered_map<uint32_t, ChunkState> chunkStates;

    bool poolOverflowLogged = false;
    uint32_t lastLoggedFaceCount = 0xFFFFFFFFu;
    bool loggedEmpty = false;
};
