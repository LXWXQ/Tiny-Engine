#include "Render/ShadowPass.h"

#include "AssetPaths.h"
#include "Core/FrameInFlight.h"
#include "Core/FrameInfo.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/TerrainMeshArena.h"
#include "Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
// 级联划分：对数与线性各占一半。纯对数在近处过度精细、远处稀疏得能看见锯齿；
// 纯线性则是反过来的问题。0.5 是工程上常用的折中。
constexpr float kSplitLambda = 0.5f;
// 超过这个距离不再投影（阴影贴图分辨率固定，更远的级联只会糊成一团）。
constexpr float kMaxShadowDistance = 90.0f;

struct CameraIntrinsics
{
    float nearZ = 0.1f;
    float farZ = 100.0f;
    float tanX = 1.0f;
    float tanY = 1.0f;
};

// 从投影矩阵反推 near / far / 半角正切。工程用 GLM_FORCE_DEPTH_ZERO_TO_ONE，
// 所以这里按 [0,1] 深度约定取：near = P[3][2] / P[2][2]，far = P[3][2] / (P[2][2] + 1)。
CameraIntrinsics extractIntrinsics(const glm::mat4& projection)
{
    CameraIntrinsics result{};
    const float p22 = projection[2][2];
    const float p32 = projection[3][2];
    if (std::abs(p22) > 1.0e-6f)
        result.nearZ = p32 / p22;
    if (std::abs(p22 + 1.0f) > 1.0e-6f)
        result.farZ = p32 / (p22 + 1.0f);
    if (result.nearZ <= 0.0f || !std::isfinite(result.nearZ))
        result.nearZ = 0.1f;
    if (result.farZ <= result.nearZ || !std::isfinite(result.farZ))
        result.farZ = 100.0f;
    result.tanX = std::abs(projection[0][0]) > 1.0e-6f ? 1.0f / projection[0][0] : 1.0f;
    result.tanY = std::abs(projection[1][1]) > 1.0e-6f ? 1.0f / projection[1][1] : 1.0f;
    return result;
}

// 视空间里一段 [nearZ, farZ] 的视锥 8 角点（返回世界坐标）。
std::array<glm::vec3, 8> frustumCorners(const glm::mat4& inverseView, float nearZ, float farZ,
                                       float tanX, float tanY)
{
    std::array<glm::vec3, 8> corners{};
    const float depth[2] = { nearZ, farZ };
    int index = 0;
    for (int plane = 0; plane < 2; ++plane)
    {
        const float z = depth[plane];
        for (int y = -1; y <= 1; y += 2)
        {
            for (int x = -1; x <= 1; x += 2)
            {
                const glm::vec3 viewPoint(tanX * z * static_cast<float>(x),
                                          tanY * z * static_cast<float>(y), -z);
                corners[index++] = glm::vec3(inverseView * glm::vec4(viewPoint, 1.0f));
            }
        }
    }
    return corners;
}

// 单级联：把视锥角点包成一个球的包围盒，用正交光相机覆盖它。
ShadowPass::Cascade buildCascade(const std::array<glm::vec3, 8>& corners, float splitDistance,
                                const glm::vec3& lightDirection, uint32_t resolution)
{
    glm::vec3 center(0.0f);
    for (const glm::vec3& corner : corners)
        center += corner;
    center /= static_cast<float>(corners.size());

    float radius = 0.0f;
    for (const glm::vec3& corner : corners)
        radius = std::max(radius, glm::length(corner - center));
    radius = std::max(radius, 0.5f);

    glm::vec3 direction = glm::normalize(lightDirection);
    if (glm::length(direction) < 1.0e-4f)
        direction = glm::vec3(0.0f, -1.0f, 0.0f);

    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f)
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(direction, up));
    const glm::vec3 lightUp = glm::cross(right, direction);

    // 纹素对齐：光空间里把包围球中心吸到纹素栅格上，否则相机一移动阴影边缘就会「爬行」。
    const float texelSize = (2.0f * radius) / static_cast<float>(resolution);
    const glm::vec3 centerLight = glm::vec3(glm::dot(right, center), glm::dot(lightUp, center), 0.0f);
    const float snappedX = std::round(centerLight.x / texelSize) * texelSize;
    const float snappedY = std::round(centerLight.y / texelSize) * texelSize;
    center += right * (snappedX - centerLight.x);
    center += lightUp * (snappedY - centerLight.y);

    const glm::vec3 eye = center - direction * (radius * 2.0f);
    const glm::mat4 lightView = glm::lookAt(eye, center, lightUp);
    // 手写正交投影，不依赖 glm::ortho 在 ZERO_TO_ONE + 右手视图空间下的参数约定：
    // 光相机在 center 后方 2r 处，要覆盖「距相机 r .. 3r」这一段，
    // 所以深度按 dist = -z_view 线性映射到 [0,1]：z_ndc = (-z_view - r) / (2r)。
    // x/y 直接按包围球半径归一化（对称框，左右手性不影响覆盖范围）。
    glm::mat4 lightProjection(1.0f);
    lightProjection[0][0] = 1.0f / radius;
    lightProjection[1][1] = 1.0f / radius;
    lightProjection[2][2] = -1.0f / (2.0f * radius);
    lightProjection[3][2] = -0.5f;

    ShadowPass::Cascade cascade{};
    cascade.viewProjection = lightProjection * lightView;
    cascade.splitDistance = splitDistance;
    return cascade;
}

