#include "src/Core/Application.h"
#include "src/Core/JobSystem.h"
#include "src/Core/Diagnostics.h"
#include "src/Core/StartupLog.h"
#include "src/Scene/EntityHandle.h"
#include "src/Scene/Trace.h"
#include "src/Scene/Model.h"
#include "src/Core/AssetPack.h"
#include "src/Editor/SceneSerializer.h"
#include "src/Terrain/TerrainSculptDelta.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>
#include <filesystem>

// ---------------------------------------------------------------------------
// 命令行参数
//   （无）                        正常跑窗口，关闭窗口退出
//   --selftest [N]                跑 N 帧（默认 600）后退出，用于验证层回归
//   --no-bloom                    自检时关掉 Bloom
//   --no-ssao                     自检时关掉 SSAO
//   --no-taa                      自检时关掉 TAA（Pass 仍运行，只输出当前帧）
//   --cross-queue-test            启用 TD-02 跨队列验收探针（需配合 --selftest）
//   --resize-test                 自检中连续缩放窗口并切换动态分辨率
//   --material-stress             自检中每帧分配/延迟归还材质槽位（TD-04）
//   --subresource-test            启用 TD-05 多 mip/layer 正向探针
//   --subresource-error-test      期望 RenderGraph 拒绝 mip0 写、mip1 读
//   --compute-test                启用 D-5 compute probe 独立回归（默认关闭）
//   --config-test                 EngineConfig 版本迁移 / 损坏配置保护自检
//   --diagnostics-test [path]     诊断管线自检（合成消息 -> 计数 + JSONL 落盘）
//   --device-fault-test           VK_EXT_device_fault 自检（健康设备上查询应成功）
//   --screenshot <path>           目标帧截图写 PNG（+ --screenshot-frame <n>）
//   --capture-frame <n>           RenderDoc 在第 n 帧抓帧（需注入 RenderDoc）
//   --render-scale <0.25..1.0>    动态分辨率初始倍率（需配合 --selftest）
//   --editor                      以场景编辑器模式启动（默认关闭，行为与运行时一致）
//   --screenshot-viewport         截图拍编辑器离屏视口（纯 3D，不含 UI）；需配合 --editor
//   --empty-scene                 不加载 Sponza / 天空盒，只留相机（编辑器调试单物体时用）
//   --terrain-test                启动时创建持久地形，用于地形 G-Buffer/截图回归
//   --hidden                      不显示窗口（自检/截图/回归这类无人值守运行默认开启）
//   --minimized                   启动后最小化窗口（最小化会跳过渲染，只适合看日志）
//   --visible                     强制显示窗口，覆盖 --hidden 的默认行为
//
// 本文件被编译两次，产出两个可执行文件（见 CMakeLists.txt）：
//   TinyEngine.exe   运行时模式
//   TinyEditor.exe 编辑器模式（TINY_DEFAULT_EDITOR=1，等价于 --editor）
//
//   ---- P0：调试 / 剖析能力（随用随开，默认关闭）----
//   --build-info                  打印本二进制编译进的 & 本次开启的调试能力后退出
//   --gpu-timing                  每 pass GPU 计时，退出时写 gpu_timing.csv
//   --gpu-timing-csv <path>       指定 CSV 路径（隐含 --gpu-timing）
//   --validation-features <list>  启用验证层增强：bp(=best practices) / sync(=同步校验)，逗号分隔或 all
//   --validation-jsonl <path>     把验证层消息写成 JSONL（默认不落盘，也不写 stderr）
//
// 为什么需要自检开关：
//   "关掉某个 Pass"是最容易漏验证层错误的分支 —— 中间目标整帧没人写
//   （布局一直停在 UNDEFINED），后面的 Pass 却仍然声明要采样它。
//   而 ImGui 的复选框没法在无人值守的验证流程里点，所以把开关做成
//   命令行参数，让这些分支也能进"验证层 0 字节"的回归检查。
//
// 这两个开关走的是和 UI 复选框完全相同的代码路径（EngineSettings）。
//
// 调试能力的开关模型（见 docs/60-debug-profiling/zero-overhead.md）：
//   是否"编译进来"由 CMake option -> TINY_ENABLE_* 宏决定；
//   是否"本次开启"由上面的 flag 决定。关闭时不分配、不插命令、不落盘。
// ---------------------------------------------------------------------------
// UX-02（B1）：帮助入口。参数表是数据，不写在注释里 —— 注释会烂掉，表格能被 grep 和测试引用。
// 新增参数时必须同时在 kUsageLines 里加一行，tools/Validate.ps1 的 b1-help 用例会核对总数。
struct UsageLine
{
    const char* group;
    const char* flags;
    const char* text;
};

