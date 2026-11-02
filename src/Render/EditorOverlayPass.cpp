#include "EditorOverlayPass.h"
#include "AssetPaths.h"

#include "Scene/Components.h"

#include <glm/glm.hpp>
#include <stdexcept>
#include <cstring>

namespace
{
// 世界 AABB 的 12 条棱（顶点索引对）。
// 顶点顺序与 buildBoxVertices() 里的 corners 数组一致。
constexpr int kBoxEdges[12][2] = {
    {0,1},{2,3},{4,5},{6,7},   // 沿 X
    {0,2},{1,3},{4,6},{5,7},   // 沿 Y
    {0,4},{1,5},{2,6},{3,7},   // 沿 Z
};
} // namespace

EditorOverlayPass::EditorOverlayPass(Device& device, RenderGraph& graph, RenderResources& resources)
    : device(device), renderGraph(graph), resources(resources)
{
}

EditorOverlayPass::~EditorOverlayPass()
{
    pipeline.reset();
    if (pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (vertexBuffers[frame] != VK_NULL_HANDLE)
            vmaDestroyBuffer(device.getVmaAllocator(), vertexBuffers[frame], vertexAllocations[frame]);
    }
}

void EditorOverlayPass::createPipeline(VkFormat colorFormat, VkFormat depthFormat)
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(Push);

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("EditorOverlayPass: pipeline layout creation failed");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, 1, 1);
    config.pipelineLayout = pipelineLayout;

    // 线列表：每条边两个端点
    config.inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    config.inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    // 不做背面剔除：线没有正反面
    config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    config.rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
    config.rasterizationInfo.lineWidth = 1.0f;

    // 顶点只有一个 vec3 位置
    config.bindingDescriptions = {
        { 0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX }
    };
    config.attributeDescriptions = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }
    };

    // 深度测试开启、深度写关闭：
    //   开启 -> 被场景几何挡住的边不画（这就是需求）
    //   关闭写 -> 不能污染 G-Buffer 的深度，后面的 Pass 还要用它
    config.depthStencilInfo.depthTestEnable = VK_TRUE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.depthStencilInfo.depthBoundsTestEnable = VK_FALSE;
    config.depthStencilInfo.stencilTestEnable = VK_FALSE;

    config.colorAttachmentFormats = { colorFormat };
    config.depthAttachmentFormat = depthFormat;

    pipeline = std::make_unique<Pipeline>(device,
        std::string(TINY_SHADER_DIR) + "editor_overlay.vert.spv",
        std::string(TINY_SHADER_DIR) + "editor_overlay.frag.spv",
        config);
}

void EditorOverlayPass::init()
{
    const VkFormat colorFormat = resources.editorViewportFormatHint;
    const VkFormat depthFormat = resources.depthFormatHint;

    if (colorFormat == VK_FORMAT_UNDEFINED || depthFormat == VK_FORMAT_UNDEFINED)
        return;   // 编辑器未启用：不需要这条管线

    createPipeline(colorFormat, depthFormat);

    constexpr VkDeviceSize bufferSize = sizeof(glm::vec3) * 24;
    static const char* const names[kFramesInFlight] = { "EditorOverlayVtx.Frame0", "EditorOverlayVtx.Frame1" };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocInfo.flags |= tiny_vmaNameFlag();
        allocInfo.pUserData = const_cast<char*>(names[frame]);

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device.getVmaAllocator(), &bufferInfo, &allocInfo,
                            &vertexBuffers[frame], &vertexAllocations[frame], &info) != VK_SUCCESS)
            throw std::runtime_error("EditorOverlayPass: vertex buffer creation failed");

        vertexMapped[frame] = info.pMappedData;
    }
}

void EditorOverlayPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    // 非编辑器模式：什么都不注册（和 UiCompositePass 一样）
    if (!resources.editorViewportEnabled || !pipeline)
        return;

    const uint32_t frame = frameInfo.frameInFlightIndex;

    // 只有选中了带包围盒的实体才需要画
    const entt::entity selected = frameInfo.editorSelection;
    const WorldBoundsComponent* worldBounds = nullptr;
    if (selected != entt::null && frameInfo.registry.valid(selected))
        worldBounds = frameInfo.registry.try_get<WorldBoundsComponent>(selected);

    // 选中框优先用**局部包围盒 × 世界矩阵**（OBB）：
    // 物体旋转/缩放时框必须跟着一起转，不能用一个世界 AABB 把旋转"包大"。
    // 没有局部包围盒时才退回世界 AABB。
    const BoundsComponent* localBounds = nullptr;
    const WorldTransformComponent* worldTransform = nullptr;
    if (selected != entt::null && frameInfo.registry.valid(selected))
    {
        localBounds = frameInfo.registry.try_get<BoundsComponent>(selected);
        worldTransform = frameInfo.registry.try_get<WorldTransformComponent>(selected);
    }

    glm::vec3 corners[8];
    if (localBounds != nullptr && localBounds->getLocal().valid && worldTransform != nullptr)
    {
        const AABB& box = localBounds->getLocal();
        const glm::vec3 localCorners[8] = {
            { box.min.x, box.min.y, box.min.z }, { box.max.x, box.min.y, box.min.z },
            { box.min.x, box.max.y, box.min.z }, { box.max.x, box.max.y, box.min.z },
            { box.min.x, box.min.y, box.max.z }, { box.max.x, box.min.y, box.max.z },
            { box.min.x, box.max.y, box.max.z }, { box.max.x, box.max.y, box.max.z },
        };
        for (int i = 0; i < 8; ++i)
            corners[i] = glm::vec3(worldTransform->matrix * glm::vec4(localCorners[i], 1.0f));
    }
    else if (worldBounds != nullptr && worldBounds->world.valid)
    {
        const AABB& box = worldBounds->world;
        const glm::vec3 worldCorners[8] = {
            { box.min.x, box.min.y, box.min.z }, { box.max.x, box.min.y, box.min.z },
            { box.min.x, box.max.y, box.min.z }, { box.max.x, box.max.y, box.min.z },
            { box.min.x, box.min.y, box.max.z }, { box.max.x, box.min.y, box.max.z },
            { box.min.x, box.max.y, box.max.z }, { box.max.x, box.max.y, box.max.z },
        };
        for (int i = 0; i < 8; ++i)
            corners[i] = worldCorners[i];
    }
    else
    {
        return;
    }

    // 12 条棱 -> 24 个世界空间顶点
    glm::vec3 vertices[24];
    for (int e = 0; e < 12; ++e)
    {
        vertices[e * 2 + 0] = corners[kBoxEdges[e][0]];
        vertices[e * 2 + 1] = corners[kBoxEdges[e][1]];
    }

    std::memcpy(vertexMapped[frame], vertices, sizeof(vertices));
    vmaFlushAllocation(device.getVmaAllocator(), vertexAllocations[frame], 0, sizeof(vertices));

    auto& pass = graph.addGraphicsPass("EditorOverlay");

    // 颜色目标：PresentPass 刚写好的视口纹理，LOAD 保住画面
    pass.writeColor(resources.editorViewport[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    // 深度附件：复用 G-Buffer 深度。LOAD + STORE 表示"只读不改"，
    // 管线里 depthWriteEnable = VK_FALSE 保证内容真的不被改。
    pass.writeDepth(resources.gbDepth[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    Push push{};
    push.viewProjection = frameInfo.projectionMatrix * frameInfo.viewMatrix;
    push.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

    const VkBuffer vbo = vertexBuffers[frame];
    pass.execute([this, push, vbo](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
        vkCmdPushConstants(cmd, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdDraw(cmd, 24, 1, 0, 0);
    });
}