std::array<ShadowPass::Cascade, ShadowPass::kCascadeCount> buildCascades(
    const glm::mat4& cameraView, const CameraIntrinsics& intrinsics,
    const glm::vec3& lightDirection, uint32_t resolution)
{
    std::array<ShadowPass::Cascade, ShadowPass::kCascadeCount> result{};
    const glm::mat4 inverseView = glm::inverse(cameraView);
    const float farUsed = std::min(intrinsics.farZ, kMaxShadowDistance);
    const float nearZ = intrinsics.nearZ;

    float previous = nearZ;
    for (uint32_t i = 0; i < ShadowPass::kCascadeCount; ++i)
    {
        const float fraction = static_cast<float>(i + 1) / static_cast<float>(ShadowPass::kCascadeCount);
        const float linearSplit = nearZ + (farUsed - nearZ) * fraction;
        const float logSplit = nearZ * std::pow(farUsed / std::max(nearZ, 1.0e-4f), fraction);
        const float split = kSplitLambda * logSplit + (1.0f - kSplitLambda) * linearSplit;

        const auto corners = frustumCorners(inverseView, previous, split, intrinsics.tanX, intrinsics.tanY);
        result[i] = buildCascade(corners, split, lightDirection, resolution);
        previous = split;
    }
    return result;
}
}   // namespace

ShadowPass::ShadowPass(Device& device, RenderGraph& graph, TerrainMeshArena* arena)
    : vulkanDevice(device), renderGraph(graph), terrainMeshArena(arena)
{
}

ShadowPass::~ShadowPass()
{
    // 渲染管线（unique_ptr<ShadowPass>）在 FirstApp 里先于 Device 析构，
    // 所以这里直接释放是安全的；忘了释放会触发 VMA 的
    // "Some allocations were not freed before destruction of this memory block!"。
    pipeline.reset();
    destroyResources();
    if (shadowSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vulkanDevice.getDevice(), shadowSampler, nullptr);
        shadowSampler = VK_NULL_HANDLE;
    }
    if (pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vulkanDevice.getDevice(), pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
}

void ShadowPass::init()
{
    createResources();
    createPipeline();

    std::cout << "[Shadow] ready | cascades=" << kCascadeCount
              << " resolution=" << kResolution << "x" << kResolution
              << " (CPU built geometry, manual barriers)" << std::endl;
}

