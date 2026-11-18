#include "RHI/PipelineCache.h"
#include "RHI/Device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    // 任何"缓存不可用"的原因都走这里，统一前缀方便 grep。
    void warn(const char* reason)
    {
        std::cout << "[PipelineCache] 丢弃旧缓存：" << reason << std::endl;
    }

    // 真写一个探针文件再删：Windows 上目录的权限位/access() 结论经常和
    // ofstream 实际结果不一致（受限账户、沙盒、被策略收紧的 %LOCALAPPDATA%）。
    bool directoryIsWritable(const std::filesystem::path& dir)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return false;

        const std::filesystem::path probe = dir / ".write_probe";
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.put('\0');
        const bool ok = static_cast<bool>(out);
        out.close();
        std::filesystem::remove(probe, ec);
        return ok;
    }
}

std::string PipelineCache::defaultPath()
{
    namespace fs = std::filesystem;

    // 优先 %LOCALAPPDATA%：这是 Windows 上放"每用户、可随时清除"的产物
    // 的标准位置（清理磁盘时会被工具正确识别）。
    fs::path preferred;
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr && *localAppData != '\0')
        preferred = fs::path(localAppData) / "TinyEngine";
    else
        preferred = fs::current_path() / "TinyEngine";

    // 「目录建得出来」不等于「写得进去」：受限环境里 ofstream 会直接 EACCES，
    // 那样每次启动都白跑一趟再打一行「缓存不持久化」。所以先探一下，
    // 写不进去就退到工作目录 —— 缓存有没有比放哪儿重要得多。
    if (directoryIsWritable(preferred))
        return (preferred / "pipeline_cache.bin").string();
    return (fs::current_path() / "pipeline_cache.bin").string();
}

uint32_t PipelineCache::hashFNV1a(const void* data, size_t size)
{
    // FNV-1a 32bit：够用、无依赖、几十行内可复现。
    // 目的不是防篡改，而是抓"写到一半被截断"。
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

PipelineCache::PipelineCache(Device& device, std::string cachePath)
    : device(device)
    , path(std::move(cachePath))
{
    std::vector<char> payload;
    const bool haveBlob = tryLoadFromDisk(payload);

    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    createInfo.initialDataSize = payload.size();
    createInfo.pInitialData = payload.empty() ? nullptr : payload.data();

    VkResult result = vkCreatePipelineCache(device.getDevice(), &createInfo, nullptr, &cache);

    if (result != VK_SUCCESS)
    {
        // 驱动拒绝这份 blob 也是**合法**结果（例如驱动内部做了升级，UUID
        // 却没变）。此时必须按"完全没有缓存"重建 —— 缓存是纯优化，
        // 绝不能因此让启动失败。
        std::cout << "[PipelineCache] 驱动拒绝了旧 blob（VkResult=" << result << "），改为空缓存启动" << std::endl;

        createInfo.initialDataSize = 0;
        createInfo.pInitialData = nullptr;
        result = vkCreatePipelineCache(device.getDevice(), &createInfo, nullptr, &cache);
    }

    if (result != VK_SUCCESS || cache == VK_NULL_HANDLE)
    {
        // 连空 cache 都建不出来：退化成"不用缓存"，管线创建走 VK_NULL_HANDLE。
        cache = VK_NULL_HANDLE;
        std::cout << "[PipelineCache] 禁用管线缓存（vkCreatePipelineCache 失败，VkResult=" << result << "）" << std::endl;
        return;
    }

    loaded = haveBlob;
    bytesLoaded = haveBlob ? payload.size() : 0;

    if (loaded)
        std::cout << "[PipelineCache] 命中旧缓存：" << bytesLoaded << " 字节 <- " << path << std::endl;
    else
        std::cout << "[PipelineCache] 空缓存启动，退出时写入 " << path << std::endl;
}

PipelineCache::~PipelineCache()
{
    save();

    if (cache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(device.getDevice(), cache, nullptr);
        cache = VK_NULL_HANDLE;
    }
}

bool PipelineCache::tryLoadFromDisk(std::vector<char>& outPayload)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;   // 首次运行，正常路径，不打印

    const std::streamsize fileSize = file.tellg();
    if (fileSize < static_cast<std::streamsize>(sizeof(Header)))
    {
        warn("文件比文件头还短（上次写入被中断）");
        return false;
    }

    file.seekg(0, std::ios::beg);
    Header header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file)
    {
        warn("读取文件头失败");
        return false;
    }

    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0)
    {
        warn("magic 不匹配（不是本程序写的文件）");
        return false;
    }

    if (header.version != kVersion)
    {
        warn("版本不匹配");
        return false;
    }

    // —— 最关键的一道守卫 ——
    // 驱动/显卡/着色器编译器变了就一定换 UUID。跨 UUID 复用 blob 是
    // 未定义行为：验证层不会报，画面可能花。
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &props);
    if (std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) != 0)
    {
        warn("pipelineCacheUUID 不匹配（驱动或显卡变了）");
        return false;
    }

    const uint64_t expectedPayload = static_cast<uint64_t>(fileSize) - sizeof(Header);
    if (header.payloadBytes != expectedPayload)
    {
        warn("payload 长度与文件头不符（文件被截断）");
        return false;
    }

    outPayload.resize(static_cast<size_t>(header.payloadBytes));
    if (!outPayload.empty())
        file.read(outPayload.data(), static_cast<std::streamsize>(outPayload.size()));
    if (!file)
    {
        warn("读取 payload 失败");
        outPayload.clear();
        return false;
    }

    if (hashFNV1a(outPayload.data(), outPayload.size()) != header.payloadHash)
    {
        warn("payload 校验和不符（内容损坏）");
        outPayload.clear();
        return false;
    }

    return true;
}

