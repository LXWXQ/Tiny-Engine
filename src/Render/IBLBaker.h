#pragma once

#include "RHI/Device.h"
#include "RHI/CubemapTexture.h"
#include "RHI/Pipeline.h"
#include <memory>

class Texture;

// 把等距柱状 HDR 环境贴图烘焙成 IBL 需要的三样东西：
//   * Irradiance Map   —— 漫反射 IBL（半球余弦卷积，非常模糊）
//   * Prefiltered Map  —— 镜面 IBL（GGX 重要性采样 + mip 链，每级对应一个粗糙度）
//   * BRDF LUT         —— split-sum 近似的第二项，(NdotV, roughness) -> (A, B)
//
// 烘焙全部在 GPU 上完成，一次性执行（初始化阶段），之后只读。
class IBLBaker
{
public:
    IBLBaker(Device& device, VkCommandPool commandPool);
    ~IBLBaker();

    IBLBaker(const IBLBaker&) = delete;
    IBLBaker& operator=(const IBLBaker&) = delete;

    // equirectEnv 必须是 SHADER_READ_ONLY 布局的 2D HDR 纹理
    void bake(Texture& equirectEnv);

    const CubemapTexture& getIrradianceMap()  const { return *irradianceMap; }
    const CubemapTexture& getPrefilteredMap() const { return *prefilteredMap; }
    VkSampler   getBrdfLutSampler() const { return brdfLutSampler; }
    VkImageView getBrdfLutView()    const { return brdfLutView; }
    VkImage     getBrdfLutImage()   const { return brdfLutImage; }

private:
    static constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kLutFormat  = VK_FORMAT_R16G16_SFLOAT;

    static constexpr uint32_t kEnvCubeSize    = 256;
    static constexpr uint32_t kEnvCubeMips    = 9;    // 256 -> 1
    static constexpr uint32_t kIrradianceSize = 32;
    static constexpr uint32_t kPrefilterSize  = 128;
    static constexpr uint32_t kPrefilterMips  = 5;    // 128 -> 8
    static constexpr uint32_t kBrdfLutSize    = 256;
    static constexpr int      kPrefilterSamples = 1024;

    struct PushConstants
    {
        int32_t faceIndex = 0;
        float   roughness = 0.0f;
        int32_t sampleCount = kPrefilterSamples;
        float   sourceFaceSize = static_cast<float>(kEnvCubeSize);
    };

    void createTargets();
    void createDescriptors(Texture& equirectEnv);
    void createPipelines();

    void transitionImageRange(VkCommandBuffer cmd, VkImage image, uint32_t baseMip, uint32_t mipCount,
                              uint32_t layerCount,
                              VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                              VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
    void transitionImage(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels, uint32_t layerCount,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
    void renderFaces(VkCommandBuffer cmd, CubemapTexture& target, Pipeline* pipeline,
                     VkPipelineLayout layout, VkDescriptorSet set,
                     int32_t sampleCount, float sourceFaceSize, bool perMipRoughness);
    void renderBrdfLut(VkCommandBuffer cmd);

    void recordEquirectToCube(VkCommandBuffer cmd);
    void recordEnvCubeMips(VkCommandBuffer cmd);
    void recordIrradiance(VkCommandBuffer cmd);
    void recordPrefilter(VkCommandBuffer cmd);

    Device& device;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    std::unique_ptr<CubemapTexture> envCube;
    std::unique_ptr<CubemapTexture> irradianceMap;
    std::unique_ptr<CubemapTexture> prefilteredMap;

    VkImage brdfLutImage = VK_NULL_HANDLE;
    VmaAllocation brdfLutAllocation = VK_NULL_HANDLE;
    VkImageView brdfLutView = VK_NULL_HANDLE;
    VkSampler brdfLutSampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout srcSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet equirectSet = VK_NULL_HANDLE;
    VkDescriptorSet envCubeSet  = VK_NULL_HANDLE;

    VkPipelineLayout bakeLayout = VK_NULL_HANDLE;   // 带 set 0 + push constant
    VkPipelineLayout lutLayout  = VK_NULL_HANDLE;   // 空

    std::unique_ptr<Pipeline> equirectToCubePipeline;
    std::unique_ptr<Pipeline> irradiancePipeline;
    std::unique_ptr<Pipeline> prefilterPipeline;
    std::unique_ptr<Pipeline> brdfLutPipeline;
};