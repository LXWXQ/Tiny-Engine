#include "GeometrySystem.h"
#include "Core/DescriptorManager.h"
#include "RHI/CommandUtils.h"
#include "RHI/GBuffer.h"
#include "AssetPaths.h"
#include <iostream>
#include <algorithm>
#include <stdexcept>

GeometrySystem::GeometrySystem(Device& device, VkFormat depthFormat,
                               VkDescriptorSetLayout globalSetLayout,
                               VkDescriptorSetLayout materialSetLayout)
    : vulkanDevice(device), depthFormat(depthFormat)
{
    createPipelineLayout(globalSetLayout, materialSetLayout);
    createPipelines();
}

GeometrySystem::~GeometrySystem()
{
    vkDestroyPipelineLayout(vulkanDevice.getDevice(), pipelineLayout, nullptr);
}

void GeometrySystem::createPipelineLayout(VkDescriptorSetLayout globalSetLayout,
                                          VkDescriptorSetLayout materialSetLayout)
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(SimplePushConstantData);

    std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ globalSetLayout, materialSetLayout };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(descriptorSetLayouts.size());
    pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(vulkanDevice.getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("GeometrySystem: pipeline layout failed!");
}

void GeometrySystem::createPipelines()
{
    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);

    // 动态渲染：附件格式必须与渲染图里 writeColor 的顺序完全一致
    config.colorAttachmentFormats = {
        GBufferFormat::Position, GBufferFormat::Normal, GBufferFormat::Albedo, GBufferFormat::Pbr
    };
    config.depthAttachmentFormat = depthFormat;
    config.pipelineLayout = pipelineLayout;

    // 4 个颜色附件，混合全部关闭
    VkPipelineColorBlendAttachmentState defaultAttachment = config.colorBlendAttachments[0];
    config.colorBlendAttachments.assign(4, defaultAttachment);
    config.colorBlendInfo.attachmentCount = 4;
    config.colorBlendInfo.pAttachments = config.colorBlendAttachments.data();

    geometryPipeline = std::make_unique<Pipeline>(vulkanDevice,
        SENTINEL_SHADER_DIR "mrt.vert.spv", SENTINEL_SHADER_DIR "mrt.frag.spv", config);
    gridPipeline = std::make_unique<Pipeline>(vulkanDevice,
        SENTINEL_SHADER_DIR "grid_mrt.vert.spv", SENTINEL_SHADER_DIR "grid_mrt.frag.spv", config);

    // 天空盒：不写深度、深度比较改成 LESS_OR_EQUAL（配合 gl_Position = pos.xyww）
    config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;
    config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    skyboxPipeline = std::make_unique<Pipeline>(vulkanDevice,
        SENTINEL_SHADER_DIR "skybox_mrt.vert.spv", SENTINEL_SHADER_DIR "skybox_mrt.frag.spv", config);
}

