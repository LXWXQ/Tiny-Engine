#include "CloudPass.h"
#include "AssetPaths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

static_assert(sizeof(CloudPass::PushConstants) == 128,
              "CloudPass::PushConstants 必须与 cloud.frag 的 push_constant 等大");

namespace
{
    constexpr uint32_t kNoiseSize = 64;

    uint32_t wrapIndex(int value, int period)
    {
        const int wrapped = value % period;
        return static_cast<uint32_t>(wrapped < 0 ? wrapped + period : wrapped);
    }

    uint32_t hash3(int x, int y, int z)
    {
        uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u;
        h ^= static_cast<uint32_t>(y) * 0xd8163841u;
        h ^= static_cast<uint32_t>(z) * 0xcb1ab31fu;
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        h *= 0x297a2d39u;
        h ^= h >> 15;
        return h;
    }

    float random01(uint32_t h)
    {
        return static_cast<float>(h & 0x00ffffffu) / 16777216.0f;
    }

    glm::vec3 gradient(uint32_t h)
    {
        constexpr float k = 0.70710678f;
        switch (h % 12u)
        {
        case 0:  return {  k,  k, 0.0f };
        case 1:  return { -k,  k, 0.0f };
        case 2:  return {  k, -k, 0.0f };
        case 3:  return { -k, -k, 0.0f };
        case 4:  return {  k, 0.0f,  k };
        case 5:  return { -k, 0.0f,  k };
        case 6:  return {  k, 0.0f, -k };
        case 7:  return { -k, 0.0f, -k };
        case 8:  return { 0.0f,  k,  k };
        case 9:  return { 0.0f, -k,  k };
        case 10: return { 0.0f,  k, -k };
        default: return { 0.0f, -k, -k };
        }
    }

