#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

class Device;

// ===========================================================================
// ① VkPipelineCache 持久化（借鉴 Granite vulkan/pipeline_cache.cpp）
//
// 目标：第二次启动不再从零编译管线。本项目 GBuffer/SSAO/SSR/Shadow/TAA 等
// 十几条管线在 Debug 下每条都要走一遍完整编译，冷启动有肉眼可见的卡顿；
// 把 VkPipelineCache 的二进制存到磁盘、下次喂回 vkCreatePipelineCache 即可。
//
// 但这件事有三个坑，踩到任何一个都会得到"启动正常、画面偶尔花"的二进制，
// 而且验证层不会报错（驱动内部行为未定义）：
//
//   1. 旧 blob 不能跨驱动/跨卡复用。
//      判据是 VkPhysicalDeviceProperties::pipelineCacheUUID —— 驱动版本、
//      显卡型号、着色器编译器版本任一变化都会让它变。UUID 不一致必须整份
//      丢弃，而不是"试着用用看"。
//   2. 文件可能被截断。
//      上次写到一半被 Ctrl+C/崩溃，会留下半截文件。所以文件头记长度 +
//      FNV-1a 校验和，任一不符就丢弃。
//   3. 落盘必须原子。
//      直接覆盖唯一一份可用缓存，等于"崩溃一次就把优化成果清零"。
//      先写 .tmp 再 rename（Windows 上 rename 到已存在文件会失败，所以先
//      remove 目标）。
//
// 文件位置默认 %LOCALAPPDATA%\TinyEngine\ —— 缓存是机器本地、可随时删除的
// 产物，放进仓库既会被误提交，也会在多人共享时互相污染。
// ===========================================================================
class PipelineCache
{
public:
    // 拿不到 LOCALAPPDATA 时退回当前工作目录。
    static std::string defaultPath();

    // 参数不叫 path：ctor 体里再写 path 会被形参遮蔽，而形参刚被 move 走（空串）。
    PipelineCache(Device& device, std::string cachePath = defaultPath());
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // 传给 vkCreate*Pipelines 的 cache 参数。可能为 VK_NULL_HANDLE
    // （创建失败时的最后兜底），调用方不需要做特殊处理：传 VK_NULL_HANDLE
    // 在 Vulkan 里本来就是"不用缓存"，行为正确只是慢。
    VkPipelineCache get() const { return cache; }

    const std::string& getPath() const { return path; }

    // 自检/日志用。
    bool loadedFromDisk() const { return loaded; }
    uint64_t loadedBytes() const { return bytesLoaded; }
    uint64_t savedBytes() const { return bytesSaved; }

    // 幂等。析构里会自动调一次，也可以提前调（例如 device 销毁之前）。
    void save();

private:
    // 磁盘头。写裸结构体是安全的：magic + version 先行校验，任何字段布局
    // 变化都必须同时改 version。
    struct Header
    {
        char     magic[8];                  // "TINYPC1\0"
        uint32_t version;
        uint32_t vendorId;
        uint32_t deviceId;
        uint32_t driverVersion;
        uint32_t apiVersion;
        uint64_t payloadBytes;
        uint32_t payloadHash;               // FNV-1a(payload)
        uint8_t  pipelineCacheUUID[VK_UUID_SIZE];
    };

    static constexpr uint32_t kVersion = 1;
    static constexpr char     kMagic[8] = { 'T', 'I', 'N', 'Y', 'P', 'C', '1', '\0' };

    static uint32_t hashFNV1a(const void* data, size_t size);

    // 成功则把 payload 填进 out 并返回 true；任何校验失败都返回 false
    // 并打印一行原因（诊断"为什么没命中缓存"比省几毫秒更重要）。
    bool tryLoadFromDisk(std::vector<char>& outPayload);

    Device&     device;
    std::string path;

    VkPipelineCache cache = VK_NULL_HANDLE;
    bool      loaded = false;
    uint64_t  bytesLoaded = 0;
    uint64_t  bytesSaved = 0;
    bool      saved = false;
};