const UsageLine kUsageLines[] = {
    { "运行模式", "--selftest [N]", "跑 N 帧后退出（默认 600），无人值守回归用" },
    { "运行模式", "--editor", "以场景编辑器模式启动（TinyEditor.exe 默认开启）" },
    { "运行模式", "--empty-scene", "不加载 Sponza / 天空盒，只留相机" },
    { "运行模式", "--runtime-mode", "编辑器启动后直接进入运行模式（角色 + 第三人称相机）" },
    { "窗口", "--hidden / --visible", "不显示 / 强制显示窗口（自检与截图默认 --hidden）" },
    { "窗口", "--minimized", "启动后最小化（最小化会跳过渲染）" },
    { "截图", "--screenshot <path>", "把第 --screenshot-frame 帧写成 PNG" },
    { "截图", "--screenshot-frame <n>", "截图/抓帧的目标帧号" },
    { "截图", "--screenshot-viewport", "截图只拍编辑器离屏视口（纯 3D，不含 UI）" },
    { "截图", "--capture-frame <n>", "RenderDoc 在第 n 帧抓帧（需注入 RenderDoc）" },
    { "截图", "--render-scale <f>", "动态分辨率初始倍率 0.25..1.0" },
    { "渲染", "--no-bloom / --no-ssao / --no-taa / --no-water / --no-ssr", "关掉 Bloom / SSAO / TAA / 水面 / 水面 SSR（同 UI 复选框）" },
    { "渲染", "--gpu-scene / --no-gpu-scene", "强制开 / 关 GPU 驱动提交" },
    { "渲染", "--gpu-scene-min-px <px>", "GPU 提交的屏幕空间剔除阈值（像素）" },
    { "渲染", "--gpu-scene-external <n>", "外部实例数（粒子/体素共用的提交入口）" },
    { "渲染", "--no-merged-buffers", "不合并静态网格缓冲" },
    { "渲染", "--no-hiz", "关掉 Hi-Z 遮挡剔除" },
    { "渲染", "--no-shadows", "关掉方向光级联阴影（排查 / 对比用）" },
    { "渲染", "--no-terrain-batch", "关掉地形合批（逐 chunk 绘制）" },
    { "地形", "--terrain-test", "启动时创建持久地形，用于地形回归" },
    { "地形", "--streaming-world [r]", "流送世界，半径 r（1..24，默认 3）" },
    { "地形", "--voxel-world [r]", "体素世界（32x32x32 方块 chunk），半径 r（1..12，默认 1）" },
    { "地形", "--voxel-glass-demo", "在体素世界出生点前摆交叉玻璃（V-01 透明排序目测）" },
    { "地形", "--particle-demo", "在原点放一个粒子喷泉（前向透明 billboard 目测）" },
    { "地形", "--cloud-demo", "启用体积云并把相机放到云层下方（建议配 --streaming-world）" },
    { "地形", "--sun-demo", "启用太阳盘与镜头光晕（建议配 --streaming-world）" },
    { "地形", "--vegetation-demo", "在地形上种草与树，相机贴近地面（建议配 --streaming-world 3）" },
    { "地形", "--fog-demo", "在相机前方放一个局部体积雾盒（建议配 --streaming-world 3）" },
    { "地形", "--streaming-fly [speed]", "沿 +X 直线飞行，用于流送/卸载回归" },
    { "地形", "--streaming-fly-radius <r>", "绕圆飞行半径（0 = 直线）" },
    { "地形", "--streaming-budget <ms>", "每帧地形生成预算（毫秒）" },
    { "地形", "--no-origin-rebase", "关掉浮动原点（默认开启）" },
    { "地形", "--origin-threshold <u>", "浮动原点触发距离（世界单位，最低 64）" },
    { "自检", "--cross-queue-test", "TD-02 跨队列探针" },
    { "自检", "--resize-test", "自检中连续缩放窗口并切换动态分辨率（需配 --selftest）" },
    { "自检", "--material-stress", "每帧分配/延迟归还材质槽位（TD-04）" },
    { "自检", "--subresource-test / --subresource-error-test", "TD-05 正向 / 反例探针" },
    { "自检", "--compute-test", "D-5 compute probe 独立回归" },
    { "自检", "--config-test", "配置迁移 / 损坏配置保护自检" },
    { "自检", "--diagnostics-test [path]", "诊断管线自检（合成消息 -> 计数 + JSONL）" },
    { "自检", "--device-fault-test", "VK_EXT_device_fault 自检" },
    { "自检", "--jobtest", "JobSystem 自检" },
    { "自检", "--handle-test", "实体句柄契约自检（不需要 Vulkan / 窗口）" },
    { "自检", "--trace-test", "射线契约自检（不需要 Vulkan / 窗口）" },
    { "自检", "--gltf-node-test", "glTF 节点树契约自检（不需要 Vulkan / 窗口）" },
    { "资产", "--pack-test", "资产包契约自检（不需要 Vulkan / 窗口）" },
    { "资产", "--pack-build <dir> <out>", "把目录递归打成 .tpack 资产包" },
        { "序列化", "--material-json-test", "材质 JSON 只存非默认字段的自检（不需要 Vulkan / 窗口）" },
        { "地形", "--terrain-sculpt-test", "地形雕刻差分与 JSON 往返的自检（不需要 Vulkan / 窗口）" },
    { "剖析", "--build-info", "打印本二进制的调试能力后退出" },
    { "剖析", "--gpu-timing", "每 pass GPU 计时，退出时写 gpu_timing.csv" },
    { "剖析", "--gpu-timing-csv <path>", "指定计时 CSV 路径（隐含 --gpu-timing）" },
    { "剖析", "--validation-features <list>", "验证层增强：bp / sync / all" },
    { "剖析", "--validation-jsonl <path>", "验证层消息写 JSONL" },
    { "剖析", "--update-profile", "每 60 帧打印 Scene::update 分段耗时" },
    { "帮助", "--help / -h", "打印这张表并退出（退出码 0）" },
};

