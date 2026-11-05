#include "Core/StartupLog.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace tiny
{
    namespace
    {
#if defined(_WIN32)
        std::wstring utf8ToWide(const std::string& text)
        {
            if (text.empty())
                return {};

            const int byteCount = static_cast<int>(text.size());
            UINT codePage = CP_UTF8;
            DWORD flags = MB_ERR_INVALID_CHARS;
            int length = MultiByteToWideChar(codePage, flags, text.data(), byteCount, nullptr, 0);
            if (length <= 0)
            {
                // 旧配置/控制台消息不一定是合法 UTF-8；退回系统 ANSI 代码页，
                // 至少不要因为编码问题把启动失败吞掉。
                codePage = CP_ACP;
                flags = 0;
                length = MultiByteToWideChar(codePage, flags, text.data(), byteCount, nullptr, 0);
            }
            if (length <= 0)
                return {};

            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(codePage, flags, text.data(), byteCount, wide.data(), length);
            return wide;
        }
#endif

        std::string timestampNow()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char buffer[32]{};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
            return std::string(buffer);
        }
    }

    std::string writeStartupErrorLog(const std::string& stage,
                                     const std::string& message,
                                     const std::string& directory)
    {
        std::error_code ec;
        const std::filesystem::path cwd = directory.empty()
            ? std::filesystem::current_path(ec)
            : std::filesystem::path(directory);
        if (ec)
            return {};

        const std::filesystem::path path = cwd / "startup-error.log";
        std::ofstream file(path, std::ios::app);
        if (!file)
            return {};

        file << "[" << timestampNow() << "] " << stage << std::endl;
        file << message << std::endl;
        file << "cwd: " << cwd.string() << std::endl;
        file << "提示：本文件与 build/validation 下的日志一起提供，能定位大多数启动失败。" << std::endl;
        file << std::endl;
        if (!file)
            return {};

        return path.string();
    }

    std::string formatStartupErrorMessage(const std::string& stage,
                                          const std::string& message,
                                          const std::string& logPath)
    {
        std::string text;
        text += "阶段：" + stage + "\n";
        text += "原因：" + message + "\n\n";
        text += "诊断日志：";
        text += logPath.empty() ? "（未能写入，目录可能不可写）" : logPath;
        text += "\n\n下一步：\n";
        text += "  1. 用 --help 查看启动参数\n";
        text += "  2. 确认显卡驱动支持 Vulkan 1.3 与必需特性\n";
        text += "  3. 把 startup-error.log 与 build/validation 下的日志一起提供";
        return text;
    }

    void showStartupErrorBox(const std::string& title, const std::string& message)
    {
#if defined(_WIN32)
        const std::wstring wideTitle = utf8ToWide(title.empty() ? "Tiny Engine" : title);
        const std::wstring wideMessage = utf8ToWide(message);
        if (wideMessage.empty())
            return;

        // MB_TOPMOST：双击启动时窗口可能在后台，错误提示必须让用户看见。
        // MB_SETFOREGROUND：避免 MessageBox 被其它窗口盖住。
        std::fprintf(stderr, "[Startup] showing native error dialog\n");
        const int result = ::MessageBoxW(nullptr, wideMessage.c_str(), wideTitle.c_str(),
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        if (result == 0)
        {
            // 无交互桌面（服务/自动化环境）时 MessageBox 会立刻失败；
            // 不要静默，至少让 stderr 能解释“为什么没弹窗”。
            std::fprintf(stderr, "[Startup] MessageBoxW failed (GetLastError=%lu)\n",
                static_cast<unsigned long>(::GetLastError()));
        }
#else
        (void)title;
        (void)message;
#endif
    }
}