void ShadowPass::createResources()
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        Frame& slot = frames[frame];
        if (slot.image != VK_NULL_HANDLE)
            continue;

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = { kResolution, kResolution, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = kCascadeCount;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(vulkanDevice.getVmaAllocator(), &imageInfo, &allocInfo,
                           &slot.image, &slot.allocation, nullptr) != VK_SUCCESS)
            throw std::runtime_error("ShadowPass: 阴影图创建失败");

        VkImageViewCreateInfo arrayInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        arrayInfo.image = slot.image;
        arrayInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        arrayInfo.format = VK_FORMAT_D32_SFLOAT;
        arrayInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kCascadeCount };
        if (vkCreateImageView(vulkanDevice.getDevice(), &arrayInfo, nullptr, &slot.arrayView) != VK_SUCCESS)
            throw std::runtime_error("ShadowPass: 阴影图层视图创建失败");

        for (uint32_t layer = 0; layer < kCascadeCount; ++layer)
        {
            VkImageViewCreateInfo layerInfo = arrayInfo;
            layerInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            layerInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1 };
            if (vkCreateImageView(vulkanDevice.getDevice(), &layerInfo, nullptr,
                                  &slot.layerViews[layer]) != VK_SUCCESS)
                throw std::runtime_error("ShadowPass: 阴影单层视图创建失败");
        }
    }

    if (shadowSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        // 边界外按「最远」处理：采样落到阴影图外面就当作没有遮挡，避免远处出现黑边。
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(vulkanDevice.getDevice(), &samplerInfo, nullptr, &shadowSampler) != VK_SUCCESS)
            throw std::runtime_error("ShadowPass: 阴影采样器创建失败");
    }
}

void ShadowPass::destroyResources()
{
    for (Frame& slot : frames)
    {
        for (VkImageView view : slot.layerViews)
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(vulkanDevice.getDevice(), view, nullptr);
        }
        slot.layerViews.fill(VK_NULL_HANDLE);
        if (slot.arrayView != VK_NULL_HANDLE)
            vkDestroyImageView(vulkanDevice.getDevice(), slot.arrayView, nullptr);
        slot.arrayView = VK_NULL_HANDLE;
        if (slot.image != VK_NULL_HANDLE)
            vmaDestroyImage(vulkanDevice.getVmaAllocator(), slot.image, slot.allocation);
        slot.image = VK_NULL_HANDLE;
        slot.allocation = VK_NULL_HANDLE;
    }
}

void ShadowPass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    range.offset = 0;
    range.size = sizeof(glm::mat4) * 2;   // model + lightViewProjection

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 0;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("ShadowPass: 管线布局创建失败");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, kResolution, kResolution);
    config.pipelineLayout = pipelineLayout;
    // 只有深度：动态渲染下把颜色附件清空。
    config.colorAttachmentFormats.clear();
    config.colorBlendAttachments.clear();
    config.colorBlendInfo.attachmentCount = 0;
    config.colorBlendInfo.pAttachments = nullptr;
    config.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.depthStencilInfo.depthWriteEnable = VK_TRUE;
    // 阴影图的经典配方：深度偏移压住自遮挡，背面剔除用无（薄壁网格不会漏光）。
    // 光栅化阶段的深度偏移在掠射角下会把整个地形往外推几十厘米，等于把接触阴影全抹掉。
    // 偏移改由采样端负责（法线偏移 + 斜率 bias），这里必须关闭。
    config.rasterizationInfo.depthBiasEnable = VK_FALSE;
    config.rasterizationInfo.depthBiasConstantFactor = 0.0f;
    config.rasterizationInfo.depthBiasSlopeFactor = 0.0f;
    config.rasterizationInfo.depthBiasClamp = 0.0f;
    // 剔除背面，只让朝向光源的那一侧进阴影图。经典的"画背面"配方（剔除正面）
    // 能把自遮蔽从几何上消掉，但本工程的地形是单面网格 —— FRONT 剔除会让整个
    // 地形从阴影图里消失，所以这里选 BACK，自遮蔽交给采样端的法线偏移 + 斜率 bias。
    config.rasterizationInfo.cullMode = VK_CULL_MODE_BACK_BIT;
    // 阴影只吃位置：把顶点输入裁到 location 0，否则验证层会对 color/uv/normal
    // 三个"没被顶点着色器消费"的属性各报一条 UNASSIGNED-CoreValidation-Shader-OutputNotConsumed。
    config.bindingDescriptions.clear();
    config.bindingDescriptions.push_back(Vertex::getBindingDescription());
    config.attributeDescriptions.clear();
    config.attributeDescriptions.push_back(VkVertexInputAttributeDescription{
        0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, position)) });

    pipeline = std::make_unique<Pipeline>(vulkanDevice,
        TINY_SHADER_DIR "shadow.vert.spv", TINY_SHADER_DIR "shadow.frag.spv", config);
}

