#pragma once

#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>

// ---------------------------------------------------------------------------
// TM-19：格式与能力表
//
// 为什么需要这张表：
//   以前「这个格式能不能当 storage image / 能不能 blit / 线性过滤支持吗」
//   全靠临时去查 `VkFormatProperties`，同一个判断在不同文件里写法不一，
//   改一个 G-Buffer 格式时没人知道它同时影响了哪些 usage（附件？采样？
//   storage？拷贝？）。这张表把「格式 -> 能力位」集中成一处，`--print-formats`
//   直接把它打出来，改格式前先看一眼就够。
//
// 名字为什么要自己写 switch：
//   Vulkan SDK 没有提供 VkFormat 的反查函数（`vkFormatToString` 之类不存在），
//   日志里出现 "format 37" 根本没法读。这里只覆盖引擎实际用到的十几个格式，
//   其余退回 `VK_FORMAT_<数字>`，比什么都不打印强。
// ---------------------------------------------------------------------------
namespace tiny::formats
{
    // 一个格式的「能力快照」。字段名与 VkFormatFeatureFlags 一一对应，
    // 但只保留引擎真正会判断的那几位，避免日志里全是噪声。
    struct Entry
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        const char* name = "VK_FORMAT_UNDEFINED";
        uint32_t bytesPerPixel = 0;      // 0 = 无固定像素大小（压缩格式或未收录）
        bool colorAttachment = false;    // 可当颜色附件（渲染目标）
        bool depthStencil = false;       // 可当深度/模板附件
        bool sampled = false;            // 可被采样（纹理读取）
        bool storage = false;            // 可当 storage image（计算着色器可写）
        bool blitSrc = false;            // 可作为 vkCmdBlitImage 的源
        bool blitDst = false;            // 可作为 vkCmdBlitImage 的目标
        bool linearFilter = false;       // 支持线性过滤（Bloom / TAA 的双线性前提）
    };

    // 枚举转名字（未收录的格式返回静态缓冲里的 "VK_FORMAT_<数字>"，不要保存这个指针）
    const char* formatName(VkFormat format);

    // 每像素字节数；未收录或压缩格式返回 0
    uint32_t bytesPerPixel(VkFormat format);

    // 用一份 VkFormatProperties 填出能力快照。只读 optimalTilingFeatures ——
    // 引擎所有中间目标都是 OPTIMAL tiling，linear 那一组用不上。
    Entry describe(VkFormat format, const VkFormatProperties& properties);

    // 批量查询（每个格式一次 vkGetPhysicalDeviceFormatProperties）
    std::vector<Entry> query(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& formats);

    // 打印成对齐的表；out 通常是 stdout
    void print(const std::vector<Entry>& entries, std::FILE* out);

    // 纯逻辑自检：名字映射、字节数、能力位映射、未收录格式的退化行为
    bool selfTest();
}