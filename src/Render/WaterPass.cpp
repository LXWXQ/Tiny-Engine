#include "WaterPass.h"

#include "Scene/Components.h"
#include "AssetPaths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

static_assert(sizeof(WaterPushConstantData) == 32, "water push constant must stay 32 bytes");

namespace
{
constexpr float kTwoPi = 6.28318530718f;

VkDeviceSize bufferBytes(VkDeviceSize elementSize, uint32_t count)
{
    return elementSize * static_cast<VkDeviceSize>(count);
}
}

WaterPass::WaterPass(Device& device, RenderGraph& graph, RenderResources& resources,
                     VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
                     VkFormat depthFormat)
    : device(device), renderGraph(graph), resources(resources),
      descriptorManager(descriptorManager), globalSetLayout(globalSetLayout),
      depthFormat(depthFormat)
{
}

WaterPass::~WaterPass()
{
    waterPipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    destroyBuffers();
}

void WaterPass::init()
{
    createBuffers();
    createPipeline();

    std::cout << "[Water] initialized | rings=" << kRingCount
              << " segments=" << kSegmentCount
              << " vertices=" << vertexCount
              << " indices=" << indexCount
              << " radius=" << kFarRadius << "m\n";
}

void WaterPass::createBuffers()
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    vertices.reserve(1u + kRingCount * kSegmentCount);
    indices.reserve(static_cast<size_t>(kSegmentCount) * 3u
                    + static_cast<size_t>(kRingCount - 1u) * kSegmentCount * 6u);

    // 中心顶点：扇形的公共端点。
    Vertex center{};
    center.position = glm::vec3(0.0f);
    center.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    center.color = glm::vec3(0.0f);
    center.uv = glm::vec2(0.5f, 0.5f);
    vertices.push_back(center);

    // 同心圆环。半径二次增长：近处密（顶点位移看得清），远处疏（省顶点）。
    for (uint32_t ring = 1; ring <= kRingCount; ++ring)
    {
        const float radius = 1.2f * static_cast<float>(ring)
                           + 0.15f * static_cast<float>(ring * ring);
        for (uint32_t segment = 0; segment < kSegmentCount; ++segment)
        {
            const float angle = kTwoPi * static_cast<float>(segment)
                              / static_cast<float>(kSegmentCount);
            Vertex vertex{};
            vertex.position = glm::vec3(std::cos(angle) * radius, 0.0f,
                                        std::sin(angle) * radius);
            vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            vertex.color = glm::vec3(0.0f);
            vertex.uv = glm::vec2(radius / kFarRadius,
                                  static_cast<float>(segment) / static_cast<float>(kSegmentCount));
            vertices.push_back(vertex);
        }
    }

    // 中心扇形。
    for (uint32_t segment = 0; segment < kSegmentCount; ++segment)
    {
        const uint32_t current = 1u + segment;
        const uint32_t next = 1u + (segment + 1u) % kSegmentCount;
        indices.push_back(0u);
        indices.push_back(current);
        indices.push_back(next);
    }

    // 环与环之间的四边形。绕序无关紧要（管线关闭了背面剔除），
    // 但保持每格两个三角形、共用对角线，避免远处出现裂缝。
    for (uint32_t ring = 1; ring < kRingCount; ++ring)
    {
        const uint32_t inner = 1u + (ring - 1u) * kSegmentCount;
        const uint32_t outer = inner + kSegmentCount;
        for (uint32_t segment = 0; segment < kSegmentCount; ++segment)
        {
            const uint32_t next = (segment + 1u) % kSegmentCount;
            const uint32_t a = inner + segment;
            const uint32_t b = inner + next;
            const uint32_t c = outer + segment;
            const uint32_t d = outer + next;
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(d);
            indices.push_back(c);
        }
    }

    vertexCount = static_cast<uint32_t>(vertices.size());
    indexCount = static_cast<uint32_t>(indices.size());

    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferBytes(sizeof(Vertex), vertexCount);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>("WaterVertices");

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &vertexBuffer, &vertexAllocation, &info) != VK_SUCCESS)
            throw std::runtime_error("WaterPass: vertex buffer creation failed");
        vertexMapped = info.pMappedData;
        if (vertexMapped == nullptr)
            throw std::runtime_error("WaterPass: vertex buffer is not mapped");

        std::memcpy(vertexMapped, vertices.data(), bufferInfo.size);
        vmaFlushAllocation(device.getVmaAllocator(), vertexAllocation, 0, bufferInfo.size);
    }

    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferBytes(sizeof(uint32_t), indexCount);
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT | tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>("WaterIndices");

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &indexBuffer, &indexAllocation, &info) != VK_SUCCESS)
            throw std::runtime_error("WaterPass: index buffer creation failed");
        indexMapped = info.pMappedData;
        if (indexMapped == nullptr)
            throw std::runtime_error("WaterPass: index buffer is not mapped");

        std::memcpy(indexMapped, indices.data(), bufferInfo.size);
        vmaFlushAllocation(device.getVmaAllocator(), indexAllocation, 0, bufferInfo.size);
    }
}

