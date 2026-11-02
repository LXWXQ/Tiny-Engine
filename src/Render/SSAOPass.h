#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <array>
#include "Core/FrameInFlight.h"

struct SSAOUbo
{
    glm::mat4 projection;
    glm::mat4 view;
    glm::vec4 samples[64];
    int kernelSize = 64;
    float radius = 0.5f;
    float bias = 0.025f;
    float enabled = 1.0f;
};

// 屏幕空间环境光遮蔽。
//
// D-1(b)：输出目标由渲染图创建（createTexture），自身不再持有 RenderPass / Framebuffer。
// D-2   ：输出格式保持 R8_UNORM —— 遮蔽因子是纯 0~1 的调制量，不需要 HDR。
//         它只调制光照里的环境项（见 lighting.frag 结尾），不参与 Bloom。
class SSAOPass : public IRenderPass
{
public:
    SSAOPass(Device& device, RenderGraph& graph, RenderResources& resources,
             VkDescriptorSetLayout globalLayout, DescriptorManager* descMgr);
    ~SSAOPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "SSAO"; }

    // 供 Application 写进每帧的全局描述符集（binding 6）。
    VkSampler   getOutputSampler() const { return outputSampler; }
    VkImageView getOutputImageView(uint32_t frameInFlightIndex) const;

private:
    void generateSampleKernel();
    void createNoiseTexture();
    void createUniformBuffer();
    void createLocalDescriptors();
    void createPipeline();
    void createBlurPipeline();
    void updateBlurDescriptor(uint32_t frame);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager;
    VkDescriptorSetLayout globalSetLayout;

    std::vector<glm::vec4> ssaoKernel;

    struct
    {
        VkImage       image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView   view = VK_NULL_HANDLE;
        VkSampler     sampler = VK_NULL_HANDLE;
    } noiseTexture;

    std::array<VkBuffer, kFramesInFlight>      uboBuffers{};
    std::array<VmaAllocation, kFramesInFlight> uboAllocations{};
    std::array<void*, kFramesInFlight>         uboMapped{};

    VkSampler     outputSampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout localSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> localSets{};

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> ssaoPipeline;

    VkDescriptorSetLayout blurSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> blurSets{};
    VkPipelineLayout blurPipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> blurPipeline;
};