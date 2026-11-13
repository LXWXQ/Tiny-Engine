#pragma once

#include "Core/DescriptorManager.h"
#include "Core/FrameInFlight.h"
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Vegetation/VegetationGenerator.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Scene;
struct FrameInfo;

// F6-07：地形植被。在 Geometry 之后、SSAO 之前把草和树写进 G-Buffer，
// 这样延迟光照、SSAO、阴影采样和反射都能直接看到植被，而不需要额外一套
// 前向着色。树/草程序化生成，无外部模型和贴图。
struct VegetationPushConstantData
{
    alignas(16) glm::vec4 timeWind{ 0.0f };       // x=time, y=wind, z=mode
    alignas(16) glm::vec4 grassColor{ 0.0f };    // 保留：草色由实例色承载
    alignas(16) glm::vec4 treeColor{ 0.0f };     // 保留：树冠色由实例色承载
    alignas(16) glm::vec4 trunkColor{ 0.0f };
};

class VegetationPass : public IRenderPass
{
public:
    VegetationPass(Device& device, RenderGraph& graph, RenderResources& resources,
                   VkDescriptorSetLayout globalSetLayout, VkFormat depthFormat,
                   Scene* scene = nullptr);
    ~VegetationPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Vegetation"; }

    // ShadowPass calls these once per frame / once per cascade. Trees are the
    // only vegetation that casts shadows: grass shadows are sub-pixel at the
    // 2048 shadow-map resolution and would only add noise.
    void prepareShadow(FrameInfo& frameInfo, uint32_t frame);
    void recordShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProjection,
                      uint32_t frame, uint32_t& drawCalls, uint32_t& triangles);

private:
    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    void createPipeline();
    void createShadowPipeline();
    void createBuffers();
    void destroyBuffers();
    void createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          const char* name, GpuBuffer& out);
    void updatePlacement(FrameInfo& frameInfo, const glm::vec3& cameraPosition);
    void uploadTreeInstances(uint32_t frame);
    void record(VkCommandBuffer cmd, FrameInfo& frameInfo, uint32_t frame);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    Scene* scene = nullptr;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> vegetationPipeline;

    VkPipelineLayout shadowPipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> shadowPipeline;

    GpuBuffer grassVertexBuffer;
    GpuBuffer grassIndexBuffer;
    GpuBuffer treeVertexBuffer;
    GpuBuffer treeIndexBuffer;
    GpuBuffer broadleafVertexBuffer;
    GpuBuffer broadleafIndexBuffer;
    uint32_t grassIndexCount = 0;
    uint32_t treeIndexCount = 0;
    uint32_t broadleafIndexCount = 0;
    uint32_t shadowConiferCount = 0;
    uint32_t shadowBroadleafCount = 0;

    std::array<GpuBuffer, kFramesInFlight> grassInstanceBuffers{};
    std::array<GpuBuffer, kFramesInFlight> treeInstanceBuffers{};

    vegetation::Placement placement;
    // Trees are partitioned by type when uploaded so one instance buffer can
    // feed two draw calls (conifer / broadleaf) without a second allocation.
    std::vector<vegetation::Instance> sortedTrees;
    vegetation::Params lastParams{};
    bool placementValid = false;
    bool hasLastCamera = false;
    glm::vec3 lastCameraPosition{ 0.0f };
    uint64_t lastTerrainSignature = 0;
    uint64_t placedTerrainSignature = 0;
    bool hasTerrainSignature = false;
    float terrainStableSeconds = 0.0f;
    float sinceLastPlacement = 0.0f;

    float elapsedTime = 0.0f;
    uint32_t lastLoggedGrass = 0xFFFFFFFFu;
    uint32_t lastLoggedTrees = 0xFFFFFFFFu;
    bool loggedFirstDraw = false;
};
