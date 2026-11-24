#pragma once
#include <string>
#include <cstdint>

// ============================================================================
// 引擎诊断基础设施（P0）
//
// 设计原则见 docs/60-debug-profiling/zero-overhead.md：
//   * 所有调试 / 剖析能力「随用随开」，关闭时对运行时性能零影响；
//   * 「是否编译进来」由 CMake option -> TINY_ENABLE_* 宏决定；
//   * 「本次是否开启」由命令行 flag -> diag::Options 决定。
//
// 本文件不引入任何全局构造 / 分配：Options 由调用方（main）持有，
// 验证层计数只在回调真正被调用时才累加。
// ============================================================================

namespace diag
{
    // ---- 编译期能力（CMake option -> 宏）----
    struct CompileTimeFeatures
    {
        // --stats-pipelines：退出时打印管线数量、建管线耗时、状态去重结果（TM-06）。
        bool statsPipelines = false;

        // --stats-scratch：退出时打印每帧暂存区的分配次数、字节数、扩容与刷新次数（TM-07）。
        bool statsScratch = false;

        bool gpuTiming = false;
        bool validationFeatures = false;
        bool buildInfo = false;
        bool screenshot = false;
        bool tracy = false;
        bool renderdoc = false;
    };
    CompileTimeFeatures compileTimeFeatures();

    // ---- 运行期选项（main 在构造 FirstApp 前填好）----
    struct Options
    {
        bool printBuildInfo = false;

        // 验证层
        bool validationBestPractices = false;
        bool validationSync = false;
        bool validationDebugPrintf = false;
        bool validationGpuAssisted = false;
        std::string validationJsonlPath;   // 空 = 不落盘

        // GPU 计时
        // --stats-pipelines：退出时打印管线数量、建管线耗时、状态去重结果（TM-06）。
        bool statsPipelines = false;

        // --stats-scratch：退出时打印每帧暂存区的分配次数、字节数、扩容与刷新次数（TM-07）。
        bool statsScratch = false;

        // --alias-report：退出时打印渲染图资源别名候选对与预估可省显存（TM-08，只读报告）。
        bool aliasReport = false;

        // --parallel-prepare：Pass 的准备阶段（访问去重 + 声明校验 + 组装附件描述）
        // 交给作业系统并行，录制仍然是串行的（TM-10）。默认关闭 —— 先用
        // --stats-rendergraph 看两边的耗时再决定要不要翻默认。
        bool parallelPrepare = false;

        // --stats-rendergraph：退出时打印准备 / 录制各自的每帧耗时（TM-10）。
        bool statsRenderGraph = false;

        // --stats-frame：退出时打印每帧屏障条数与管线绑定 / 冗余绑定（T-10）。
        // 这是 P0 度量基线的一半：屏障条数证明 T-03 能省多少，冗余绑定率是 T-09 的上限。
        bool statsFrame = false;
    // T-09 的整体回退：关掉管线绑定缓存，每次绑定都照发。
    bool noPipelineCache = false;
    // T-05：Pass 剔除默认关（本引擎暂无「输出无人读」的 Pass）；--pass-cull 打开，
    // --no-pass-cull 覆盖它，--pass-cull-dry-run 只打印不跳过。
    bool passCull = false;
    bool noPassCull = false;
    bool passCullDryRun = false;
    // T-04：跨帧 RT 池。默认关 —— 实测只在「尺寸换回旧值」时命中，空闲图要占 250 MiB 以上。
        bool renderTargetPool = false;
        bool statsQueue = false;
        bool noAsyncSplit = false;
// P2 的入场尺子：n 个立方体排成立方格点，走外部实例注入路径。
uint32_t cubeField = 0;
    // T-04b：同帧目标合成一张。默认开：实测更快（lights-512 +5.4%）且少 15 张图；
    // 它自带「本帧等上一帧」的等待，用 --no-single-frame-targets 整体回退。
    bool singleFrameTargets = true;
    bool noSingleFrameTargets = false;
    // T-04b：本帧图形工作等上一帧图形工作完成 —— 让「同帧目标合成一张」成立的前提。
    bool serializeFrames = false;
    // 只用于证明「两帧确实重叠」：故意关掉上面的等待，让跨帧竞争显形（配同步验证层用）。
    bool noSerializeFrames = false;
    // T-04：跨帧 RT 池。默认关 —— 实测只在
    // --camera <px,py,pz,pitch,yaw>：覆盖 config 里保存的相机位姿。基准场景与截图
    // 金标必须与"上次会话存下的相机"无关，否则同一份代码两次跑出的画面不同。
    bool hasCameraOverride = false;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraPitch = 0.0f;
    float cameraYaw = 0.0f;
        // TM-19：打印本机对本引擎用到的那些格式的能力（颜色/深度/采样/存储/blit/线性过滤），
        // 打印完就退出 —— 改 G-Buffer 格式或加 storage image 之前先跑一次。
        bool printFormats = false;
    // TM-16：打印三个资产根与热重载登记情况后继续跑（不退出）
    bool printAssets = false;
    // TM-16：每帧检查 .spv 的 mtime，变了就重建对应管线
    bool hotReload = false;
    // TM-17：打印 latched 事件（交换链重建 / 渲染目标尺寸）的当前值与订阅数
    bool printEvents = false;
    // TM-18：探测惰性分配（transient attachment）能否在本机启用，打印后退出
    bool probeTransient = false;
    // F6-02：自动曝光。GPU 统计平均对数亮度，CPU 按帧适应；
    // 默认关 —— 默认画面必须与没有这个功能时逐像素一致。
    bool autoExposure = false;

