#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "RHI/Texture3D.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"

#include <array>
#include <glm/glm.hpp>
#include <memory>

// 场景里可放置的局部体积雾上限。CPU 端 UBO 与着色器数组必须一致。
inline constexpr uint32_t kMaxFogVolumes = 8;

// 局部体积雾（盒形）：
//
// 每个 FogVolumeComponent + WorldTransform 定义一个旋转盒体。全屏 raymarch
// 先读 G-Buffer 世界坐标，把步进区间截到最近的不透明表面；再对每个盒体做
// slab 相交，在盒内累积密度、噪声和高度衰减。输出预乘 alpha 混进 hdrScene。
//
// 为什么放在 Cloud 之后、Reflection/SSR/Glass/Water 之前：
//   - 雾通常离相机更近，应该覆盖远景云；
//   - 它要进入后续透明通道读取的 HDR，水面/玻璃才能看到雾中的倒影。
//   雾本身不写深度、不进 G-Buffer，也不参与 SSAO/阴影。
class FogVolumePass : public IRenderPass
{
public:
    FogVolumePass(Device& device, RenderGraph& graph, RenderResources& resources,
                  VkDescriptorSetLayout globalSetLayout, DescriptorManager* descriptorManager,
                  VkCommandPool commandPool);
    ~FogVolumePass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    // G-Buffer 的 view 在 resize 时重建，描述符要跟着换。
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "FogVolume"; }

    // 必须与 shaders/fog_volume.frag 的 push_constant 逐字节对应。
    struct PushConstants
    {
        glm::mat4 inverseProjectionView;
        glm::vec4 params;   // x=time y=maxDistance z/w=reserved
    };

    // 必须与 shader 里的 FogVolume 结构体逐字段对应（std140）。
    struct GpuVolume
    {
        alignas(16) glm::mat4 world;
        alignas(16) glm::mat4 inverseWorld;
        alignas(16) glm::vec4 params0;   // x=density y=heightFalloff z=noiseStrength w=noiseScale
        alignas(16) glm::vec4 params1;   // x=centerY y=edgeSoftness z=anisotropy w=ambient
        alignas(16) glm::vec4 color;     // rgb=散射色调 a=steps
        alignas(16) glm::vec4 wind;      // xyz=风速（米/秒）
    };

    struct VolumeUbo
    {
        alignas(16) glm::vec4 header;    // x=count
        GpuVolume volumes[kMaxFogVolumes];
    };

    bool isActive() const { return active; }

private:
    void createNoiseTexture();
    void createUniformBuffers();
    void createDescriptorLayout();
    void createSampler();
    void createPipeline();
    void updateDescriptor(uint32_t frame);
    void destroyUniformBuffers();

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    std::unique_ptr<Texture3D> noiseTexture;
    VkSampler positionSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;

    std::array<VkBuffer, kFramesInFlight> uboBuffers{};
    std::array<VmaAllocation, kFramesInFlight> uboAllocations{};
    std::array<void*, kFramesInFlight> uboMapped{};
    std::array<VkDescriptorSet, kFramesInFlight> sets{};

    float timeSeconds = 0.0f;
    bool active = false;
    bool loggedFirstDraw = false;
    bool loggedEmpty = false;
};

static_assert(sizeof(FogVolumePass::PushConstants) == 80,
              "FogVolumePass::PushConstants 必须与 fog_volume.frag 的 push_constant 等大");
static_assert(sizeof(FogVolumePass::GpuVolume) == 192,
              "FogVolumePass::GpuVolume 必须与 fog_volume.frag 的 FogVolume 等大");
static_assert(sizeof(FogVolumePass::VolumeUbo) == 16 + 192 * kMaxFogVolumes,
              "FogVolumePass::VolumeUbo 必须与 fog_volume.frag 的 UBO 等大");
