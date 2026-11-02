#pragma once
#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "RHI/RenderGraph.h"
#include "Render/RenderResources.h"
#include "Core/DescriptorManager.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// D-2: Bloom（泛光）
//
// 结构是标准的「降采样链 + 上采样叠加」：
//
//   hdrScene ──bright──> bloom0 ──down──> bloom1 ──down──> bloom2 ──down──> bloom3
//                          ^            ^             ^             ^
//                          └──up────────┴─────up──────┴─────up──────┘
//
// 为什么用金字塔而不是"一次大半径高斯模糊"：
//   同等的辉光半径，金字塔的采样数是 O(N)（每级固定 13 + 9 个抽样），
//   而单级大核高斯是 O(r^2)。更重要的是金字塔天然给出多尺度的光晕 ——
//   近处的锐利辉光来自细级、远处的大片光雾来自粗级，
//   这正是真实镜头/眼睛的散射形态。
//
// 为什么两个方向都要写同一张图（bloom[i] 先被 down 写、后被 up 读改写）：
//   up 级用的是「加法混合 + LOAD_OP_LOAD」，等于把粗级的贡献累加回细级，
//   因此不需要额外的累加缓冲。渲染图会按声明顺序自动插好
//   COLOR_ATTACHMENT -> SHADER_READ -> COLOR_ATTACHMENT 的屏障。
// ---------------------------------------------------------------------------
class BloomPass : public IRenderPass
{
public:
    BloomPass(Device& device, RenderGraph& graph, RenderResources& resources,
              DescriptorManager* descMgr);
    ~BloomPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    void resize(VkExtent2D extent) override;
    std::string getName() const override { return "Bloom"; }

    // 每一级金字塔的降采样倍率固定为 2，级数决定辉光的最大半径
    static constexpr uint32_t kLevelCount = 4;

    // 这三个结构体的字段顺序/大小必须与 shaders/bloom_*.frag 里的 push_constant
    // 声明逐一对应（GLSL push_constant 走 std430 布局）。它们是 public 的
    // 唯一原因是让 BloomPass.cpp 里的 static_assert 能取到 sizeof —— 这条断言
    // 是这个约定的唯一护栏，宁可牺牲一点封装。
    struct BrightPush
    {
        float threshold;
        float knee;
        float intensity;
    };

    struct DownPush
    {
        glm::vec2 srcTexelSize;
        int32_t   karisAverage;
        float     pad;
    };

    struct UpPush
    {
        glm::vec2 srcTexelSize;
        float     radius;
        float     pad;
    };

private:
    void createDescriptorLayout();
    void createSampler();
    void createPipelines();
    void updateDescriptor(uint32_t index, VkImageView view);

    Device& device;
    RenderGraph& renderGraph;
    RenderResources& resources;
    DescriptorManager* descriptorManager;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;      // [0]=bright, [1..3]=down, [4..6]=up

    VkPipelineLayout brightLayout = VK_NULL_HANDLE;
    VkPipelineLayout downLayout = VK_NULL_HANDLE;
    VkPipelineLayout upLayout = VK_NULL_HANDLE;

    std::unique_ptr<Pipeline> brightPipeline;
    std::unique_ptr<Pipeline> downPipeline;
    std::unique_ptr<Pipeline> upPipeline;
};
