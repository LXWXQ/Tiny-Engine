#include "Core/Diagnostics.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>

namespace
{
    std::atomic<uint32_t> g_validationErrors{ 0 };
    std::atomic<uint32_t> g_validationWarnings{ 0 };

    std::mutex g_logMutex;
    std::ofstream g_jsonl;
    std::string g_jsonlPath;
    bool g_jsonlTried = false;

    std::string escapeJson(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
            }
        }
        return out;
    }
}

namespace diag
{

CompileTimeFeatures compileTimeFeatures()
{
    CompileTimeFeatures f;
#ifdef SENTINEL_ENABLE_GPU_TIMING
    f.gpuTiming = true;
#endif
#ifdef SENTINEL_ENABLE_VALIDATION_FEATURES
    f.validationFeatures = true;
#endif
#ifdef SENTINEL_ENABLE_BUILD_INFO
    f.buildInfo = true;
#endif
#ifdef SENTINEL_ENABLE_SCREENSHOT
    f.screenshot = true;
#endif
#ifdef SENTINEL_ENABLE_TRACY
    f.tracy = true;
#endif
#ifdef SENTINEL_ENABLE_RENDERDOC
    f.renderdoc = true;
#endif
    return f;
}

void configureValidationLog(const std::string& jsonlPath)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_jsonlPath = jsonlPath;
    g_jsonlTried = false;
    if (g_jsonl.is_open())
        g_jsonl.close();
}

void recordValidationMessage(int severity, const char* messageId, const char* message)
{
    if (severity == 1)
        g_validationErrors.fetch_add(1, std::memory_order_relaxed);
    else if (severity == 2)
        g_validationWarnings.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_jsonlPath.empty())
        return;

    if (!g_jsonlTried)
    {
        g_jsonlTried = true;
        g_jsonl.open(g_jsonlPath, std::ios::out | std::ios::trunc);
    }
    if (!g_jsonl.is_open())
        return;

    g_jsonl << "{\"severity\":\""
            << (severity == 1 ? "ERROR" : (severity == 2 ? "WARNING" : "INFO"))
            << "\",\"id\":\"" << escapeJson(messageId ? messageId : "")
            << "\",\"msg\":\"" << escapeJson(message ? message : "")
            << "\"}\n";
    g_jsonl.flush();
}

uint32_t validationErrorCount() { return g_validationErrors.load(std::memory_order_relaxed); }
uint32_t validationWarningCount() { return g_validationWarnings.load(std::memory_order_relaxed); }

void printValidationSummary()
{
    std::cout << "[Validation] errors=" << validationErrorCount()
              << " warnings=" << validationWarningCount() << "\n";
}

void printBuildInfo(const Options& options)
{
    const CompileTimeFeatures f = compileTimeFeatures();
    std::cout << "[BuildInfo] compiled: gpuTiming=" << (f.gpuTiming ? 1 : 0)
              << " validationFeatures=" << (f.validationFeatures ? 1 : 0)
              << " buildInfo=" << (f.buildInfo ? 1 : 0)
              << " screenshot=" << (f.screenshot ? 1 : 0)
              << " tracy=" << (f.tracy ? 1 : 0)
              << " renderdoc=" << (f.renderdoc ? 1 : 0)
              << "\n";
    std::cout << "[BuildInfo] runtime : gpuTiming=" << ((options.gpuTiming && f.gpuTiming) ? 1 : 0)
              << " validationBestPractices=" << ((options.validationBestPractices && f.validationFeatures) ? 1 : 0)
              << " validationSync=" << ((options.validationSync && f.validationFeatures) ? 1 : 0)
              << " validationGpuAssisted=" << ((options.validationGpuAssisted && f.validationFeatures) ? 1 : 0)
              << " validationJsonl=" << (options.validationJsonlPath.empty() ? 0 : 1)
              << "\n";
    std::cout << "[BuildInfo] policy: debug capabilities are off by default; "
                 "when off they allocate nothing and record no GPU commands\n";
}


void resetValidationCounts()
{
    g_validationErrors.store(0, std::memory_order_relaxed);
    g_validationWarnings.store(0, std::memory_order_relaxed);
}

bool selfTest(const std::string& jsonlPath)
{
    resetValidationCounts();
    configureValidationLog(jsonlPath);

    recordValidationMessage(1, "SENTINEL-TEST-ERROR", "synthetic error for diagnostics self-test");
    recordValidationMessage(2, "SENTINEL-TEST-WARNING", "synthetic warning for diagnostics self-test");

    const bool countsOk = (validationErrorCount() == 1 && validationWarningCount() == 1);

    bool jsonlOk = true;
    if (!jsonlPath.empty())
    {
        std::ifstream in(jsonlPath, std::ios::binary | std::ios::ate);
        jsonlOk = in.is_open() && in.tellg() > 0;
    }

    std::cout << "[Diag] diagnostics selfTest "
              << ((countsOk && jsonlOk) ? "PASS" : "FAIL")
              << " | errors=" << validationErrorCount()
              << " warnings=" << validationWarningCount()
              << " jsonl=" << (jsonlPath.empty() ? "off" : (jsonlOk ? "written" : "missing"))
              << "\n";

    configureValidationLog(std::string());   // 关闭落盘
    return countsOk && jsonlOk;
}
}