void WaterPass::destroyBuffers()
{
    if (vertexBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(device.getVmaAllocator(), vertexBuffer, vertexAllocation);
    vertexBuffer = VK_NULL_HANDLE;
    vertexAllocation = VK_NULL_HANDLE;
    vertexMapped = nullptr;

    if (indexBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(device.getVmaAllocator(), indexBuffer, indexAllocation);
    indexBuffer = VK_NULL_HANDLE;
    indexAllocation = VK_NULL_HANDLE;
    indexMapped = nullptr;
}

void WaterPass::createPipeline()
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(WaterPushConstantData);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &globalSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("WaterPass: pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.depthAttachmentFormat = depthFormat;

    // 标准 over 混合：水色按 alpha 覆盖到已有 HDR 颜色上，alpha 通道保持 1。
    VkPipelineColorBlendAttachmentState& blend = config.colorBlendAttachments[0];
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    config.colorBlendInfo.attachmentCount = 1;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    // 水不写深度，但必须按不透明场景的深度做遮挡测试。水面与岸边地面共面
    // 时用 LESS_OR_EQUAL，避免因为浮点误差把整片水裁掉。
    config.depthStencilInfo.depthTestEnable = VK_TRUE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;

    config.bindingDescriptions = { Vertex::getBindingDescription() };
    // 水面 shader 只消费 position；描述符里仍然列全部 4 个属性会让验证层
    // 报 "Vertex attribute at location 1/2/3 not consumed"。只声明实际用到的那个。
    config.attributeDescriptions = { Vertex::getAttributeDescriptions()[0] };

    waterPipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "water.vert.spv", TINY_SHADER_DIR "water.frag.spv", config);
}

void WaterPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    if (!frameInfo.settings.waterEnabled)
    {
        if (!loggedDisabled)
        {
            std::cout << "[Water] disabled\n";
            loggedDisabled = true;
        }
        return;
    }

    // 没有地形就没有海平面：Sponza / 空场景里铺一张 2m 高的全球水面毫无意义。
    // 地形组件和体素 chunk 都算"有地面"，两者任一存在就启用。
    const bool hasTerrain =
        frameInfo.registry.view<TerrainComponent>().size() > 0 ||
        frameInfo.registry.view<VoxelChunkComponent>().size() > 0;
    if (!hasTerrain)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;
    if (frame >= kFramesInFlight || frame >= resources.hdrScene.size() ||
        frame >= resources.gbDepth.size() || vertexBuffer == VK_NULL_HANDLE ||
        indexBuffer == VK_NULL_HANDLE || waterPipeline == nullptr)
        return;

    auto& pass = graph.addGraphicsPass("Water");
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_LOAD);
    pass.writeDepth(resources.gbDepth[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    pass.execute([this, &frameInfo, frame](VkCommandBuffer cmd)
    {
        if (indexCount == 0u || waterPipeline == nullptr)
            return;

        timeSeconds += std::min(std::max(frameInfo.frameTime, 0.0f), 0.1f);

        glm::vec3 cameraPosition(0.0f);
        if (frameInfo.cameraTransform != nullptr)
            cameraPosition = frameInfo.cameraTransform->getTranslation();
        else
            cameraPosition = glm::vec3(glm::inverse(frameInfo.viewMatrix)[3]);

        const EngineSettings& settings = frameInfo.settings;

        waterPipeline->bind(cmd);

        VkDescriptorSet globalSet = frameInfo.descriptorManager->getGlobalSet(frame);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &globalSet, 0, nullptr);

        WaterPushConstantData push{};
        push.params = glm::vec4(timeSeconds, settings.waterLevel,
                                settings.waterWaveStrength, settings.waterWaveSpeed);
        push.params2 = glm::vec4(kFarRadius, 1.0f, 0.0f, 0.0f);
        vkCmdPushConstants(cmd, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(WaterPushConstantData), &push);

        VkBuffer vertexBuffers[] = { vertexBuffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);

        frameInfo.telemetry.drawCalls += 1;
        frameInfo.telemetry.triangles += indexCount / 3u;
        frameInfo.telemetry.vertices += vertexCount;
        frameInfo.telemetry.descriptorBinds += 1;

        if (!loggedFirstDraw)
        {
            std::cout << "[Water] first draw | level=" << settings.waterLevel
                      << " waveStrength=" << settings.waterWaveStrength
                      << " waveSpeed=" << settings.waterWaveSpeed
                      << " cameraY=" << cameraPosition.y << "\n";
            loggedFirstDraw = true;
        }
    });
}