void printUsage(const char* exeName)
{
        std::string displayName = "TinyEngine";
    if (exeName != nullptr)
        displayName = std::filesystem::path(exeName).filename().string();
    std::printf("Tiny Engine - %s\n\n", displayName.c_str());
    std::printf("用法: TinyEngine.exe [选项]\n\n");
    const char* currentGroup = nullptr;
    for (const UsageLine& line : kUsageLines)
    {
        if (currentGroup == nullptr || std::strcmp(currentGroup, line.group) != 0)
        {
            std::printf("%s\n", line.group);
            currentGroup = line.group;
        }
        std::printf("  %-34s %s\n", line.flags, line.text);
    }
    std::printf("\n未知参数会以退出码 2 结束，并在 stderr 打印一行说明。\n");
}

int main(int argc, char** argv)
{
    // stdout 关闭缓冲：日志重定向到文件时也能实时看到加载进度，
    // 而不是等到崩溃/退出才一次性落盘（排查卡死时非常有用）。
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    system("chcp 65001 > nul");
#endif

    bool selfTest = false;
    bool noBloom = false;
    bool noTAA = false;
    bool noSSAO = false;
    bool noWater = false;
    bool noSSR = false;
    bool noGpuScene = false;
    bool forceGpuScene = false;
    float gpuSceneMinPixels = 0.0f;
    uint32_t gpuSceneExternal = 0u;
    bool noMergedBuffers = false;
    bool noHiZ = false;
    bool noShadows = false;
    bool noTerrainBatch = false;
    // F2-a：流送世界。radius 默认 3（7x7=49 个 chunk 实体，低于地形 GPU 选择的 64 个 slot 上限）。
    bool streamingWorld = false;
    int32_t streamingRadius = 3;
    bool streamingFly = false;
    float streamingFlySpeed = 12.0f;
    // F2-c: origin rebase. Threshold is in world units.
    bool originRebase = true;
    float originThreshold = 0.0f;
    float streamingFlyRadius = 0.0f;
    float streamingBudgetMs = 0.0f;
    bool jobTest = false;
    bool handleTest = false;
    bool traceTest = false;
    bool gltfNodeTest = false;
    bool packTest = false;
    bool materialJsonTest = false;
    bool terrainSculptTest = false;
    std::string packBuildDir;
    std::string packBuildOut;
    bool crossQueueTest = false;
    bool resizeTest = false;
    bool materialStress = false;
    bool subresourceTest = false;
    bool subresourceErrorTest = false;
    bool computeTest = false;
    bool configTest = false;
    bool deviceFaultTest = false;
    bool diagTest = false;
    bool windowHiddenFlag = false;
    bool windowMinimizedFlag = false;
    bool windowVisibleFlag = false;
    // TinyEditor.exe 用 TINY_DEFAULT_EDITOR=1 编译，于是"打开就是编辑器"；
    // TinyEngine.exe 不带这个宏，行为与引入编辑器之前完全一致。
    // 两个 exe 共用同一份 main.cpp，差异只在这一个宏上。
#ifdef TINY_DEFAULT_EDITOR
    bool editorMode = true;
#else
    bool editorMode = false;
#endif
    std::string diagTestPath;
    float renderScale = 1.0f;
    int  selfTestFrames = 600;

    diag::Options diagOptions;
    // UX-02（B1）：未知参数不再静默忽略。--selftest 的帧数是唯一一个"值不跟 ++i 走"的
    // 分支，所以用一个一次性标志把它放过去，别的多余 token 一律记进 unknownArgs。
    std::vector<std::string> unknownArgs;
    bool selfTestFramesPending = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            printUsage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (selfTestFramesPending && !arg.empty() &&
            std::isdigit(static_cast<unsigned char>(arg[0])) != 0)
        {
            selfTestFramesPending = false;
            continue;
        }
        if (arg == "--selftest")
        {
            selfTest = true;
            selfTestFramesPending = true;
            if (i + 1 < argc)
            {
                try { selfTestFrames = std::stoi(argv[i + 1]); }
                catch (...) { /* 参数不是数字就沿用默认帧数 */ }
            }
        }
        else if (arg == "--no-bloom") noBloom = true;
        else if (arg == "--no-ssao")  noSSAO = true;
        else if (arg == "--no-taa")   noTAA = true;
        else if (arg == "--no-water") noWater = true;
        else if (arg == "--no-ssr")   noSSR = true;
        else if (arg == "--no-gpu-scene") noGpuScene = true;
        else if (arg == "--gpu-scene") forceGpuScene = true;
        else if (arg == "--no-merged-buffers") noMergedBuffers = true;
        else if (arg == "--no-hiz") noHiZ = true;
        else if (arg == "--no-shadows") noShadows = true;
        else if (arg == "--no-terrain-batch") noTerrainBatch = true;
        else if (arg == "--gpu-scene-min-px" && i + 1 < argc)
        {
            try { gpuSceneMinPixels = std::stof(argv[i + 1]); }
            catch (...) { /* 参数不是数字就忽略 */ }
            ++i;
        }
        else if (arg == "--gpu-scene-external" && i + 1 < argc)
        {
            try { gpuSceneExternal = static_cast<uint32_t>(std::stoul(argv[i + 1])); }
            catch (...) { /* 参数不是数字就忽略 */ }
            ++i;
        }
        else if (arg == "--streaming-world")
        {
            streamingWorld = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                try { streamingRadius = std::stoi(argv[i + 1]); }
                catch (...) { /* 参数不是数字就忽略 */ }
                ++i;
            }
            // 世界必须在 FirstApp 构造里创建（Scene 已就绪），所以同时写进 Options。
            diagOptions.streamingWorld = true;
            diagOptions.streamingRadius = streamingRadius;
        }
        else if (arg == "--voxel-world")
        {
            diagOptions.voxelWorld = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                try { diagOptions.voxelRadius = std::stoi(argv[i + 1]); }
                catch (...) { /* 参数不是数字就忽略 */ }
                ++i;
            }
        }
        else if (arg == "--voxel-glass-demo")
        {
            diagOptions.voxelGlassDemo = true;
        }
        else if (arg == "--particle-demo")
        {
            diagOptions.particleDemo = true;
        }
        else if (arg == "--cloud-demo")
        {
            diagOptions.cloudDemo = true;
        }
        else if (arg == "--sun-demo")
        {
            diagOptions.sunDemo = true;
        }
        else if (arg == "--vegetation-demo")
        {
            diagOptions.vegetationDemo = true;
        }
        else if (arg == "--fog-demo")
        {
            diagOptions.fogDemo = true;
        }
        else if (arg == "--streaming-fly")
        {
            streamingFly = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                try { streamingFlySpeed = std::stof(argv[i + 1]); }
                catch (...) { /* 参数不是数字就忽略 */ }
                ++i;
            }
            diagOptions.streamingFly = true;
            diagOptions.streamingFlySpeed = streamingFlySpeed;
        }
        else if (arg == "--no-origin-rebase")
        {
            originRebase = false;
        }
        else if (arg == "--origin-threshold")
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                try { originThreshold = std::stof(argv[i + 1]); }
                catch (...) { }
                ++i;
            }
        }
        else if (arg == "--streaming-fly-radius")
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                try { streamingFlyRadius = std::stof(argv[i + 1]); }
                catch (...) { }
                ++i;
            }
        }        else if (arg == "--streaming-budget")
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                try { streamingBudgetMs = std::stof(argv[i + 1]); }
                catch (...) { }
                ++i;
            }
        }
        else if (arg == "--jobtest")  jobTest = true;
        else if (arg == "--handle-test") handleTest = true;
        else if (arg == "--trace-test") traceTest = true;
    else if (arg == "--gltf-node-test") gltfNodeTest = true;
        else if (arg == "--pack-test") packTest = true;
        else if (arg == "--material-json-test") materialJsonTest = true;
        else if (arg == "--terrain-sculpt-test") terrainSculptTest = true;
        else if (arg == "--pack-build")
        {
            if (i + 2 < argc)
            {
                packBuildDir = argv[i + 1];
                packBuildOut = argv[i + 2];
                i += 2;
            }
        }
        else if (arg == "--cross-queue-test") crossQueueTest = true;
        else if (arg == "--resize-test") resizeTest = true;
        else if (arg == "--material-stress") materialStress = true;
        else if (arg == "--subresource-test") subresourceTest = true;
        else if (arg == "--subresource-error-test") subresourceErrorTest = true;
        else if (arg == "--compute-test") computeTest = true;
        else if (arg == "--config-test") configTest = true;
        else if (arg == "--device-fault-test") deviceFaultTest = true;
        else if (arg == "--diagnostics-test")
        {
            diagTest = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') diagTestPath = argv[++i];
        }
        else if (arg == "--render-scale" && i + 1 < argc)
        {
            try { renderScale = std::stof(argv[++i]); }
            catch (...) { /* keep default */ }
        }
        else if (arg == "--editor") editorMode = true;
        else if (arg == "--screenshot-viewport") diagOptions.screenshotViewport = true;
        else if (arg == "--update-profile") diagOptions.updateProfile = true;
        else if (arg == "--empty-scene") diagOptions.emptyScene = true;
        else if (arg == "--runtime-mode") diagOptions.runtimeMode = true;
        else if (arg == "--terrain-test") diagOptions.terrainTest = true;
        else if (arg == "--hidden") windowHiddenFlag = true;
        else if (arg == "--minimized") windowMinimizedFlag = true;
        else if (arg == "--visible") windowVisibleFlag = true;
        else if (arg == "--build-info") diagOptions.printBuildInfo = true;
        else if (arg == "--gpu-timing") diagOptions.gpuTiming = true;
        else if (arg == "--gpu-timing-csv" && i + 1 < argc)
        {
            diagOptions.gpuTimingCsvPath = argv[++i];
            diagOptions.gpuTiming = true;
        }
        else if (arg == "--screenshot" && i + 1 < argc)
        {
            diagOptions.screenshot = true;
            diagOptions.screenshotPath = argv[++i];
        }
        else if (arg == "--capture-frame" && i + 1 < argc)
        {
            try { diagOptions.captureFrame = std::stoi(argv[++i]); } catch (...) {}
        }
        else if (arg == "--screenshot-frame" && i + 1 < argc)
        {
            try { diagOptions.screenshotFrame = std::stoi(argv[++i]); } catch (...) {}
            diagOptions.screenshot = true;
        }
        else if (arg == "--validation-jsonl" && i + 1 < argc)
        {
            diagOptions.validationJsonlPath = argv[++i];
        }
        else if (arg == "--validation-features" && i + 1 < argc)
        {
            std::stringstream ss(argv[++i]);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                if (token == "bp" || token == "best-practices" || token == "all")
                    diagOptions.validationBestPractices = true;
                if (token == "sync" || token == "synchronization" || token == "syncval" || token == "all")
                    diagOptions.validationSync = true;
                if (token == "printf" || token == "debug-printf" || token == "all")
                    diagOptions.validationDebugPrintf = true;
                if (token == "gpuav" || token == "gpu-assisted")   // 单独指定：GPU-AV 较重且与 bindless 有兼容性风险，不并入 all
                    diagOptions.validationGpuAssisted = true;
            }
        }
        else
        {
            unknownArgs.push_back(arg);
        }
    }

    // UX-02（B1）：未知参数在创建窗口/设备之前就退出，避免"参数写错却照常启动"。
    if (!unknownArgs.empty())
    {
        for (const std::string& unknown : unknownArgs)
            std::cerr << "unknown argument: " << unknown << std::endl;
        std::cerr << "用 --help 查看全部参数。" << std::endl;
        return 2;
    }

    // 无人值守运行默认不显示窗口：每次跑测试都把窗口弹到最前会打断用户。
    // 交互式启动（无参数 / --editor）仍然正常显示；--minimized 需要窗口存在
    // 才能最小化，所以它优先于隐藏；--visible 可以强制显示。
    const bool unattendedRun = selfTest || crossQueueTest || resizeTest || materialStress ||
        subresourceTest || subresourceErrorTest || computeTest || deviceFaultTest ||
        diagOptions.screenshot || diagOptions.terrainTest;
    // --minimized 只对交互式启动有意义：最小化会命中跳过渲染的分支，而无人值守
    // 自检/截图必须持续出帧，因此该组合退化为隐藏窗口（不弹窗、不阻塞）。
    const bool minimizedIgnored = windowMinimizedFlag && unattendedRun;
    diagOptions.windowHidden = !windowVisibleFlag &&
        (windowHiddenFlag || minimizedIgnored || (unattendedRun && !editorMode));
    diagOptions.windowMinimized = windowMinimizedFlag && !unattendedRun;
    if (minimizedIgnored && !windowVisibleFlag)
        std::cout << "[Window] --minimized ignored for unattended run; staying hidden" << std::endl;

    // UX-04：致命错误写到可执行文件旁边，双击启动（没有控制台）时也能找到。
    std::string errorLogDirectory;
    {
        std::error_code exeEc;
        const std::filesystem::path exePath = std::filesystem::absolute(argv[0], exeEc);
        if (!exeEc && !exePath.parent_path().empty())
            errorLogDirectory = exePath.parent_path().string();
    }

    try
    {
        // --build-info：不需要 Vulkan / 窗口，直接打印能力清单后退出。
        if (diagOptions.printBuildInfo)
        {
            diag::printBuildInfo(diagOptions);
            return EXIT_SUCCESS;
        }

        if (configTest)
            return EngineConfig::selfTest() ? EXIT_SUCCESS : EXIT_FAILURE;

        if (diagTest)
            return diag::selfTest(diagTestPath) ? EXIT_SUCCESS : EXIT_FAILURE;

        if (jobTest)
        {
            JobSystem jobSystem;
            return jobSystem.selfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        if (handleTest)
            return scene::entityHandleSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;

        if (traceTest)
            return scene::traceContractSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;

        if (gltfNodeTest)
            return modelNodeSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;

        if (packTest)
            return assets::assetPackSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;

        if (materialJsonTest)
            return editor::SceneSerializer::materialJsonSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;

        if (terrainSculptTest)
        {
            const bool deltaOk = terrain::terrainSculptDeltaSelfTest();
            const bool jsonOk = editor::SceneSerializer::terrainSculptJsonSelfTest();
            return (deltaOk && jsonOk) ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        if (!packBuildDir.empty())
        {
            std::string packError;
            if (!assets::packDirectory(packBuildDir, packBuildOut, packError))
            {
                std::cerr << "[AssetPack] 打包失败: " << packError << '\n';
                return EXIT_FAILURE;
            }
            std::cout << "[AssetPack] 已写出 " << packBuildOut << '\n';
            return EXIT_SUCCESS;
        }

        // M1-B2：存在资产包就先挂上，之后所有贴图/模型加载都会优先从包内读。
        {
            std::string packError;
            if (auto pack = assets::AssetPack::open("Resources/assets.tpack", packError))
            {
                assets::mounts().mount(pack);
                std::cout << "[AssetPack] 已挂载 Resources/assets.tpack | 条目 " << pack->entries().size() << '\n';
            }
        }

        diag::configureValidationLog(diagOptions.validationJsonlPath);

#ifdef TINY_DEFAULT_EDITOR
        std::cout << "[Tiny] TinyEditor build (editor mode on by default)\n";
#endif
        // 让引擎知道是否编辑器模式：编辑器默认不加载 Sponza
        diagOptions.editorMode = editorMode;

        FirstApp app(diagOptions);

        if (editorMode) app.setEditorEnabled(true);
        if (noBloom) app.setBloomEnabled(false);
        if (noSSAO) app.setSSAOEnabled(false);
        if (noTAA) app.setTAAEnabled(false);
        if (noWater) app.setWaterEnabled(false);
        if (noSSR) app.setSSREnabled(false);
        if (noGpuScene) app.setGpuSceneEnabled(false);
        if (forceGpuScene) app.setGpuSceneForced(true);
        if (noMergedBuffers) app.setGpuMergedBuffers(false);
        if (noHiZ) app.setHiZEnabled(false);
        if (noShadows) app.setShadowsEnabled(false);
        if (noTerrainBatch) app.setTerrainBatchEnabled(false);
        if (gpuSceneMinPixels > 0.0f) app.setGpuSceneMinScreenPixels(gpuSceneMinPixels);
        if (gpuSceneExternal > 0u) app.setGpuSceneExternalDebugCount(gpuSceneExternal);
        if (streamingWorld) app.setStreamingWorldEnabled(true, streamingRadius);
        if (streamingFly) app.setStreamingFly(true, streamingFlySpeed);
        if (!originRebase) app.setOriginRebaseEnabled(false);
        if (originThreshold > 0.0f) app.setOriginRebaseThreshold(originThreshold);
        if (streamingFlyRadius > 0.0f) app.setStreamingFlyRadius(streamingFlyRadius);
        if (streamingBudgetMs > 0.0f) app.setStreamingBudget(streamingBudgetMs);
        if (diagOptions.updateProfile) app.setUpdateProfiling(true);
        if (crossQueueTest) app.setCrossQueueTestEnabled(true);
        if (resizeTest) app.setResizeTestEnabled(true);
        if (materialStress) app.setMaterialStressEnabled(true);
        if (subresourceTest) app.setSubresourceTestEnabled(true);
        if (computeTest) app.setComputeProbeEnabled(true);
        app.setRenderScale(renderScale);

        if (deviceFaultTest)
            return app.runDeviceFaultSelfTest();

        if (subresourceErrorTest)
            return app.runSubresourceErrorSelfTest();

        if (selfTest)
            return app.runHeadlessSelfTest(selfTestFrames);

        app.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "致命错误: " << e.what() << std::endl;
        // UX-04：双击启动没有可读的控制台，所以把致命错误落到文件里。
        const std::string logPath = tiny::writeStartupErrorLog("启动/运行", e.what(), errorLogDirectory);
        if (!logPath.empty())
            std::cerr << "详情已写入 " << logPath << "（没用控制台启动时看这个文件）" << std::endl;

        // UX-04：交互式启动失败时再弹原生提示；--hidden / --selftest / 截图等
        // 无人值守运行保持静默，否则回归会卡在等人的“确定”上。
        if (!unattendedRun && !diagOptions.windowHidden)
        {
            tiny::showStartupErrorBox(
                "Tiny Engine 启动失败",
                tiny::formatStartupErrorMessage("启动/运行", e.what(), logPath));
        }
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}