    float fade(float t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    // 可平铺 Perlin：整数格点按 period 取模，因此 u=0 与 u=1 的边界连续。
    float perlin(glm::vec3 p, int period)
    {
        const glm::vec3 base = glm::floor(p);
        const glm::vec3 f = p - base;

        const int ix = static_cast<int>(base.x);
        const int iy = static_cast<int>(base.y);
        const int iz = static_cast<int>(base.z);

        auto corner = [&](int dx, int dy, int dz)
        {
            const uint32_t h = hash3(static_cast<int>(wrapIndex(ix + dx, period)),
                                     static_cast<int>(wrapIndex(iy + dy, period)),
                                     static_cast<int>(wrapIndex(iz + dz, period)));
            return glm::dot(gradient(h), f - glm::vec3(float(dx), float(dy), float(dz)));
        };

        const float n000 = corner(0, 0, 0);
        const float n100 = corner(1, 0, 0);
        const float n010 = corner(0, 1, 0);
        const float n110 = corner(1, 1, 0);
        const float n001 = corner(0, 0, 1);
        const float n101 = corner(1, 0, 1);
        const float n011 = corner(0, 1, 1);
        const float n111 = corner(1, 1, 1);

        const float u = fade(f.x);
        const float v = fade(f.y);
        const float w = fade(f.z);
        const float nx00 = glm::mix(n000, n100, u);
        const float nx10 = glm::mix(n010, n110, u);
        const float nx01 = glm::mix(n001, n101, u);
        const float nx11 = glm::mix(n011, n111, u);
        return glm::mix(glm::mix(nx00, nx10, v), glm::mix(nx01, nx11, v), w);
    }

    float perlinFbm(glm::vec3 u, int basePeriod, int octaves)
    {
        float sum = 0.0f;
        float amplitude = 0.5f;
        float norm = 0.0f;
        int period = basePeriod;
        for (int i = 0; i < octaves; ++i)
        {
            sum += perlin(u * static_cast<float>(period), period) * amplitude;
            norm += amplitude;
            amplitude *= 0.5f;
            period *= 2;
        }
        return norm > 0.0f ? sum / norm : 0.0f;
    }

    // 可平铺 Worley：特征点按 period 回绕，和 Perlin 共用同一套周期约定。
    float worley(glm::vec3 u, int period)
    {
        const glm::vec3 p = u * static_cast<float>(period);
        const glm::vec3 base = glm::floor(p);
        const glm::vec3 f = p - base;
        const int bx = static_cast<int>(base.x);
        const int by = static_cast<int>(base.y);
        const int bz = static_cast<int>(base.z);

        float minDistance = 1.0e9f;
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int cx = bx + dx;
                    const int cy = by + dy;
                    const int cz = bz + dz;
                    const uint32_t h = hash3(static_cast<int>(wrapIndex(cx, period)),
                                             static_cast<int>(wrapIndex(cy, period)),
                                             static_cast<int>(wrapIndex(cz, period)));
                    const glm::vec3 feature{
                        random01(h),
                        random01(h * 1664525u + 1013904223u),
                        random01(h * 22695477u + 1u)
                    };
                    const glm::vec3 delta = glm::vec3(float(cx), float(cy), float(cz)) + feature - p;
                    minDistance = std::min(minDistance, glm::dot(delta, delta));
                }
            }
        }
        return std::sqrt(minDistance);
    }

    float worleyFbm(glm::vec3 u, int basePeriod, int octaves)
    {
        float sum = 0.0f;
        float amplitude = 0.5f;
        float norm = 0.0f;
        int period = basePeriod;
        for (int i = 0; i < octaves; ++i)
        {
            // 1 - distance：特征点处 0，格子边界处接近 1，做成“侵蚀”纹理。
            sum += (1.0f - std::min(worley(u, period), 1.0f)) * amplitude;
            norm += amplitude;
            amplitude *= 0.5f;
            period *= 2;
        }
        return norm > 0.0f ? sum / norm : 0.0f;
    }

    std::vector<uint8_t> buildNoiseVolume()
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(kNoiseSize) * kNoiseSize * kNoiseSize * 4u);
        size_t offset = 0;
        for (uint32_t z = 0; z < kNoiseSize; ++z)
        {
            for (uint32_t y = 0; y < kNoiseSize; ++y)
            {
                for (uint32_t x = 0; x < kNoiseSize; ++x)
                {
                    const glm::vec3 u{
                        (static_cast<float>(x) + 0.5f) / static_cast<float>(kNoiseSize),
                        (static_cast<float>(y) + 0.5f) / static_cast<float>(kNoiseSize),
                        (static_cast<float>(z) + 0.5f) / static_cast<float>(kNoiseSize)
                    };

                    const float low = 0.5f + 0.5f * perlinFbm(u, 2, 2);
                    const float billow = worleyFbm(u, 4, 3);
                    const float detail = 0.5f + 0.5f * perlinFbm(u, 8, 3);
                    const float shape = 0.5f + 0.5f * perlinFbm(u, 4, 5);

                    // 低频大团 + Worley 边界做成云的“菜花状”，再用 smoothstep
                    // 拉开对比度；直接线性叠加会得到一团均匀的灰。
                    const float combined = shape * 0.55f + billow * 0.45f;
                    const float masked = std::clamp((combined - 0.28f) / 0.55f, 0.0f, 1.0f);
                    const float shaped = masked * masked * (3.0f - 2.0f * masked);

                    pixels[offset + 0] = static_cast<uint8_t>(std::clamp(shaped, 0.0f, 1.0f) * 255.0f + 0.5f);
                    pixels[offset + 1] = static_cast<uint8_t>(std::clamp(billow, 0.0f, 1.0f) * 255.0f + 0.5f);
                    pixels[offset + 2] = static_cast<uint8_t>(std::clamp(detail, 0.0f, 1.0f) * 255.0f + 0.5f);
                    pixels[offset + 3] = static_cast<uint8_t>(std::clamp(low, 0.0f, 1.0f) * 255.0f + 0.5f);
                    offset += 4;
                }
            }
        }
        return pixels;
    }
}

CloudPass::CloudPass(Device& device, RenderGraph& graph, RenderResources& resources,
                     VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
                     VkCommandPool commandPool)
    : device(device), renderGraph(graph), resources(resources),
      globalSetLayout(globalSetLayout), descriptorManager(descriptorManager), commandPool(commandPool)
{
}

CloudPass::~CloudPass()
{
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device.getDevice(), pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)      vkDestroyDescriptorSetLayout(device.getDevice(), setLayout, nullptr);
    if (positionSampler != VK_NULL_HANDLE) vkDestroySampler(device.getDevice(), positionSampler, nullptr);
}

void CloudPass::init()
{
    createNoiseTexture();
    createDescriptorLayout();
    createSampler();

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        sets[frame] = descriptorManager->allocateSet(setLayout);
        updateDescriptor(frame);
    }

    createPipeline();
    std::cout << "[Cloud] initialized | noise=" << kNoiseSize << "^3" << std::endl;
}

void CloudPass::createNoiseTexture()
{
    const std::vector<uint8_t> pixels = buildNoiseVolume();
    noiseTexture = std::make_unique<Texture3D>(
        device, pixels.data(), kNoiseSize, kNoiseSize, kNoiseSize,
        VK_FORMAT_R8G8B8A8_UNORM, commandPool);
}