void ShadowPass::updateCascades(const glm::mat4& cameraView, const glm::mat4& cameraProjection,
                               const glm::vec3& lightDirection)
{
    const CameraIntrinsics intrinsics = extractIntrinsics(cameraProjection);
    cascades = buildCascades(cameraView, intrinsics, lightDirection, kResolution);

    cachedLightDirection = lightDirection;
    cascadesDirty = false;
}

VkImageView ShadowPass::getShadowView(uint32_t frameInFlightIndex) const
{
    if (frameInFlightIndex >= kFramesInFlight)
        return VK_NULL_HANDLE;
    return frames[frameInFlightIndex].arrayView;
}

void ShadowPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    auto& pass = graph.addGraphicsPass("Shadow");
    // 零附件 Pass：渲染图只保证顺序，阴影图由本类自己管（见类注释）。
    pass.execute([this, &frameInfo](VkCommandBuffer cmd)
    {
        stats = Stats{};

        if (!layoutReady)
        {
            // 一次性：把每个帧槽位的 3 层都从 UNDEFINED 转到 SHADER_READ_ONLY。
            // 关掉阴影时也要做 —— 描述符一直绑着这张图，布局错会被验证层抓住。
            std::vector<VkImageMemoryBarrier> barriers;
            barriers.reserve(kFramesInFlight * kCascadeCount);
            for (const Frame& slot : frames)
            {
                if (slot.image == VK_NULL_HANDLE)
                    continue;
                for (uint32_t layer = 0; layer < kCascadeCount; ++layer)
                {
                    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = slot.image;
                    barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1 };
                    barrier.srcAccessMask = 0;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barriers.push_back(barrier);
                }
            }
            if (!barriers.empty())
            {
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr,
                                     static_cast<uint32_t>(barriers.size()), barriers.data());
            }
            layoutReady = true;
        }

        if (!enabled || pipeline == nullptr)
            return;

        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
            recordCascade(cmd, cascade, frameInfo.frameInFlightIndex, frameInfo);

        if (!reported)
        {
            std::cout << "[Shadow] first frame | cascades=" << kCascadeCount
                      << " entities=" << stats.drewEntities
                      << " terrainChunks=" << stats.drewTerrainChunks
                      << " drawCalls=" << stats.drawCalls << std::endl;
            reported = true;
        }
    });
}

