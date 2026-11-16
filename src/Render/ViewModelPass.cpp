#include "ViewModelPass.h"

#include "AssetPaths.h"
#include "Scene/Components.h"
#include "Scene/ViewModel.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

static_assert(sizeof(ViewModelPushConstantData) == 160,
              "viewmodel push constant must stay 160 bytes (2 mat4 + 2 vec4)");

namespace
{
// 程序化的单位立方体：每个面 4 个顶点（各自独立法线），24 顶点 / 36 索引。
// 有真实武器资产后，这里换成 Model 即可，通道其余部分不用动。
constexpr std::array<glm::vec3, 6> kFaceNormals = {
    glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f),
    glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
    glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)
};

struct FaceCorners
{
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 c;
    glm::vec3 d;
};

constexpr std::array<FaceCorners, 6> kFaceCorners = {
    FaceCorners{ { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f } },
    FaceCorners{ { 0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { -0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f } },
    FaceCorners{ { 0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } },
    FaceCorners{ { -0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, -0.5f } },
    FaceCorners{ { -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, -0.5f }, { -0.5f, 0.5f, -0.5f } },
    FaceCorners{ { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, 0.5f }, { -0.5f, -0.5f, 0.5f } }
};
} // namespace

ViewModelPass::ViewModelPass(Device& device, RenderGraph& graph, RenderResources& resources)
    : device(device), renderGraph(graph), resources(resources)
{
}

ViewModelPass::~ViewModelPass()
{
    destroyMesh();
    if (pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
}

void ViewModelPass::init()
{
    createMesh();
    createPipeline();
}

void ViewModelPass::createMesh()
{
    std::vector<Vertex> vertices;
    vertices.reserve(24);
    std::vector<uint32_t> indices;
    indices.reserve(36);

    // 每个面给一点亮度差，纯色方块也能看出三个面。
    const std::array<float, 6> tints = { 1.0f, 0.88f, 0.96f, 0.92f, 1.0f, 0.82f };

    for (std::size_t face = 0; face < kFaceCorners.size(); ++face)
    {
        const FaceCorners& corners = kFaceCorners[face];
        const glm::vec3 normal = kFaceNormals[face];
        const float tint = tints[face];
        const glm::vec3 color(tint, tint, tint);
        vertices.push_back(Vertex{ corners.a, normal, color, glm::vec2(0.0f, 0.0f) });
        vertices.push_back(Vertex{ corners.b, normal, color, glm::vec2(1.0f, 0.0f) });
        vertices.push_back(Vertex{ corners.c, normal, color, glm::vec2(1.0f, 1.0f) });
        vertices.push_back(Vertex{ corners.d, normal, color, glm::vec2(0.0f, 1.0f) });

        const uint32_t base = static_cast<uint32_t>(face) * 4u;
        indices.push_back(base + 0u);
        indices.push_back(base + 1u);
        indices.push_back(base + 2u);
        indices.push_back(base + 0u);
        indices.push_back(base + 2u);
        indices.push_back(base + 3u);
    }
    indexCount = static_cast<uint32_t>(indices.size());

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertices.size() * sizeof(Vertex));
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t));

    auto createBuffer = [this](VkDeviceSize size, VkBufferUsageFlags usage, const char* name,
                               VkBuffer& buffer, VmaAllocation& allocation) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>(name);

        VmaAllocationInfo allocationInfo{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &info, &allocInfo, &buffer, &allocation,
                            &allocationInfo) != VK_SUCCESS)
            throw std::runtime_error("ViewModelPass: buffer creation failed");
        if (allocationInfo.pMappedData == nullptr)
            throw std::runtime_error("ViewModelPass: buffer is not mapped");
        return allocationInfo.pMappedData;
    };

    void* vertexMapped = createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      "ViewModelVertices", vertexBuffer, vertexAllocation);
    std::memcpy(vertexMapped, vertices.data(), static_cast<std::size_t>(vertexBytes));

    void* indexMapped = createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                     "ViewModelIndices", indexBuffer, indexAllocation);
    std::memcpy(indexMapped, indices.data(), static_cast<std::size_t>(indexBytes));

    vmaFlushAllocation(device.getVmaAllocator(), vertexAllocation, 0, vertexBytes);
    vmaFlushAllocation(device.getVmaAllocator(), indexAllocation, 0, indexBytes);
}

void ViewModelPass::destroyMesh()
{
    if (vertexBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(device.getVmaAllocator(), vertexBuffer, vertexAllocation);
        vertexBuffer = VK_NULL_HANDLE;
        vertexAllocation = VK_NULL_HANDLE;
    }
    if (indexBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(device.getVmaAllocator(), indexBuffer, indexAllocation);
        indexBuffer = VK_NULL_HANDLE;
        indexAllocation = VK_NULL_HANDLE;
    }
    indexCount = 0;
}

