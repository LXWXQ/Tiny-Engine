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