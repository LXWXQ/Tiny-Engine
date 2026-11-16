#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>

// D3：第一人称手持物通道。
//
// 与场景其它通道的区别只有三点：
//   1. 用 viewmodel 自己的投影（默认 54°、近远平面 0.01..10 米），
//      与场景相机无关，所以贴墙时不会被墙裁掉；
//   2. 位置是相机空间的偏移（ViewModelComponent::offset），不读实体的世界变换；
//   3. 关掉深度测试并画在所有场景内容之后——一个凸盒子开背面剔除后不会自遮挡。
// 将来接入真正的武器模型时，把 buildMesh() 换成加载资产即可，其余不变。
struct ViewModelPushConstantData
{
    alignas(16) glm::mat4 modelView{ 1.0f };
    alignas(16) glm::mat4 projection{ 1.0f };
    alignas(16) glm::vec4 baseColor{ 1.0f };
    alignas(16) glm::vec4 lightDirView{ 0.0f, 0.0f, 1.0f, 1.0f };
};

class ViewModelPass : public IRenderPass
{
public:
    ViewModelPass(Device& device, RenderGraph& graph, RenderResources& resources);
    ~ViewModelPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "ViewModel"; }

private:
    struct Vertex
    {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec3 color;
        glm::vec2 uv;
    };

    void createMesh();
    void destroyMesh();
    void createPipeline();

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    bool loggedReady = false;
};