        // TM-13：点光源与聚簇光照。lightStress > 0 时铺这么多盏灯（0 = 不加灯），
        // 用来量"512 盏灯下前向还能不能跑满 60 FPS"。
        uint32_t lightStress = 0;
        // --light-stress-radius：压力场铺开的半径（米）。默认 18 米是"贴脸"密度，
        // 每簇塞上百盏灯；调到 100~200 米就是"稀疏分布"，聚簇才有意义的那一档。
        float lightStressRadius = 60.0f;

    // 萤火虫演示：区域内 N 个有形状、有颜色的光源（默认关，绝不动默认画面）。
    // 形状由 ParticlePass 的 billboard 画，光照由 LightSources 喂给聚簇/逐灯路径。
    uint32_t fireflies = 0;
    float firefliesRadius = 12.0f;
        bool clusteredLighting = false;
        bool statsClusters = false;
        // TM-13 第二步：建簇放到 GPU（图形流里一次 dispatch），CPU 侧不再建表。
        bool clusteredGpuBuild = false;

        bool gpuTiming = false;
        std::string gpuTimingCsvPath = "gpu_timing.csv";

        // 截图 / 抓帧（P1）
        bool screenshot = false;
        std::string screenshotPath = "screenshot.png";
        int  screenshotFrame = -1;   // -1 = 自动（selftest 最后一帧）

// --screenshot-viewport：拍编辑器离屏视口而不是交换链。
// 交换链里含 ImGui（面板、菜单栏），视口纹理只有 3D 画面 ——
// 写文档 / 做视觉回归时后者才是想要的。
bool screenshotViewport = false;

// --update-profile：每 60 帧打印一次 Scene::update() 的分阶段 CPU 耗时。
// 关闭时零开销（连计时器都不取）。
bool updateProfile = false;

// --empty-scene：不加载 Sponza / 天空盒，只留相机。
// 编辑器调试单个物体时用 —— Sponza 的包围盒包住整个场景，
// 会干扰"点选/包围盒/手柄"这类交互的观察。
bool emptyScene = false;

// --runtime-mode：编辑器启动后直接进入运行模式（便于截图/演练角色）。
bool runtimeMode = false;

// --terrain-test：启动时创建一块持久地形，用于渲染/截图回归。
bool terrainTest = false;

// --streaming-world [radius]：启动即创建流送地形世界（相机周围按块加载）。
// 与 --terrain-test 的区别：那个是固定的单块地形，这个是一圈独立 chunk。
bool streamingWorld = false;
// --voxel-world [radius]：启动即建一个 32^3 方块体素世界（F3）。
// radius 是 chunk 环半径，1 = 3x3 = 9 块。
bool voxelWorld = false;
int32_t voxelRadius = 1;
// --voxel-glass-demo：在体素世界里摆一组交叉玻璃，用于目测 V-01 的
// 透明排序与混合（配合 --screenshot 做视觉回归）。
bool voxelGlassDemo = false;
// --particle-demo：在空场景原点放一个粒子发射器，用于目测前向透明
// billboard（配合 --screenshot 做视觉回归）。
bool particleDemo = false;
// --cloud-demo：启用体积云场景组件，并把相机放到云层下方（配合
// --streaming-world 可同时看到地形与云层）。
bool cloudDemo = false;
// --sun-demo：启用太阳盘与镜头光晕，并把方向光/相机调到能看见太阳的
// 角度（配合 --streaming-world 可同时看到地形轮廓）。
bool sunDemo = false;
// --vegetation-demo：启用 scene.vegetation，把相机贴近地面看草与树
// （建议配合 --streaming-world 3，否则空场景没有地形可长）。
bool vegetationDemo = false;
// --fog-demo：在相机前方放一个可放置的局部体积雾盒（effects.fogVolume），
// 用来目测盒体边缘、噪声与高度衰减（建议配合 --streaming-world 3）。
bool fogDemo = false;
int32_t streamingRadius = 3;
// --streaming-fly [speed]：自检/无人值守时让相机沿圆周自动飞行，
// 用来验证 chunk 的加载与卸载（不飞就只会在原地加载一圈）。
bool streamingFly = false;
float streamingFlySpeed = 12.0f;

// 是否以编辑器模式启动。编辑器默认不加载 Sponza（见 Scene::init 的说明），
// 但 TinyEngine.exe 的常规运行仍然加载，视觉回归基线不受影响。
bool editorMode = false;

// --hidden / --minimized：无人值守运行（自检、截图、回归）不要抢焦点。
// 隐藏窗口仍有真实 HWND 与客户区尺寸，swapchain / 渲染 / 截图都不受影响；
// 最小化会命中 handleWindowMaintenance() 的跳过分支，只适合"看日志"的运行。
bool windowHidden = false;
bool windowMinimized = false;
        int  captureFrame = -1;      // RenderDoc in-app 抓帧的帧号（P1-2）
    };

    // 验证层消息落盘路径（空 = 关闭）。启动时调用一次。
    void configureValidationLog(const std::string& jsonlPath);

    // Device 的 debug 回调调用。severity: 1=error, 2=warning, 3=info/verbose。
    // 绝不写 stderr —— 保持「stderr 0 字节」的验收基线。
    void recordValidationMessage(int severity, const char* messageId, const char* message);
    uint32_t validationErrorCount();
    uint32_t validationWarningCount();
    void printValidationSummary();

    // --build-info：打印「编译进来的」与「本次开启的」能力清单。
    void printBuildInfo(const Options& options);

    // 重置验证层计数（自检用）。
    void resetValidationCounts();

    // 诊断管线自检（不需要 Vulkan）：写入合成消息并校验计数 + JSONL 落盘。
    // 用于在没有真实验证层消息时也能回归"结构化输出 + 落盘"这条链路。
    bool selfTest(const std::string& jsonlPath);
}
