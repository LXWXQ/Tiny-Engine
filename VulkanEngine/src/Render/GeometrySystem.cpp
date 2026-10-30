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

        // 只有"实体自己带材质、但那个材质没有 albedo 贴图"才算 fallback。
        // glTF 实体的材质在子网格槽位里（materialComp 为空），不能误判成 fallback，
        // 否则 Sponza 会整片变成棋盘格。
        push.albedoFallback = (materialComp && materialComp->material &&
                               !materialComp->material->albedoMap) ? 1u : 0u;

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
                    push.checkerScale = 12.0f / maxExtent;   // 滤波公式周期为 2 -> 实际 6 格
            }
        }

        auto* pipe = &geometryPipeline;
        if (isSkybox)    pipe = &skyboxPipeline;
        else if (isGrid) pipe = &gridPipeline;

        (*pipe)->bind(commandBuffer);
        mesh->model->bind(commandBuffer);

        for (const SubMesh& subMesh : mesh->model->getSubMeshes())
        {
            const uint32_t objectMaterialSlot = materialComp
                ? (materialComp->material ? materialComp->material->descriptorSetIndex
                                          : materialComp->materialSetIndex)
                : kInvalidMaterialSlot;

            uint32_t slot = (subMesh.materialSlot != kInvalidMaterialSlot)
                ? subMesh.materialSlot
                : objectMaterialSlot;

            if (objectUsesMaterial && slot == kInvalidMaterialSlot)
            {
                std::cerr << "[GeometrySystem] entity " << entt::to_integral(entity)
                          << " 的子网格没有有效材质槽位，退化为默认着色。\n";
                slot = fallbackMatId;
                push.materialValid = 0u;
            }
            else if (slot != kInvalidMaterialSlot)
            {
                push.materialValid = objectUsesMaterial ? 1u : 0u;
            }

            push.materialId = (slot == kInvalidMaterialSlot) ? fallbackMatId : slot;
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