void CloudPass::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    info.bindingCount = 2;
    info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.getDevice(), &info, nullptr, &setLayout) != VK_SUCCESS)
        throw std::runtime_error("CloudPass: 描述符集布局创建失败");
}

void CloudPass::createSampler()
{
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device.getDevice(), &info, nullptr, &positionSampler) != VK_SUCCESS)
        throw std::runtime_error("CloudPass: 位置采样器创建失败");
}

void CloudPass::updateDescriptor(uint32_t frame)
{
    VkDescriptorImageInfo positionInfo{ positionSampler, renderGraph.getView(resources.gbPosition[frame]),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo noiseInfo{ noiseTexture->getSampler(), noiseTexture->getView(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = sets[frame];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &positionInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = sets[frame];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &noiseInfo;
    vkUpdateDescriptorSets(device.getDevice(), 2, writes, 0, nullptr);
}

void CloudPass::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    const VkDescriptorSetLayout layouts[2] = { globalSetLayout, setLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("CloudPass: 管线布局创建失败");

    PipelineConfigInfo config{};
    Pipeline::defaultPipelineConfigInfo(config, WIDTH, HEIGHT);
    config.pipelineLayout = pipelineLayout;
    config.colorAttachmentFormats = { kHdrSceneFormat };
    config.bindingDescriptions.clear();
    config.attributeDescriptions.clear();
    // 云没有几何，完全靠全屏三角形在片元里算；深度测试关闭，遮挡由着色器
    // 读取的 G-Buffer 世界坐标自行截断。
    config.depthStencilInfo.depthTestEnable = VK_FALSE;
    config.depthStencilInfo.depthWriteEnable = VK_FALSE;

    // 预乘 alpha：颜色已经乘过透射率，因此 RGB 用 ONE，alpha 保持目标值。
    VkPipelineColorBlendAttachmentState& blend = config.colorBlendAttachments[0];
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    pipeline = std::make_unique<Pipeline>(device,
        TINY_SHADER_DIR "fullscreen.vert.spv",
        TINY_SHADER_DIR "cloud.frag.spv", config);
}

void CloudPass::resize(VkExtent2D /*extent*/)
{
    // 云自身不持有 resize 相关目标；只要把 set1 的 gbPosition 重新指向新 view。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        updateDescriptor(frame);
}

void CloudPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
{
    const EngineSettings& s = frameInfo.settings;
    const uint32_t frame = frameInfo.frameInFlightIndex;

    if (!s.cloudEnabled || s.cloudSteps <= 0 || s.cloudDensity <= 0.0f || s.cloudThickness <= 0.0f)
    {
        active = false;
        if (!loggedDisabled && !s.cloudEnabled)
        {
            loggedDisabled = true;
            std::cout << "[Cloud] disabled | settings.cloudEnabled=0" << std::endl;
        }
        return;
    }

    active = true;
    timeSeconds += std::max(0.0f, frameInfo.frameTime);

    auto& pass = graph.addGraphicsPass("Cloud");
    pass.read(resources.gbPosition[frame]);
    pass.writeColor(resources.hdrScene[frame], VK_ATTACHMENT_LOAD_OP_LOAD);

    PushConstants push{};
    push.inverseProjectionView = glm::inverse(frameInfo.projectionMatrix * frameInfo.viewMatrix);
    push.params0 = glm::vec4(s.cloudCoverage, s.cloudDensity, s.cloudBaseHeight, s.cloudThickness);
    push.params1 = glm::vec4(s.cloudSpeed, s.cloudAbsorption, float(s.cloudSteps), 4.0f);
    push.params2 = glm::vec4(timeSeconds, 6500.0f, s.cloudAmbient, 1.0f);
    push.color = s.cloudColor;

    const VkDescriptorSet globalSet = descriptorManager->getGlobalSet(frame);
    const VkDescriptorSet localSet = sets[frame];
    if (!loggedFirstDraw)
    {
        loggedFirstDraw = true;
        std::cout << "[Cloud] first draw | coverage=" << s.cloudCoverage
                  << " density=" << s.cloudDensity
                  << " base=" << s.cloudBaseHeight
                  << " thickness=" << s.cloudThickness
                  << " steps=" << s.cloudSteps << "\n";
    }

    pass.execute([this, push, globalSet, localSet](VkCommandBuffer cmd)
    {
        pipeline->bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            0, 1, &globalSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            1, 1, &localSet, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });
}