void ShadowPass::recordCascade(VkCommandBuffer cmd, uint32_t cascadeIndex, uint32_t frameIndex,
                               FrameInfo& frameInfo)
{
    Frame& slot = frames[frameIndex];
    const Cascade& cascade = cascades[cascadeIndex];

    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slot.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, cascadeIndex, 1 };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkRenderingAttachmentInfo depthAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depthAttachment.imageView = slot.layerViews[cascadeIndex];
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderingInfo.renderArea.offset = { 0, 0 };
    renderingInfo.renderArea.extent = { kResolution, kResolution };
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 0;
    renderingInfo.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(kResolution),
                         static_cast<float>(kResolution), 0.0f, 1.0f };
    VkRect2D scissor{ { 0, 0 }, { kResolution, kResolution } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    pipeline->bind(cmd);

    entt::registry& registry = frameInfo.registry;
    struct PushData { glm::mat4 model; glm::mat4 lightViewProjection; };

    // ---- 地形 chunk：几何在 arena 里，subMesh 的 firstIndex 是模型内偏移 ----
    const bool arenaReady = terrainMeshArena != nullptr &&
                            terrainMeshArena->getVertexBuffer() != VK_NULL_HANDLE &&
                            terrainMeshArena->getIndexBuffer() != VK_NULL_HANDLE;
    if (arenaReady)
    {
        bool buffersBound = false;
        for (auto [entity, mesh, terrain] : registry.view<MeshComponent, TerrainComponent>().each())
        {
            if (terrain.arenaVertexBase == 0xFFFFFFFFu || terrain.arenaIndexBase == 0xFFFFFFFFu)
                continue;
            const auto* world = registry.try_get<WorldTransformComponent>(entity);
            if (world == nullptr || !world->valid || !mesh.model)
                continue;
            // 阴影投射者不能按相机视锥裁剪：视野外的高处物体同样会把阴影投进画面。
            // 所以这里不做剔除，靠深度测试和 2048² 分辨率兜住开销。
            if (!buffersBound)
            {
                VkBuffer vertexBuffers[] = { terrainMeshArena->getVertexBuffer() };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(cmd, terrainMeshArena->getIndexBuffer(), 0,
                                     VK_INDEX_TYPE_UINT32);
                buffersBound = true;
            }

            PushData push{ world->matrix, cascade.viewProjection };
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(PushData), &push);

            for (const SubMesh& subMesh : mesh.model->getSubMeshes())
            {
                // 地形有 LOD 链；阴影只画 LOD 0（2048² 下细节差不出来，且省一半三角形）。
                if (!subMesh.isTerrainChunk || subMesh.terrainLod != 0u || subMesh.indexCount == 0u)
                    continue;
                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1,
                                 subMesh.firstIndex + terrain.arenaIndexBase,
                                 static_cast<int32_t>(terrain.arenaVertexBase), 0);
                stats.drawCalls += 1;
                stats.triangles += subMesh.indexCount / 3;
            }
            stats.drewTerrainChunks += 1;
        }
    }

    // ---- 普通网格：每个模型绑一次缓冲 ----
    for (auto [entity, mesh] : registry.view<MeshComponent>().each())
    {
        if (!mesh.model || registry.all_of<TerrainComponent>(entity))
            continue;
        if (const auto* tag = registry.try_get<RenderTagComponent>(entity);
            tag != nullptr && tag->isSkybox)
            continue;
        const auto* world = registry.try_get<WorldTransformComponent>(entity);
        if (world == nullptr || !world->valid)
            continue;
        // 同上：阴影 pass 不做视锥剔除。

        // V-04：体素几何在共享 arena 里，Model 的 GPU 缓冲已释放。
        const auto* voxel = registry.try_get<VoxelChunkComponent>(entity);
        const bool voxelArenaBacked = voxel != nullptr && arenaReady &&
            voxel->arenaIndexBase != 0xFFFFFFFFu;
        if (voxelArenaBacked)
        {
            VkBuffer vertexBuffers[] = { terrainMeshArena->getVertexBuffer() };
            VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(cmd, terrainMeshArena->getIndexBuffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
        }
        else
        {
            mesh.model->bind(cmd);
        }
        PushData push{ world->matrix, cascade.viewProjection };
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(PushData), &push);

        for (const SubMesh& subMesh : mesh.model->getSubMeshes())
        {
            if (subMesh.indexCount == 0u)
                continue;
            if (voxelArenaBacked)
            {
                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1,
                                 subMesh.firstIndex + voxel->arenaIndexBase,
                                 voxel->arenaVertexBase, 0);
            }
            else
            {
                mesh.model->drawSubMesh(cmd, subMesh);
            }
            stats.drawCalls += 1;
            stats.triangles += subMesh.indexCount / 3;
        }
        stats.drewEntities += 1;
    }

    vkCmdEndRendering(cmd);

    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slot.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, cascadeIndex, 1 };
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    }
}

bool ShadowPass::cascadeVisible(const glm::mat4& lightViewProjection, const glm::vec3& minimum,
                                const glm::vec3& maximum)
{
    // 光空间正交裁剪体 vs 世界 AABB。
    //
    // 早先的版本拿"包围盒中心 + 包围球半径"做单点判据，对 Sponza 这种
    // 30x12x18 的大模型会把整栋楼剔掉：它的中心落在近级联之外，但有一半
    // 体积其实在框里。改成把 8 个角点投影到光空间 NDC，取变换后的包围盒
    // 与 [-1,1]x[-1,1]x[0,1] 求交 —— 对 AABB 是保守且正确的判据。
    glm::vec3 ndcMin(std::numeric_limits<float>::max());
    glm::vec3 ndcMax(-std::numeric_limits<float>::max());
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 world((corner & 1) ? maximum.x : minimum.x,
                              (corner & 2) ? maximum.y : minimum.y,
                              (corner & 4) ? maximum.z : minimum.z);
        const glm::vec4 clip = lightViewProjection * glm::vec4(world, 1.0f);
        if (clip.w <= 1.0e-5f)
            return true;   // 有一个点在光相机背后，交给硬件裁剪，别在这里冒险
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        ndcMin = glm::min(ndcMin, ndc);
        ndcMax = glm::max(ndcMax, ndc);
    }
    if (ndcMax.x < -1.0f || ndcMin.x > 1.0f)
        return false;
    if (ndcMax.y < -1.0f || ndcMin.y > 1.0f)
        return false;
    if (ndcMax.z < 0.0f || ndcMin.z > 1.0f)
        return false;
    return true;
}

