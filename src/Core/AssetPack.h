#pragma once

// .tpack 资产包（M1-B2）
//
// 目标不是"抄 Source 的 pack 格式"，而是解决三个具体问题：
//   1. 129 个资产文件、85MB 散落在 Resources 下，冷启动要逐个 stat/open；
//   2. 每个文件至少占一个簇，小文件（shader/材质）碎片化严重；
//   3. 打包后可以直接从内存喂给 tinygltf / stb，不必再走 fopen。
//
// 格式刻意做得很小：
//   [16B header][entryCount * 144B entry table][16B 对齐的 payload...]
//   entry.name 固定 128 字节（UTF-8，'/' 分隔，不含 "./" 前缀），offset/size 各 8 字节。
// 没有压缩、没有加密、没有 CRC：这些都可以后加，但一旦写进 v1 就再也改不掉了。
//
// 已知取舍：open() 会把整个包读进内存（Resources 全量 85MB 量级，可以接受）。
// 如果将来包涨到几百 MB，把 blob_ 换成文件句柄 + pread 即可，read() 的签名不变。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace assets
{

struct PackEntry
{
    std::string name;
    uint64_t offset = 0;   // 相对文件起点的绝对偏移，16 字节对齐
    uint64_t size = 0;
};

class AssetPack
{
public:
    // 打开并校验一个 .tpack。失败返回 nullptr，error 里带具体原因（给日志用）。
    static std::shared_ptr<AssetPack> open(const std::string& path, std::string& error);

    bool contains(const std::string& name) const;

    // 返回包内部的只读视图，不拷贝。命中失败返回 false。
    bool read(const std::string& name, const uint8_t*& data, size_t& size) const;

    const std::vector<PackEntry>& entries() const { return entries_; }
    const std::string& path() const { return sourcePath; }
    size_t totalBytes() const { return blob.size(); }

private:
    std::string sourcePath;
    std::vector<uint8_t> blob;
    std::vector<PackEntry> entries_;
};

struct PackInput
{
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// 写一个 .tpack。名字会先归一化（统一 '/'、去掉 "./" 与前导 '/'）。
bool writePack(const std::string& path, const std::vector<PackInput>& inputs, std::string& error);

// 把目录（递归）打成 .tpack，包内名字是相对 dir 的路径。
bool packDirectory(const std::string& dir, const std::string& outPath, std::string& error);

// 进程级挂载表：启动时挂包，之后只读。
//
// 为什么允许全局：贴图 / glTF / shader 的加载点分散在 RHI 与 Scene 层，
// 逐个把挂载表当参数传下去会污染十几个签名；而挂载表在启动后不再变化，
// 属于"只读配置"，与 EngineConfig 同类。写入只发生在 Application 初始化阶段。
class Mounts
{
public:
    void mount(std::shared_ptr<AssetPack> pack);
    void unmountAll();

    bool exists(const std::string& path) const;

    // 先查包，再退回文件系统。命中时把字节拷进 out。
    bool readFile(const std::string& path, std::vector<uint8_t>& out) const;

    // 只查大小，不拷贝（给需要预分配大小的读取方用）。
    bool stat(const std::string& path, size_t& size) const;

    size_t mountedCount() const { return packs.size(); }

private:
    std::vector<std::shared_ptr<AssetPack>> packs;
};

Mounts& mounts();

// 路径归一化：反斜杠转正斜杠、去掉 "./" 与前导 '/'。
std::string normalizeAssetName(const std::string& path);

// 资产包契约自检（不需要 Vulkan / 窗口）。
bool assetPackSelfTest();

} // namespace assets