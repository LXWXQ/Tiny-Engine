#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "Core/FrameInFlight.h"
#include "Scene/Model.h"
#include "RHI/Device.h"
#include "RHI/Pipeline.h"
#include "Render/GpuScene.h"

class Model;
struct FrameInfo;

// F1：GPU 驱动提交。
//
// CPU 只做两件事：把本帧的网格摊平成实例记录、把 (模型, 子网格) 归成组；
// 可见性判断与间接命令都由 compute 完成，图形侧一条 indirect 提交收尾。
//
// 分组键从「模型」细化到「(模型, 子网格)」：同一个子网格被多个实体复用时，
// 它们的实例在缓冲里连续排列，一条命令带 instanceCount = N 就能一次画完（实例化），
// 而不是每实例一条命令。P2-5 起剔除不再按组：compute 逐实例测视锥/屏率，再把每组的
// 可见实例压成一段紧凑区间改写命令 —— 组只剩「一段索引区间 + 一段实例区间」的身份。
class SceneGpuBatch
{
public:
    // 与 shaders/scene_cull.comp、shaders/mrt.vert 的 InstanceRecord 逐字节一致。
    struct InstanceRecord
    {
        glm::mat4 model;
        glm::mat4 normal;
        glm::uvec4 flags;      // x=materialId y=materialValid z=alphaMasked w=albedoFallback
        glm::vec4 misc;        // x=alphaCutoff y=checkerScale z=terrainMode w=保留
        glm::vec4 uvTransform; // xy=缩放 zw=偏移
        glm::vec4 boundsMin;   // xyz=局部 AABB 下界 w=1 表示有效
        glm::vec4 boundsMax;
        glm::uvec4 draw;       // 保留：图形侧不再读，命令由 compute 写
        glm::mat4 previousModel; // 上一次结构上传时的世界矩阵（P2-2 收尾；P5-1 velocity 的数据源）
    };

    // 与 shaders/scene_cull.comp 的 GroupRecord 逐字节一致。
    // P2-5 起组只描述「合并网格里的一段索引区间 + 实例缓冲里的一段实例区间」：
    // 包围盒不再上传（剔除全部按实例做），组的 AABB 只有 CPU 侧的 primitive 缓冲还在用。
    struct GroupRecord
    {
        glm::uvec4 draw;       // x=firstIndex y=indexCount z=vertexOffset w=firstInstance
        glm::uvec4 counts;     // x=instanceCount y=materialSlot z=merged w=保留
    };

    // 与 shaders/scene_cull.comp 的 FrameData 一致。
    struct FrameData
    {
        glm::mat4 viewProjection;
        glm::mat4 view;          // Hi-Z 用：世界坐标 -> 线性视深度
        glm::vec4 planes[6];
        glm::vec4 screen;        // x=最小屏幕像素阈值 y=视口高度 z=投影 [1][1] w=保留
        glm::uvec4 params;     // x=groupCount y=保留 z=1 表示 Hi-Z 可用
    };

    // 外部实例入口：粒子/体素这类没有 ECS 实体的几何可以每帧填进来，走同一套分组、
    // 合并缓冲与 GPU 剔除。变换每帧不同，结构签名会随之变化（预期行为）。
    struct ExternalInstance
    {
        const Model* model = nullptr;
        uint32_t subMeshIndex = 0;
        uint32_t materialSlot = 0;
        glm::mat4 transform{ 1.0f };
    };

    // 剔除累计量：每帧 readStats 之后累加，退出时由 logCullSummary 打一行。
    // P2 要把剔除粒度从「组」换成「实例」，这一行就是改造前的对照读数。
    struct CullTotals
    {
        uint64_t frames = 0;
        uint64_t submitted = 0;
        uint64_t visibleGroups = 0;
        uint64_t culledGroups = 0;
        uint64_t visibleInstances = 0;
        uint64_t occlusionCulled = 0;
        uint64_t screenCulled = 0;
		uint64_t flattenMicros = 0; // 结构摊平累计微秒（P2 的「CPU 摊平时间」判据）
        uint64_t mergedBuildSamples = 0;   // 真正重建合并网格的样本数（首帧那类一次性成本）
    uint64_t externalReused = 0;    // 复用缓存记录的外部实例数（P2 增量重建）
        uint64_t instanceFrustumTested = 0;   // P2：做过视锥测试的实例数（每重建累加）
        uint64_t instanceFrustumVisible = 0;  // P2：其中在视锥内的实例数
        uint64_t gpuInstanceTested = 0;       // P2 第二步：GPU 侧实例视锥测试数（每帧累加）
        uint64_t gpuInstanceVisible = 0;      // P2 第二步：GPU 侧判定在视锥内的实例数
        // P2 度量：摊平分段耗时（微秒，只在重建结构的帧累加）。
        uint64_t structureSamples = 0;
        uint64_t collectMicros = 0;
        uint64_t mergedMicros = 0;
        uint64_t entityMicros = 0;
        uint64_t externalMicros = 0;
        uint64_t sortMicros = 0;
        uint64_t assembleMicros = 0;
        uint64_t mirrorMicros = 0;
        uint64_t entityAabbMicros = 0;
        uint64_t entityRecords = 0;
        uint64_t entityCount = 0;
        uint64_t entityBoundsMicros = 0;
        uint64_t entityNodeMicros = 0;
        uint64_t entityNormalMicros = 0;
        uint64_t externalNormalMicros = 0;
        uint64_t externalAabbMicros = 0;
        uint64_t externalRecordMicros = 0;
        uint64_t externalPushMicros = 0;
    };