void PipelineCache::save()
{
    if (saved)
        return;

    saved = true;   // 无论成功失败都只尝试一次：进程退出路径上不要重试。

    if (cache == VK_NULL_HANDLE)
        return;

    size_t payloadSize = 0;
    if (vkGetPipelineCacheData(device.getDevice(), cache, &payloadSize, nullptr) != VK_SUCCESS || payloadSize == 0)
        return;

    std::vector<char> payload(payloadSize);
    VkResult result = vkGetPipelineCacheData(device.getDevice(), cache, &payloadSize, payload.data());
    if (result == VK_INCOMPLETE)
    {
        // 期间又有管线被创建（比如热重载）：按新长度再取一次。
        payload.resize(payloadSize);
        result = vkGetPipelineCacheData(device.getDevice(), cache, &payloadSize, payload.data());
    }
    if (result != VK_SUCCESS)
    {
        std::cout << "[PipelineCache] 读取缓存数据失败，本次不写盘" << std::endl;
        return;
    }
    payload.resize(payloadSize);

    if (payloadSize <= 8)
    {
        // 只有驱动自己的空头：没有值得保存的内容，不覆盖可能更好的旧文件。
        return;
    }

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kVersion;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &props);
    header.vendorId = props.vendorID;
    header.deviceId = props.deviceID;
    header.driverVersion = props.driverVersion;
    header.apiVersion = props.apiVersion;
    header.payloadBytes = payload.size();
    header.payloadHash = hashFNV1a(payload.data(), payload.size());
    std::memcpy(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE);

    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path target(path);
    if (target.has_parent_path())
        fs::create_directories(target.parent_path(), ec);   // 失败无所谓，后面写文件会报

    fs::path tempPath = target;
    tempPath += ".tmp";

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            std::cout << "[PipelineCache] 无法写入 " << tempPath.string() << "（缓存不持久化）" << std::endl;
            return;
        }
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!out)
        {
            std::cout << "[PipelineCache] 写入 " << tempPath.string() << " 失败" << std::endl;
            return;
        }
    }

    // 原子替换。Windows 的 rename 不能覆盖已存在文件，所以先删目标。
    // 中间这一小段窗口里目标文件不存在，但 .tmp 是完整的，最坏情况是
    // "下次启动没有缓存"，不会变成"下次启动读到半截文件"。
    fs::remove(target, ec);
    ec.clear();
    fs::rename(tempPath, target, ec);
    if (ec)
    {
        std::cout << "[PipelineCache] 替换 " << path << " 失败：" << ec.message() << std::endl;
        return;
    }

    bytesSaved = payload.size();
    std::cout << "[PipelineCache] 已保存 " << bytesSaved << " 字节 -> " << path << std::endl;
}