#include "Core/AssetPath.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "AssetPaths.h"   // CMake 生成的构建期路径宏（TINY_SHADER_DIR 等）

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace tiny::assetpath
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string g_executableDir;
        std::string g_shaderRoot;
        std::string g_resourceRoot;
        std::string g_configRoot;
        bool g_initialized = false;

        bool directoryExists(const fs::path& path)
        {
            std::error_code ec;
            return fs::is_directory(path, ec);
        }

        // 取第一个存在的候选；都不存在就返回空串（调用方据此保持原路径不变）
        std::string firstExisting(const std::vector<fs::path>& candidates)
        {
            for (const fs::path& candidate : candidates)
            {
                if (directoryExists(candidate))
                    return candidate.lexically_normal().string();
            }
            return {};
        }

        std::string exeDirectoryImpl()
        {
#if defined(_WIN32)
            char buffer[MAX_PATH * 4];
            const DWORD length = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
            if (length > 0 && length < sizeof(buffer))
                return fs::path(std::string(buffer, length)).parent_path().string();
            return {};
#else
            return {};
#endif
        }

        // 统一成 "/" 分隔，方便按锚点切分（Vulkan/CMake 生成的路径本来就用 "/"，
        // 但 Windows 手写路径可能是 "\\"）
        std::string normalizeSeparators(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            return path;
        }

        // 在 path 里找 anchor（如 "/shaders/"），把 anchor 之后的部分拼到 root 上。
        // anchor 前面那一截是"构建期根目录"，换机器后已经不存在了，丢掉即可。
        bool rebaseOnAnchor(const std::string& path, const char* anchor, const std::string& root, std::string& out)
        {
            if (root.empty())
                return false;
            const std::string haystack = normalizeSeparators(path);
            const std::string needle = anchor;
            const std::size_t position = haystack.find(needle);
            if (position == std::string::npos)
                return false;

            std::string suffix = haystack.substr(position + needle.size());
            // anchor 结尾自带斜杠，suffix 是 anchor 之后的第一段（例如 "bloom.frag.spv"）
            if (suffix.empty())
                return false;

            std::string joined = root;
            if (!joined.empty() && joined.back() != '/' && joined.back() != '\\')
                joined += '/';
            joined += suffix;
            std::error_code ec;
            if (!fs::exists(fs::path(joined), ec))
                return false;
            out = fs::path(joined).lexically_normal().string();
            return true;
        }
    }

    void initialize()
    {
        if (g_initialized)
            return;
        g_initialized = true;

        g_executableDir = exeDirectoryImpl();
        const fs::path exeDir(g_executableDir);
        const fs::path workingDir = fs::current_path();

        // shader：构建产物在 <repo>/build/shaders，exe 在 <repo>/build/Debug/
        // 也有人把 exe 和 shaders 摆在同一层，所以两种都试。
        g_shaderRoot = firstExisting({
            exeDir / "shaders",
            exeDir / ".." / "shaders",
            exeDir / ".." / ".." / "build" / "shaders",
            workingDir / "shaders",
            workingDir / "build" / "shaders",
        });

        // 资源：<repo>/Resources，exe 往上两级
        g_resourceRoot = firstExisting({
            exeDir / ".." / ".." / "Resources",
            exeDir / ".." / "Resources",
            exeDir / "Resources",
            workingDir / "Resources",
            workingDir / ".." / "Resources",
        });

        // 配置：<repo>/config，跟着资源根走；资源根找不到时再单独回溯
        g_configRoot = firstExisting({
            exeDir / ".." / ".." / "config",
            exeDir / ".." / "config",
            exeDir / "config",
            workingDir / "config",
        });
        if (g_configRoot.empty() && !g_resourceRoot.empty())
        {
            const fs::path sibling = fs::path(g_resourceRoot).parent_path() / "config";
            if (directoryExists(sibling))
                g_configRoot = sibling.lexically_normal().string();
        }
    }

    const std::string& shaderRoot() { return g_shaderRoot; }
    const std::string& resourceRoot() { return g_resourceRoot; }
    const std::string& configRoot() { return g_configRoot; }
    const std::string& executableDir() { return g_executableDir; }

    bool fileExists(const std::string& path)
    {
        std::error_code ec;
        return fs::is_regular_file(fs::path(path), ec);
    }

    std::string resolve(const std::string& path)
    {
        // 懒初始化：resolve 可能被静态初始化期的代码调用（例如 EngineConfig
        // 的全局路径变量），那时 Application 构造函数还没跑到 initialize()。
        initialize();

        if (path.empty())
            return path;

        // 0) 已经是真实文件：原样返回。这一条保证默认行为与引入本文件之前完全一致。
        if (fileExists(path))
            return path;

        const std::string normalized = normalizeSeparators(path);

        // 1) 显式协议
        struct Scheme { const char* prefix; const std::string* root; };
        const Scheme schemes[] = {
            { "asset://",  &g_resourceRoot },
            { "shader://", &g_shaderRoot },
            { "config://", &g_configRoot },
        };
        for (const Scheme& scheme : schemes)
        {
            const std::size_t prefixLength = std::strlen(scheme.prefix);
            if (normalized.rfind(scheme.prefix, 0) == 0)
            {
                if (scheme.root->empty())
                    return path;
                std::string joined = *scheme.root;
                if (!joined.empty() && joined.back() != '/')
                    joined += '/';
                joined += normalized.substr(prefixLength);
                return fs::path(joined).lexically_normal().string();
            }
        }
        if (normalized.rfind("file://", 0) == 0)
            return normalized.substr(std::strlen("file://"));

        // 2) 构建期绝对路径失效：按锚点重拼
        std::string rebased;
        if (rebaseOnAnchor(normalized, "/shaders/", g_shaderRoot, rebased))
            return rebased;
        if (rebaseOnAnchor(normalized, "/Resources/", g_resourceRoot, rebased))
            return rebased;
        if (rebaseOnAnchor(normalized, "/config/", g_configRoot, rebased))
            return rebased;

        // 3) 只剩文件名：挨个根目录试一次（TINY_RESOURCE_DIR "textures/x.png" 被拆开传进来时的兜底）
        const fs::path bare(normalized);
        if (bare.has_parent_path() == false)
        {
            for (const std::string* root : { &g_shaderRoot, &g_resourceRoot, &g_configRoot })
            {
                if (root->empty())
                    continue;
                const fs::path candidate = fs::path(*root) / bare;
                if (fileExists(candidate.string()))
                    return candidate.lexically_normal().string();
            }
        }

        return path;
    }

    void print(std::FILE* out)
    {
        if (out == nullptr)
            out = stdout;

        std::fprintf(out, "[Assets] 可执行目录: %s\n", g_executableDir.c_str());
        std::fprintf(out, "[Assets] shader 根  : %s\n", g_shaderRoot.empty() ? "(未找到)" : g_shaderRoot.c_str());
        std::fprintf(out, "[Assets] 资源根     : %s\n", g_resourceRoot.empty() ? "(未找到)" : g_resourceRoot.c_str());
        std::fprintf(out, "[Assets] 配置根     : %s\n", g_configRoot.empty() ? "(未找到)" : g_configRoot.c_str());

        // 构建期烧进来的三个路径：换目录后它们会全部失效，这里打出来方便对数
        std::fprintf(out, "[Assets] 构建期 shader  : %s（%s）\n", TINY_SHADER_DIR,
                     directoryExists(fs::path(TINY_SHADER_DIR)) ? "存在" : "已失效");
        std::fprintf(out, "[Assets] 构建期 资源    : %s（%s）\n", TINY_RESOURCE_DIR,
                     directoryExists(fs::path(TINY_RESOURCE_DIR)) ? "存在" : "已失效");
        std::fprintf(out, "[Assets] 构建期 配置    : %s（%s）\n", TINY_CONFIG_PATH,
                     fileExists(TINY_CONFIG_PATH) ? "存在" : "已失效");
    }

    bool selfTest()
    {
        bool ok = true;

        // 1) 协议前缀解析：不依赖真实文件系统，用"根目录一定存在"的前提构造
        initialize();
        if (!g_shaderRoot.empty())
        {
            const std::string resolved = resolve("shader://bloom_bright.frag.spv");
            if (resolved.find("shader://") != std::string::npos)
                ok = false;   // 没被解成真实路径
            if (!fileExists(resolved))
                ok = false;   // 解出来的东西必须真的存在
        }
        if (!g_resourceRoot.empty())
        {
            // 空后缀：解出来的应当是资源根本身（去掉末尾分隔符后比较）
            std::string root = resolve("asset://");
            while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
                root.pop_back();
            std::string expected = g_resourceRoot;
            while (!expected.empty() && (expected.back() == '/' || expected.back() == '\\'))
                expected.pop_back();
            if (root != expected)
                ok = false;
        }

        // 2) 不存在的普通路径：必须原样返回，不能偷偷换成别的东西
        const std::string bogus = "definitely/not/here/xyz.bin";
        if (resolve(bogus) != bogus)
            ok = false;

        // 3) 空串：原样返回
        if (!resolve("").empty())
            ok = false;

        // 4) 分隔符归一化：反斜杠路径也要能被锚点重拼救回来
        const std::string backslash = "C:\\old\\place\\shaders\\bloom_bright.frag.spv";
        const std::string rebased = resolve(backslash);
        if (rebased == backslash && !g_shaderRoot.empty())
            ok = false;   // 有 shader 根却没救回来

        std::printf("[Assets] selfTest %s | 协议解析 + 失效路径重拼 + 原样返回\n", ok ? "PASS" : "FAIL");
        return ok;
    }
}