    // 与 shaders/scene_cull.comp 的 StatsBuffer 同一块内存：GPU 缓冲是 4 个 uvec4 = 16 个
    // uint，但 host 结构体的字段顺序和 GPU 字下标并不一一对应（中间插了两个 CPU 字段）：
    //   words[0..3]  = stats[0]   = (submitted, visibleGroups, culledGroups, visibleInstances)
    //   words[4..7]  = stats[1]   = (visibleTriangles, visibleVertices, occlusionTests, occlusionCulled)
    //   words[8..9]  = stats[2].xy = (screenTests, screenCulled)；stats[2].zw 未用
    //   words[12..13]= stats[3].xy = (instanceTests, instanceVisible)；stats[3].zw 未用
    // 注意 groups / drawCalls 是 CPU 字段，位置 (6,7) 与 GPU 的遮挡计数重叠，所以
    // readStats 只按名字逐字读，绝不 memcpy 整块。
    struct Stats
    {
        uint32_t submitted = 0;
        uint32_t visibleGroups = 0;
        uint32_t culledGroups = 0;
        uint32_t visibleInstances = 0;
        uint32_t visibleTriangles = 0;
        uint32_t visibleVertices = 0;
        uint32_t groups = 0;
        uint32_t drawCalls = 0;
        uint32_t occlusionTests = 0;    // 做过 Hi-Z 测试的组数（滞后两帧）
        uint32_t occlusionCulled = 0;   // 其中被 Hi-Z 判定遮挡的组数
        uint32_t screenTests = 0;       // 做过屏幕空间误差测试的组数
        uint32_t screenCulled = 0;      // 其中因为投影太小被剔除的组数
        uint32_t externalInstances = 0; // 本帧来自外部（粒子/体素）的实例数
        uint32_t instanceTests = 0;     // GPU 侧做过实例视锥测试的实例数（滞后两帧）
        uint32_t instanceVisible = 0;   // 其中判定在视锥内的实例数
        uint32_t reserved = 0;          // 补到 16 个 uint，与 shader 的 stats[4] 对齐
    };
    static_assert(sizeof(Stats) == 16u * sizeof(uint32_t),
                  "Stats 必须与 scene_cull.comp 的 uvec4 stats[4] 同为 64 字节");

    explicit SceneGpuBatch(Device& device);
    ~SceneGpuBatch();

    SceneGpuBatch(const SceneGpuBatch&) = delete;
    SceneGpuBatch& operator=(const SceneGpuBatch&) = delete;

    // 绘制管线需要把实例缓冲当 set 3 绑定；没有可用实现时返回 VK_NULL_HANDLE。
    VkDescriptorSetLayout getInstanceSetLayout() const { return instanceSetLayout; }
    // A2：每个视图（主视图 / 平面反射）各有一份实例集合 —— binding 1 的 remap 表是视图私有的。
    VkDescriptorSet getInstanceSet(uint32_t frameInFlightIndex, uint32_t viewIndex = 0u) const
    {
        return frames[frameInFlightIndex].views[viewIndex].instanceSet;
    }

    bool isAvailable() const { return available; }
    // 结构已经摊平过至少一次（合并网格也就绪）：反射视图可以安全地跑自己的剔除了。
    bool hasInstances() const { return mergedActive && !instanceScratch.empty(); }

    // 这一帧是否该走 GPU 批：可用 + 开关打开 + （强制 或 实例数达到阈值）。
    bool useGpuPath(const FrameInfo& frameInfo) const;
    const Stats& getStats() const { return stats; }