bool ShadowPass::selfTest()
{
    // 纯 CPU：拿一个真实形状的相机，验证级联数学本身。
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 5.0f, 20.0f),
                                       glm::vec3(0.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
    const CameraIntrinsics intrinsics = extractIntrinsics(projection);

    if (!(intrinsics.farZ > intrinsics.nearZ) || !(intrinsics.tanY > 0.0f))
    {
        std::cerr << "[Shadow] selfTest FAIL | intrinsics " << intrinsics.nearZ << " / "
                  << intrinsics.farZ << " tanY=" << intrinsics.tanY << std::endl;
        return false;
    }

    const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.35f, -1.0f, -0.25f));
    const std::array<Cascade, kCascadeCount> cascades =
        buildCascades(view, intrinsics, lightDirection, kResolution);

    // 1) split 必须严格递增，且末级不超过 kMaxShadowDistance（否则每级纹素密度暴跌）。
    bool splitsIncrease = true;
    for (uint32_t index = 1; index < kCascadeCount; ++index)
        splitsIncrease = splitsIncrease &&
                         cascades[index].splitDistance > cascades[index - 1].splitDistance;
    const bool lastSplitClamped = cascades[kCascadeCount - 1].splitDistance <= kMaxShadowDistance + 1.0e-3f;

    // 2) 每级联**自己那一段**的 8 个视锥角点投影进光空间后必须都落在裁剪体内
    //    （正交下即 [-1,1]x[-1,1]x[0,1]）。用最后一级的角点去测第一级会必然失败 ——
    //    第一级只覆盖 near..split0，末级远角点本来就在它外面。
    bool cornersInside = true;
    float previousSplit = intrinsics.nearZ;
    for (uint32_t index = 0; index < kCascadeCount; ++index)
    {
        const std::array<glm::vec3, 8> corners =
            frustumCorners(glm::inverse(view), previousSplit,
                           cascades[index].splitDistance, intrinsics.tanX, intrinsics.tanY);
        previousSplit = cascades[index].splitDistance;
        for (const glm::vec3& corner : corners)
        {
            const glm::vec4 clip = cascades[index].viewProjection * glm::vec4(corner, 1.0f);
            if (clip.w <= 1.0e-5f)
            {
                cornersInside = false;
                break;
            }
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.02f || ndc.x > 1.02f || ndc.y < -1.02f || ndc.y > 1.02f ||
                ndc.z < -0.02f || ndc.z > 1.02f)
            {
                cornersInside = false;
                break;
            }
        }
    }

    // 3) 视锥剔除：远处一个巨大的球必须被第一级剔掉，原点附近的球必须留下。
    const bool cullHolds = !cascadeVisible(cascades[0].viewProjection,
                                           glm::vec3(400.0f, 0.0f, 400.0f),
                                           glm::vec3(401.0f, 1.0f, 401.0f)) &&
                           cascadeVisible(cascades[0].viewProjection,
                                          glm::vec3(-1.0f, 0.0f, -1.0f),
                                          glm::vec3(1.0f, 2.0f, 1.0f));

    if (!splitsIncrease || !lastSplitClamped || !cornersInside || !cullHolds)
    {
        std::cerr << "[Shadow] selfTest FAIL | splits=" << splitsIncrease
                  << " clamp=" << lastSplitClamped << " corners=" << cornersInside
                  << " cull=" << cullHolds << std::endl;
        return false;
    }

    std::cout << "[Shadow] selfTest PASS | cascades=" << kCascadeCount
              << " lastSplit=" << cascades[kCascadeCount - 1].splitDistance
              << " corners inside, culling holds" << std::endl;
    return true;
}
