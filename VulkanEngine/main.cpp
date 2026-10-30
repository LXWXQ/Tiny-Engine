#include "src/Core/Application.h"
#include "src/Core/JobSystem.h"
#include "src/Core/Diagnostics.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <sstream>

// ---------------------------------------------------------------------------
// 命令行参数
//   （无）                        正常跑窗口，关闭窗口退出
//   --selftest [N]                跑 N 帧（默认 600）后退出，用于验证层回归
//   --no-bloom                    自检时关掉 Bloom
//   --no-ssao                     自检时关掉 SSAO
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
//   是否"编译进来"由 CMake option -> SENTINEL_ENABLE_* 宏决定；
//   是否"本次开启"由上面的 flag 决定。关闭时不分配、不插命令、不落盘。
// ---------------------------------------------------------------------------
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
    bool noSSAO = false;
    bool jobTest = false;
    bool crossQueueTest = false;
    bool resizeTest = false;
    bool materialStress = false;
    bool subresourceTest = false;
    bool subresourceErrorTest = false;
    bool computeTest = false;
    bool configTest = false;
    bool deviceFaultTest = false;
    bool diagTest = false;
    std::string diagTestPath;
    float renderScale = 1.0f;
    int  selfTestFrames = 600;

    diag::Options diagOptions;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--selftest")
        {
            selfTest = true;
            if (i + 1 < argc)
            {
                try { selfTestFrames = std::stoi(argv[i + 1]); }
                catch (...) { /* 参数不是数字就沿用默认帧数 */ }
            }
        }
        else if (arg == "--no-bloom") noBloom = true;
        else if (arg == "--no-ssao")  noSSAO = true;
        else if (arg == "--jobtest")  jobTest = true;
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

        diag::configureValidationLog(diagOptions.validationJsonlPath);

        FirstApp app(diagOptions);

        if (noBloom) app.setBloomEnabled(false);
        if (noSSAO) app.setSSAOEnabled(false);
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
        std::cerr << "致命错误: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}