    // 视图槽：0 = 主视图，1 = 平面反射（A2）。视图之间只共享实例/组缓冲，
    // 命令、帧数据、统计、组紧凑计数、remap 全部各自一份。
    static constexpr uint32_t kMaxViews = 5u;

    // 写实例记录 + 派发剔除。必须在几何 Pass 之前录制（无需附件）。
    void prepare(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);

    // A2：给指定视图做剔除。viewProjection / view 由调用方给（反射视图是镜像矩阵）。
    void prepareView(VkCommandBuffer commandBuffer, FrameInfo& frameInfo, uint32_t viewIndex,
                     const glm::mat4& viewProjection, const glm::mat4& view,
                     bool screenSpaceTest = true);

    // 绑定实例集合并提交 indirect 绘制。调用方需已绑定管线与 push constant。
    void recordDraw(VkCommandBuffer commandBuffer, FrameInfo& frameInfo);

    // A2：按视图提交。viewIndex 选命令缓冲，非 0 视图不计入主视角 telemetry。
    void recordDrawView(VkCommandBuffer commandBuffer, FrameInfo& frameInfo, uint32_t viewIndex);

    // 只绑合并网格的顶点/索引并发出该视图的 indirect 命令，供阴影这类自带管线的通道
    // 复用（调用方负责绑管线、绑 set 3、push 常量）。
    void drawMergedIndirect(VkCommandBuffer commandBuffer, FrameInfo& frameInfo, uint32_t viewIndex);

    // 每帧由引擎侧（粒子/体素系统）调用；传空表示本帧没有外部实例。
    void setExternalInstances(std::vector<ExternalInstance> instances)
    {
        externalInstances = std::move(instances);
    }

// P2 的入场尺子：立方体阵列。走外部实例注入路径，不建一万个实体 ——
// 那会撞材质槽上限（256），阵列只需要一个共享槽位。
// center 是阵列中心，由 Application 用相机前方算好传进来。
void setCubeField(std::shared_ptr<Model> model, uint32_t subMeshIndex, uint32_t materialSlot,
uint32_t count, float spacing, float distance)
{
cubeFieldModel = std::move(model);
cubeFieldSubMesh = subMeshIndex;
cubeFieldMaterialSlot = materialSlot;
cubeFieldCount = count;
cubeFieldSpacing = spacing > 0.0f ? spacing : 1.0f;
cubeFieldDistance = distance > 0.0f ? distance : 26.0f;
cubeFieldBuilt = 0;
}
    uint32_t getExternalInstanceCount() const
    {
        return static_cast<uint32_t>(externalInstances.size());
    }

    // 模型被销毁后释放缓存的子网格 AABB，避免指针复用带来的错配。
    void forgetModel(const Model* model);

    // F1-c：Hi-Z 遮挡剔除。在几何 Pass 之后调用，用本帧的 G-Buffer 位置重建金字塔；
    // 剔除用的是上一帧建好的那一份（错开一帧，相机帧间连续时几乎看不出差别）。
    void buildHiZ(VkCommandBuffer commandBuffer, const FrameInfo& frameInfo,
                  VkImageView positionView, VkExtent2D extent);

    // 登记某个帧槽位的 G-Buffer 位置视图。须在 init()/resize() 里调用：
    // Hi-Z 的构建描述符只在这时写，录制期间不再更新（否则会让在录的命令缓冲失效）。
    void setHiZSource(uint32_t frameInFlightIndex, VkImageView positionView, VkExtent2D extent);

    // 退出时打一行累计剔除统计（--stats-frame 调用）。回读值滞后两帧，这里只做观测。
    void logCullSummary() const;

    bool isHiZReady() const { return hizReady; }
    // T-08：持久场景（先只读骨架）。消费者尚未接入，只用于验证编号稳定与脏区间上传量。
    // P2 度量：每 N 帧把结构标记为失效一次，用来取"稳态重建"样本。
    uint32_t forceRebuildEvery = 0;   // P2 度量：强制重建周期（0 = 关）
    uint32_t forceRebuildTick = 0;
    void setForceRebuildEvery(uint32_t frames) { forceRebuildEvery = frames; }
    void setCubeFieldMotion(uint32_t microradiansPerFrame) { cubeFieldMotion = microradiansPerFrame; }
    // P2 实例级剔除：--no-instance-cull 时关掉（命令只按组剔除，remap 写成恒等）。
    void setInstanceCullEnabled(bool enabled) { instanceCullEnabled = enabled; }
    bool isInstanceCullEnabled() const { return instanceCullEnabled; }
    tiny::GpuScene& getGpuScene() { return gpuScene; }
    const tiny::GpuScene& getGpuScene() const { return gpuScene; }
    // P2：把 subMeshBoundsFor 的首次索引扫描挪到加载期，避免第一帧摊平被一次性成本虚高。
    void prewarmModelBounds(const Model& model);

private:
    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct ModelGroup
    {
        const Model* model = nullptr;
        uint32_t subMeshIndex = 0;
        uint32_t firstInstance = 0;
        uint32_t instanceCount = 0;
        glm::vec3 boundsMin{ 0.0f };
        glm::vec3 boundsMax{ 0.0f };
        bool hasBounds = false;
        uint32_t materialSlot = 0;
        uint32_t mergedFirstIndex = 0;
        int32_t mergedBaseVertex = 0;
        bool merged = false;
    };

