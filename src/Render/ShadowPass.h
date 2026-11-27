#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"

#include <array>
#include <memory>

#include <glm/glm.hpp>

class RenderGraph;
class TerrainMeshArena;
class Pipeline;
class VegetationPass;
class SceneGpuBatch;
class VirtualShadowMap;

// F6：方向光阴影（3 级联 CSM）。
//
// 设计取舍说明：
// 1) P4-5 起阴影几何复用 P2 的可见实例表：每级联一次剔除 + 一条合并网格的 indirect。
//    P4-5 之前这里是 CPU 逐实体逐子网格提交（Sponza 103 个子网格 x 3 级联 = 309 次 draw，
//    一次剔除都不做）。修掉「逐模型路径漏乘 glTF 节点矩阵」这个老 bug 之后，静态 Sponza
//    实测 0.087 -> 0.066 ms（P4-5 之前那个 0.204 ms 是在画一张被放大 125 倍、
//    位置也错的地图，不能当基线）。地形 chunk / 体素 chunk / 蒙皮模型不在合并缓冲里，
//    仍然逐模型提交 —— 两条路径并存。
// 2) 深度图与采样器由本 Pass 自己管理（手工屏障），不进渲染图：
//    图上一条纹理对应一个图层，3 层级联会让布局/屏障声明变复杂，
//    而这块资源只有「本 Pass 写、Lighting 读」一条依赖。
// 3) 级联矩阵写进全局 UBO（set 0 binding 0 的尾部），采样器是 set 0 binding 11，
//    这样 Lighting 不需要新描述符集，编辑器/后处理也不用改。
class ShadowPass : public IRenderPass
{
public:
    static constexpr uint32_t kCascadeCount = 3;
    // P2 的视图槽：0 = 主视图，1 = 平面反射，2/3/4 = 三个级联。级联视图不做 Hi-Z 遮挡
    // 与屏幕空间误差剔除（前者是按主相机建的，后者阈值按主视图像素密度定）。
    static constexpr uint32_t kShadowViewBase = 2u;
    // P4 第零步之后：基准从 2048 提到 4096，让「阴影 Pass 的 GPU 时间」从亚毫秒变成
    // 毫秒级，验收的比值才有分母。显存代价是每帧槽 3 x 4096^2 x 4B = 201 MiB（两个帧槽共 402 MiB）。
    static constexpr uint32_t kResolution = 4096;

    struct Cascade
    {
        glm::mat4 viewProjection{ 1.0f };
        float splitDistance = 0.0f;   // 该级的最远「视深度」，Lighting 用它选级
    };

    ShadowPass(Device& device, RenderGraph& graph, TerrainMeshArena* terrainMeshArena);
    ~ShadowPass() override;

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "Shadow"; }

    // 每帧在主相机矩阵确定后调用一次：算级联、记下本帧用哪套矩阵。
    void updateCascades(const glm::mat4& cameraView, const glm::mat4& cameraProjection,
                        const glm::vec3& lightDirection);

    const std::array<Cascade, kCascadeCount>& getCascades() const { return cascades; }

    VkImageView getShadowView(uint32_t frameInFlightIndex) const;
    VkSampler getShadowSampler() const { return shadowSampler; }

    // P2 第十四步：让阴影也能吃 GPU 分批（每级联一份剔除 + 紧凑命令）。
    // 必须在 init() 之后调用 —— SceneGpuBatch 是在 GeometryPass::init 里创建的，
    // 这时本类的管线布局要从「无描述符集」重建为「set 3 = 实例表」。
    void setSceneGpuBatch(SceneGpuBatch* batch);
    bool hasGpuBatch() const { return sceneGpuBatch != nullptr; }

    void setEnabled(bool value) { enabled = value; }
    bool isEnabled() const { return enabled; }

    // P4 第零步：CSM 基线尺子。开启时每级联各写一对时间戳（TOP_OF_PIPE 进、
    // ALL_COMMANDS 出），每 60 帧打一行、退出时打总平均 —— P4 验收里
    // 「静态场景阴影 Pass GPU 时间下降 60%」这条门槛就从这个读数起算。
    // 必须在 init() 之前调用：query pool 是在 init 里建的。
    void setTimingEnabled(bool value) { timingRequested = value; }
    bool isTimingEnabled() const { return timingEnabled; }
    uint32_t timingQueryCount() const { return timingPoolSize; }
    // 退出路径调用：先等设备空闲，把最后几帧没读回的时间戳读干净，然后打总结。
    void reportTimingSummary();

    // Optional: the shadow pass calls back into the vegetation pass so tree
    // instances land in the same cascades without a second render graph pass.
    void setVegetationPass(VegetationPass* pass) { vegetationPass = pass; }

    // P4-4：页标记要本帧的投影者包围盒，渲染管线启动时把虚拟阴影图挂上来。
    // 不挂（或没有 --dump-vsm-resources 建图）就一行都不执行。
    void setVirtualShadowMap(VirtualShadowMap* map) { virtualShadowMap = map; }

    struct Stats
    {
        uint32_t drewEntities = 0;
        uint32_t drewTerrainChunks = 0;
        uint32_t drawCalls = 0;
        uint32_t triangles = 0;
    };
    const Stats& getStats() const { return stats; }

    // 级联数学的自检：不建 Device，纯 CPU。
    static bool selfTest();