void GeometrySystem::render(VkCommandBuffer commandBuffer, FrameInfo& frameInfo)
{
    DescriptorManager& descMgr = *frameInfo.descriptorManager;

    // -----------------------------------------------------------------------
    // D-3 bindless：**整帧只绑一次 set 1**。
    //
    // 以前是"每个子网格切材质就 vkCmdBindDescriptorSets 一次"—— Sponza 有
    // 103 个子网格，于是 104 次 draw 伴随多达 104 次描述符集绑定。
    // 现在材质纹理在一个数组里，下标走 push constant，绑定与绘制彻底解耦。
    // -----------------------------------------------------------------------
    VkDescriptorSet sets[] = { descMgr.getGlobalSet(frameInfo.frameInFlightIndex), descMgr.getMaterialSet() };
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout, 0, 2, sets, 0, nullptr);
    frameInfo.telemetry.descriptorBinds += 1;

    // 没材质时用哪个下标：数组尾部允许未绑定（descriptorBindingPartiallyBound），
    // 但"没绑定的内容"采出来是未定义值。所以退化路径不指向数组，
    // 而是让着色器走 materialValid = 0 的分支（直接输出顶点色 / 默认 PBR）。
    const uint32_t fallbackMatId = 0;

    for (entt::entity entity : frameInfo.visibleEntities)
    {
        const MeshComponent* mesh = frameInfo.registry.try_get<MeshComponent>(entity);
        if (!mesh || !mesh->model)
            continue;

        const WorldTransformComponent* worldTransform =
            frameInfo.registry.try_get<WorldTransformComponent>(entity);
        if (!worldTransform)
            continue;

        const RenderTagComponent* tag = frameInfo.registry.try_get<RenderTagComponent>(entity);
        const MaterialComponent* materialComp = frameInfo.registry.try_get<MaterialComponent>(entity);

        const bool isSkybox = tag && tag->isSkybox;
        const bool isGrid = tag && tag->isGrid;

        SimplePushConstantData push{};
        push.modelMatrix = worldTransform->matrix;
        push.normalMatrix = worldTransform->normalMatrix;

        const bool objectUsesMaterial = !isSkybox && !isGrid;
        push.materialValid = objectUsesMaterial ? 1u : 0u;
        push.materialId = fallbackMatId;

        // 实体自带材质、但材质没有 albedo 贴图 -> 整实体走占位棋盘格。
        // glTF 的子网格材质另算（SubMesh::albedoFallback），不能在这里一刀切。
        const bool entityAlbedoFallback = materialComp && materialComp->material &&
                                          !materialComp->material->albedoMap;

        // 棋盘格密度：让"最大尺寸上约 6 格"。
        // 不用固定常数是因为模型的局部尺寸差异极大（debug_cube 与 Quad-Sphere
        // 相差上百倍），固定频率会让大模型糊成平均灰、小模型只有一格。
        push.checkerScale = 1.0f;
        if (const auto* bounds = frameInfo.registry.try_get<BoundsComponent>(entity))
        {
            if (bounds->local.valid)
            {
                const glm::vec3 extent = bounds->local.max - bounds->local.min;
                const float maxExtent = std::max({ extent.x, extent.y, extent.z });
                if (maxExtent > 1e-6f)
                    push.checkerScale = 6.0f / maxExtent;   // 3D 棋盘格周期为 1
            }
        }

        // UV 归一化：只有 UV 本身覆盖 [0,1] 附近时才用 UV 棋盘。
        // Quad-Sphere 的 UV 只占 0.05~0.43（图集子区间），强行归一化会在
        // 球面/接缝处变成大片扭曲的格子；这种情况改用 3D 棋盘。
        push.checkerUvTransform = glm::vec4(0.0f);
        if (mesh->model && mesh->model->hasUv())
        {
            const glm::vec2 uvMin = mesh->model->getUvMin();
            const glm::vec2 uvMax = mesh->model->getUvMax();
            const glm::vec2 uvExtent = uvMax - uvMin;
            if (uvExtent.x > 0.8f && uvExtent.y > 0.8f)
            {
                const float scaleU = 1.0f / uvExtent.x;
                const float scaleV = 1.0f / uvExtent.y;
                push.checkerUvTransform = glm::vec4(scaleU, scaleV, uvMin.x, uvMin.y);
            }
        }

        auto* pipe = &geometryPipeline;
        if (isSkybox)    pipe = &skyboxPipeline;
        else if (isGrid) pipe = &gridPipeline;

        (*pipe)->bind(commandBuffer);
        mesh->model->bind(commandBuffer);

        for (const SubMesh& subMesh : mesh->model->getSubMeshes())
        {
            // 实体级材质槽位只认 MaterialComponent::materialSetIndex：
            //   * 它跟着实体生命周期走，clearEntities() 释放槽位时会一起清掉；
            //   * Material::descriptorSetIndex 是"材质对象"的字段，材质可能被多个
            //     实体共享，槽位释放后它会过期，不能拿来判断实体有没有槽位。
            // 之前这里优先读 material->descriptorSetIndex，而 createMeshEntity 只写了
            // MaterialComponent 那一份 —— 结果每帧都误判成"没有材质"并刷屏。
            const uint32_t objectMaterialSlot = materialComp ? materialComp->materialSetIndex
                                                             : kInvalidMaterialSlot;

            uint32_t slot = (subMesh.materialSlot != kInvalidMaterialSlot)
                ? subMesh.materialSlot
                : objectMaterialSlot;

            if (objectUsesMaterial && slot == kInvalidMaterialSlot)
            {
                const uint32_t entityId = entt::to_integral(entity);
                if (warnedFallbackEntities.insert(entityId).second)
                {
                    // 只提示一次，并且走 stdout：stderr 必须保持 0 字节（回归判据）。
                    std::cout << "[GeometrySystem] entity " << entityId
                              << " 没有有效材质槽位，使用棋盘格占位着色（仅提示一次）\n";
                }
                slot = fallbackMatId;
                push.materialValid = 0u;
            }
            else if (slot != kInvalidMaterialSlot)
            {
                push.materialValid = objectUsesMaterial ? 1u : 0u;
            }

            push.materialId = (slot == kInvalidMaterialSlot) ? fallbackMatId : slot;
            push.albedoFallback = (subMesh.albedoFallback || entityAlbedoFallback) ? 1u : 0u;
            push.alphaMasked = subMesh.alphaMasked ? 1u : 0u;
            push.alphaCutoff = subMesh.alphaCutoff;
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SimplePushConstantData), &push);

            mesh->model->drawSubMesh(commandBuffer, subMesh);

            frameInfo.telemetry.drawCalls += 1;
            frameInfo.telemetry.triangles += subMesh.indexCount / 3;
            frameInfo.telemetry.vertices += subMesh.indexCount;
        }
    }

}