    struct SubMeshBounds
    {
        glm::vec3 min{ 0.0f };
        glm::vec3 max{ 0.0f };
        bool valid = false;
    };

    struct CachedBounds
    {
        std::vector<SubMeshBounds> bounds;
        uint64_t lastFrame = 0;
    };

    struct MergedPlacement
    {
        uint32_t firstIndex = 0;
        uint32_t baseVertex = 0;
    };

    struct MergedMesh
    {
        GpuBuffer vertices;
        GpuBuffer indices;
        uint64_t signature = 0;
        bool valid = false;
    };

    // 每帧槽位一份 Hi-Z 金字塔：mip0 由 G-Buffer 位置生成，之后逐级 2x2 取最远。
    // 布局恒为 GENERAL，屏障全部手动管理（不进 RenderGraph 跟踪）。
    struct HiZFrame
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView fullView = VK_NULL_HANDLE;
        std::vector<VkImageView> mipViews;
        VkExtent2D extent{};
        uint32_t mipLevels = 0;
    };

    // 一个视图独占的每帧资源。两个视图共享实例缓冲与组表，剔除结果互不干扰。
    struct ViewResources
    {
        GpuBuffer commands;
        GpuBuffer frameData;
        GpuBuffer stats;
        // P2 实例级剔除：每组一个 uvec4（x = 可见实例数, y = remap 起点），CPU 每帧清零。
        GpuBuffer cullGroups;
        // P2 实例级剔除：前半是 remap（紧凑下标 -> 真实实例下标），后半是本帧名次。
        // 顶点着色器按前半取 InstanceRecord，所以图形 set 也绑这一份。
        GpuBuffer instanceRemap;
        VkDescriptorSet computeSet = VK_NULL_HANDLE;
        VkDescriptorSet instanceSet = VK_NULL_HANDLE;
    };

    struct FrameResources
    {
        GpuBuffer instances;
        GpuBuffer groups;
        ViewResources views[kMaxViews];
    };

    void createLayoutsAndPipeline();
    void destroyResources();

    void ensureCapacity(uint32_t instanceCapacity, uint32_t groupCapacity);
    void createFrameBuffers();
    void destroyFrameBuffers();
    void writeDescriptors();

    void createBuffer(GpuBuffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroyBuffer(GpuBuffer& buffer);

    bool ensureMergedMesh(const std::vector<const Model*>& models);
    void destroyMergedMesh();
    void uploadDeviceLocal(GpuBuffer& destination, const void* data, VkDeviceSize size);

    void ensureHiZ(VkExtent2D extent);
    void destroyHiZ();
    void writeHiZDescriptors();

    void buildInstances(FrameInfo& frameInfo);
    uint64_t computeStructureSignature(const FrameInfo& frameInfo) const;
    const std::vector<SubMeshBounds>& subMeshBoundsFor(const Model& model, uint64_t frameNumber);
    void readStats(uint32_t frameInFlightIndex, uint32_t viewIndex = 0u);
    void refreshDebugExternalInstances(FrameInfo& frameInfo);
	void refreshCubeFieldInstances(FrameInfo& frameInfo);

    Device& vulkanDevice;
    bool available = false;

    VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout instanceSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool computePool = VK_NULL_HANDLE;
    VkDescriptorPool graphicsPool = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> cullPipeline;

    // Hi-Z：构建/降采样两条 compute 管线（共用一套「采样源 + 存储图像」描述符布局）。
    VkDescriptorSetLayout hizSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout hizPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool hizPool = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> hizBuildPipeline;
    std::unique_ptr<ComputePipeline> hizDownsamplePipeline;
    std::array<HiZFrame, kFramesInFlight> hiz{};
    std::array<std::vector<VkDescriptorSet>, kFramesInFlight> hizSets{};
    std::array<VkImageView, kFramesInFlight> hizPositionViews{};
    VkSampler hizSampler = VK_NULL_HANDLE;
    VkExtent2D hizExtent{};
    bool hizReady = false;

    std::array<FrameResources, kFramesInFlight> frames{};
    uint32_t instanceCapacity = 0;
    uint32_t groupCapacity = 0;
    // 稳定帧快速路径：结构签名一致时跳过摊平与 instances/groups 上传。
    uint64_t structureSignature = 0;
    bool structureBuilt = false;
    uint32_t capacityGeneration = 0;
    std::array<uint64_t, kFramesInFlight> slotSignatures{};
    std::array<uint32_t, kFramesInFlight> slotGenerations{};
    uint32_t stableFrames = 0;
    // 最近一次重建时写进实例记录的「上一帧世界矩阵」覆盖情况：静止场景 moved 为 0。
    uint32_t historyInstances = 0;
    uint32_t historyMoved = 0;
    uint32_t structureRebuilds = 0;
    uint64_t structureMicros = 0;

    std::vector<InstanceRecord> instanceScratch;
    std::vector<ModelGroup> modelGroups;
    std::vector<glm::vec4> planeScratch;
    std::vector<std::vector<InstanceRecord>> buckets;
    std::vector<std::vector<uint32_t>> bucketEntities;
    std::unordered_set<uint32_t> warnedFallback;

    std::unordered_map<uint64_t, CachedBounds> modelBoundsCache;

    MergedMesh mergedMesh;
    std::unordered_map<uint64_t, MergedPlacement> mergedPlacements;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    bool mergedActive = false;
    bool warnedNoMergedData = false;

    bool mergedRebuiltThisCall = false;
    tiny::GpuScene gpuScene;   // T-08：持久原语/实例缓冲
    Stats stats;
    // 反射视图（view 1）自己的帧级累计：它不回读 triangles/vertices，也不参与摊平计时，
    // 只留「有多少组、提交了多少实例、实例可见率」这几项，用来判断多花的 dispatch 值不值。
    CullTotals reflectionTotals;
    CullTotals cullTotals;


    // P2 增量重建：外部实例的逐条记录缓存。变换没变就不重算逆矩阵转置与 AABB。
    // 键里带模型指针与子网格号，模型重新加载（指针变了）会自动失效。
    struct ExternalRecordCache
    {
        const Model* model = nullptr;
        uint32_t subMeshIndex = 0;
        uint32_t materialSlot = 0;
        glm::mat4 transform{ 1.0f };
        InstanceRecord record{};
        glm::vec3 worldCenter{ 0.0f };
        glm::vec3 worldHalf{ 0.0f };
        bool valid = false;
    };
    std::vector<ExternalRecordCache> externalRecordCache;
    std::vector<ExternalInstance> externalInstances;
// P2 尺子：立方体阵列的模板与参数（数量变化时才重建实例）。
std::shared_ptr<Model> cubeFieldModel;
uint32_t cubeFieldSubMesh = 0;
uint32_t cubeFieldMaterialSlot = 0;
uint32_t cubeFieldCount = 0;
float cubeFieldSpacing = 1.2f;
float cubeFieldDistance = 26.0f;
    uint32_t cubeFieldMotion = 0;    // P2 度量：每帧整体旋转量（微弧度），> 0 时阵列每帧重建
    // P2 实例级剔除：开着时命令的实例区间换成紧凑区间，顶点着色器经 remap 取记录；
    // 关掉是整体回退（--no-instance-cull），画面应与开着时逐像素一致。
    bool instanceCullEnabled = true;
    // A2：反射视图的剔除只打一次日志，说明它真的跑起来了。
    bool reportedReflectionView = false;

    // 合并网格重建时不能立刻销毁旧缓冲：同一帧里阴影视图已经把它们绑进命令缓冲，
    // 而几何视图的重建发生在那之后。旧缓冲按帧槽退役，等该槽的 fence 再次等到
    // 时再销毁（此时引用它的命令缓冲早已完成）。
    std::array<std::vector<GpuBuffer>, kFramesInFlight> retiredMergedBuffers;
    uint32_t currentFrameSlot = 0;
    void destroyRetiredMergedBuffers(uint32_t frameInFlightIndex);
// 阵列中心在第一次构建时按传入帧的 view 矩阵反算：启动期相机还没刷过 view。
uint32_t cubeFieldBuilt = 0;
    uint32_t debugExternalCount = 0;

    bool reportedFirstBatch = false;
};