void ViewModelPass::createPipeline()
{
    // 只需要 push constants：viewmodel 不采样任何贴图，也不读场景 UBO。
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(ViewModelPushConstantData);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 0;
    layoutInfo.pSetLayouts = nullptr;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("ViewModelPass: pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };

    // 没有深度附件：一个凸盒子开背面剔除后不会自遮挡，也就没有排序问题。
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState& blend = config.colorBlendAttachments[0];
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    config.colorBlendInfo.attachmentCount = 1;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, normal);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[2].offset = offsetof(Vertex, color);
    attributes[3].location = 3;
    attributes[3].binding = 0;
    attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[3].offset = offsetof(Vertex, uv);

    config.bindingDescriptions = { binding };
    config.attributeDescriptions = { attributes[0], attributes[1], attributes[2], attributes[3] };

    pipeline = std::make_unique<Pipeline>(device, TINY_SHADER_DIR "viewmodel.vert.spv",
                                          TINY_SHADER_DIR "viewmodel.frag.spv", config);
}

void ViewModelPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{

    if (pipeline == nullptr || vertexBuffer == VK_NULL_HANDLE || indexBuffer == VK_NULL_HANDLE)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;

    // 先把本帧要画的手持物收集出来（组件只读，命令录制里不再访问 registry）。
    std::vector<ViewModelPushConstantData> draws;
    {
        auto view = frameInfo.registry.view<ViewModelComponent>();
        for (auto entity : view)
        {
            const ViewModelComponent& component = view.get<ViewModelComponent>(entity);
            if (!component.visible)
                continue;

            scene::ViewModelSettings settings;
            settings.fovDegrees = component.fovDegrees;
            settings.offset = component.offset;
            settings.rotation = component.rotation;
            settings.scale = component.scale;
            settings.color = component.color;

            ViewModelPushConstantData push{};
            // 偏移矩阵已经是相机空间：再乘 viewMatrix 会把它当成世界坐标变换一次。
            push.modelView = scene::viewModelOffsetMatrix(settings);
            // 场景投影矩阵里 P11/P00 就是宽高比，不必额外传参数。
            const float projectionAspect =
                std::abs(frameInfo.projectionMatrix[1][1]) > 1e-6f &&
                        std::abs(frameInfo.projectionMatrix[0][0]) > 1e-6f
                    ? std::abs(frameInfo.projectionMatrix[1][1] / frameInfo.projectionMatrix[0][0])
                    : 16.0f / 9.0f;
            push.projection = scene::viewModelProjection(settings.fovDegrees, projectionAspect,
                                                         settings.nearPlane, settings.farPlane);
            push.baseColor = glm::vec4(settings.color, 1.0f);

            // 平行光方向从世界转到视图空间（视图矩阵的行是 u/v/w）。
            const glm::vec3 lightWorld = glm::vec3(frameInfo.settings.directionalLightDir);
            glm::vec3 lightView = glm::mat3(frameInfo.viewMatrix) * (-lightWorld);
            if (glm::length(lightView) < 1e-5f)
                lightView = glm::vec3(0.0f, 0.0f, 1.0f);
            push.lightDirView = glm::vec4(glm::normalize(lightView), 1.0f);

            draws.push_back(push);
        }
    }

    if (draws.empty())
        return;

    auto& pass = graph.addGraphicsPass("ViewModel");
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    pass.execute([this, draws = std::move(draws), &frameInfo](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);

        VkBuffer vertexBuffers[] = { vertexBuffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        for (const ViewModelPushConstantData& push : draws)
        {
            vkCmdPushConstants(cmd, pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(ViewModelPushConstantData), &push);
            vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
        }

        frameInfo.telemetry.drawCalls += static_cast<uint32_t>(draws.size());
        frameInfo.telemetry.triangles += static_cast<uint32_t>(draws.size()) * (indexCount / 3u);
        frameInfo.telemetry.vertices += static_cast<uint32_t>(draws.size()) * 24u;

        if (!loggedReady)
        {
            const float loggedFov = glm::degrees(2.0f * std::atan(1.0f / std::abs(draws.front().projection[1][1])));
            const float loggedAspect = std::abs(draws.front().projection[1][1] / draws.front().projection[0][0]);
            std::cout << "[ViewModel] 通道就绪 | fov=" << loggedFov << " aspect=" << loggedAspect
                      << " draws=" << draws.size() << " depth=0.01..10\n";
            loggedReady = true;
        }
    });
}