private:

    // P4-4 / P4-4b：采集投影者 → 标脏页 → 只把脏页画进图集。
    void recordVirtualShadowPages(VkCommandBuffer cmd, FrameInfo& frameInfo);
    VirtualShadowMap* virtualShadowMap = nullptr;

    // 逐页分桶用的 CSR（每帧重建，复用容量）：一级 1024 页，各页的投影者列表。
    std::vector<uint32_t> pageCellCounts;
    std::vector<uint32_t> pageCellOffsets;
    std::vector<uint32_t> pageCellItems;
    uint64_t vsmDrawFrames = 0;
    uint64_t vsmSkipFrames = 0;
    uint64_t vsmDrawnPages = 0;
    uint64_t vsmDrawCalls = 0;
    // P4-6：图集现在是「可采样」布局（上一帧末尾转过去），下一个写帧要转回来。
    bool vsmAtlasReadable = false;
    struct Frame
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView arrayView = VK_NULL_HANDLE;
        std::array<VkImageView, kCascadeCount> layerViews{};
    };

    void createResources();
    void destroyResources();
    void createPipeline();

    void recordCascade(VkCommandBuffer cmd, uint32_t cascadeIndex, uint32_t frameInFlightIndex,
                       FrameInfo& frameInfo, bool gpuBatch);
    static bool cascadeVisible(const glm::mat4& lightViewProjection, const glm::vec3& minimum,
                               const glm::vec3& maximum);

    void createTimingResources();
    void destroyTimingResources();
    // 读回「上一轮用过这个帧槽」的时间戳。调用点必须在 vkCmdResetQueryPool 之前。
    void collectTiming(uint32_t frameInFlightIndex);

    Device& vulkanDevice;
    RenderGraph& renderGraph;
    TerrainMeshArena* terrainMeshArena = nullptr;
    VegetationPass* vegetationPass = nullptr;

    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> pipeline;
    // GPU 批量路径单独一条管线：它的像素着色器静态使用 set 3（实例表 + remap），
    // 逐模型那条管线一个描述符集都不绑，两条不能共用布局。
    VkPipelineLayout gpuPipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<Pipeline> gpuPipeline;
    SceneGpuBatch* sceneGpuBatch = nullptr;
    VkDescriptorSetLayout sceneInstanceSetLayout = VK_NULL_HANDLE;
    bool reportedGpuBatch = false;
    std::array<Frame, kFramesInFlight> frames{};

    std::array<Cascade, kCascadeCount> cascades{};
    glm::vec3 cachedLightDirection{ 0.0f, -1.0f, 0.0f };
    bool cascadesDirty = true;
    bool enabled = true;
    Stats stats{};
    bool reported = false;

    // P4 第零步：每个级联一对 timestamp（进 / 出），每个帧槽一组。
    static constexpr uint32_t kTimingQueriesPerCascade = 2;
    static constexpr uint32_t kTimingQueriesPerFrame = kCascadeCount * kTimingQueriesPerCascade;
    static constexpr uint32_t kTimingReportInterval = 60;

    bool timingRequested = false;   // 命令行要了
    bool timingEnabled = false;     // 真的建起来了（编译进来 + 设备支持）
    VkQueryPool timingPool = VK_NULL_HANDLE;
    uint32_t timingPoolSize = 0;
    float timestampPeriodNs = 1.0f;
    std::array<bool, kFramesInFlight> timingPending{};
    std::array<double, kCascadeCount> lastCascadeMs{};
    double lastTotalMs = 0.0;
    uint64_t timingFrameCount = 0;
    std::array<double, kCascadeCount> cascadeMsSum{};
    double totalMsSum = 0.0;
    double totalMsMin = 0.0;
    double totalMsMax = 0.0;
    std::array<double, kCascadeCount> windowCascadeSum{};
    double windowTotalSum = 0.0;
    uint32_t windowFrames = 0;
    // 首帧先把所有槽位转成 SHADER_READ_ONLY：即使阴影关掉，法线光照也会绑定这张
    // 描述符，validation 会拿描述符声明的布局和图像实际布局比较，UNDEFINED 会报错。
    bool layoutReady = false;
};
