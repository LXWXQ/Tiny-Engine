#include "Core/StartupLog.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace tiny
{
    namespace
    {
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
}
