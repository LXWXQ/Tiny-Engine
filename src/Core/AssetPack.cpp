#include "Core/AssetPack.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace assets
{
namespace
{
    constexpr uint32_t kPackVersion = 1;
    constexpr size_t kNameBytes = 128;
    constexpr size_t kAlign = 16;

    struct PackHeader
    {
        char magic[4];
        uint32_t version;
        uint32_t entryCount;
        uint32_t reserved;
    };
    static_assert(sizeof(PackHeader) == 16, "pack header must stay 16 bytes");

    struct DiskEntry
    {
        char name[kNameBytes];
        uint64_t offset;
        uint64_t size;
    };
    static_assert(sizeof(DiskEntry) == 144, "pack entry must stay 144 bytes");

    uint64_t alignUp(uint64_t value)
    {
        return (value + (kAlign - 1)) & ~static_cast<uint64_t>(kAlign - 1);
    }

    bool readWholeFile(const std::string& path, std::vector<uint8_t>& out)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            return false;
        const std::streamoff size = in.tellg();
        if (size < 0)
            return false;
        out.resize(static_cast<size_t>(size));
        in.seekg(0);
        if (size > 0)
            in.read(reinterpret_cast<char*>(out.data()), size);
        return static_cast<bool>(in) || in.eof();
    }
} // namespace

std::string normalizeAssetName(const std::string& path)
{
    std::string name = path;
    for (char& ch : name)
    {
        if (ch == '\\')
            ch = '/';
    }
    while (name.size() >= 2 && name[0] == '.' && name[1] == '/')
        name.erase(0, 2);
    while (!name.empty() && name.front() == '/')
        name.erase(0, 1);
    return name;
}

std::shared_ptr<AssetPack> AssetPack::open(const std::string& path, std::string& error)
{
    error.clear();
    auto pack = std::make_shared<AssetPack>();
    if (!readWholeFile(path, pack->blob))
    {
        error = "无法读取 " + path;
        return nullptr;
    }
    if (pack->blob.size() < sizeof(PackHeader))
    {
        error = "文件小于包头: " + path;
        return nullptr;
    }

    PackHeader header{};
    std::memcpy(&header, pack->blob.data(), sizeof(header));
    if (std::memcmp(header.magic, "TPAK", 4) != 0)
    {
        error = "magic 不是 TPAK: " + path;
        return nullptr;
    }
    if (header.version != kPackVersion)
    {
        error = "包版本不支持: " + std::to_string(header.version) + " (期望 " +
                std::to_string(kPackVersion) + ")";
        return nullptr;
    }

    const uint64_t tableBytes = static_cast<uint64_t>(header.entryCount) * sizeof(DiskEntry);
    if (sizeof(PackHeader) + tableBytes > pack->blob.size())
    {
        error = "entry table 越界: " + path;
        return nullptr;
    }

    pack->entries_.reserve(header.entryCount);
    for (uint32_t i = 0; i < header.entryCount; ++i)
    {
        DiskEntry disk{};
        std::memcpy(&disk, pack->blob.data() + sizeof(PackHeader) + i * sizeof(DiskEntry), sizeof(disk));

        PackEntry entry{};
        const char* terminator = static_cast<const char*>(std::memchr(disk.name, 0, kNameBytes));
        entry.name.assign(disk.name, terminator ? static_cast<size_t>(terminator - disk.name) : kNameBytes);
        entry.offset = disk.offset;
        entry.size = disk.size;

        if (entry.offset % kAlign != 0 || entry.offset + entry.size > pack->blob.size())
        {
            error = "条目越界或未对齐: " + entry.name;
            return nullptr;
        }
        pack->entries_.push_back(std::move(entry));
    }

    pack->sourcePath = path;
    return pack;
}

bool AssetPack::contains(const std::string& name) const
{
    const std::string wanted = normalizeAssetName(name);
    for (const PackEntry& entry : entries_)
    {
        if (entry.name == wanted)
            return true;
    }
    // 调用方给的是绝对路径（Resources/... 全路径）时，按"包内名是查询的后缀"匹配，
    // 取最长的那条，避免 "a/b.png" 抢走 "x/a/b.png"。
    for (const PackEntry& entry : entries_)
    {
        if (wanted.size() > entry.name.size() &&
            wanted.compare(wanted.size() - entry.name.size(), entry.name.size(), entry.name) == 0 &&
            wanted[wanted.size() - entry.name.size() - 1] == '/')
        {
            return true;
        }
    }
    return false;
}

bool AssetPack::read(const std::string& name, const uint8_t*& data, size_t& size) const
{
    const std::string wanted = normalizeAssetName(name);
    const PackEntry* best = nullptr;
    for (const PackEntry& entry : entries_)
    {
        if (entry.name == wanted)
        {
            best = &entry;
            break;
        }
        if (wanted.size() > entry.name.size() &&
            wanted.compare(wanted.size() - entry.name.size(), entry.name.size(), entry.name) == 0 &&
            wanted[wanted.size() - entry.name.size() - 1] == '/')
        {
            if (best == nullptr || entry.name.size() > best->name.size())
                best = &entry;
        }
    }
    if (best == nullptr)
        return false;
    data = blob.data() + best->offset;
    size = static_cast<size_t>(best->size);
    return true;
}

bool writePack(const std::string& path, const std::vector<PackInput>& inputs, std::string& error)
{
    error.clear();

    std::vector<DiskEntry> table(inputs.size());
    uint64_t cursor = alignUp(sizeof(PackHeader) + inputs.size() * sizeof(DiskEntry));
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        const std::string name = normalizeAssetName(inputs[i].name);
        if (name.empty() || name.size() >= kNameBytes)
        {
            error = "非法条目名: " + name;
            return false;
        }
        DiskEntry& entry = table[i];
        std::memset(&entry, 0, sizeof(entry));
        std::memcpy(entry.name, name.c_str(), name.size());
        entry.offset = cursor;
        entry.size = inputs[i].size;
        cursor = alignUp(cursor + inputs[i].size);
    }

    PackHeader header{};
    std::memcpy(header.magic, "TPAK", 4);
    header.version = kPackVersion;
    header.entryCount = static_cast<uint32_t>(inputs.size());
    header.reserved = 0;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "无法写入 " + path;
        return false;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(table.data()), static_cast<std::streamsize>(table.size() * sizeof(DiskEntry)));

    const uint64_t tableEnd = sizeof(PackHeader) + table.size() * sizeof(DiskEntry);
    std::vector<char> padding(static_cast<size_t>(alignUp(tableEnd) - tableEnd), 0);
    if (!padding.empty())
        out.write(padding.data(), static_cast<std::streamsize>(padding.size()));

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        if (inputs[i].size > 0)
            out.write(reinterpret_cast<const char*>(inputs[i].data), static_cast<std::streamsize>(inputs[i].size));
        const uint64_t end = table[i].offset + table[i].size;
        const uint64_t padded = alignUp(end);
        if (padded > end)
        {
            std::vector<char> tail(static_cast<size_t>(padded - end), 0);
            out.write(tail.data(), static_cast<std::streamsize>(tail.size()));
        }
    }

    if (!out)
    {
        error = "写入中断: " + path;
        return false;
    }
    return true;
}

bool packDirectory(const std::string& dir, const std::string& outPath, std::string& error)
{
    error.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec))
    {
        error = "目录不存在: " + dir;
        return false;
    }

    std::vector<std::vector<uint8_t>> blobs;
    std::vector<PackInput> inputs;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir, ec))
    {
        if (ec || !entry.is_regular_file())
            continue;
        std::vector<uint8_t> bytes;
        if (!readWholeFile(entry.path().string(), bytes))
        {
            error = "读取失败: " + entry.path().string();
            return false;
        }
        const std::string relative = fs::relative(entry.path(), dir, ec).generic_string();
        blobs.push_back(std::move(bytes));
        PackInput input{};
        input.name = relative;
        input.data = blobs.back().data();
        input.size = blobs.back().size();
        inputs.push_back(input);
    }

    if (inputs.empty())
    {
        error = "目录里没有文件: " + dir;
        return false;
    }
    return writePack(outPath, inputs, error);
}

void Mounts::mount(std::shared_ptr<AssetPack> pack)
{
    if (pack)
        packs.push_back(std::move(pack));
}

void Mounts::unmountAll()
{
    packs.clear();
}

bool Mounts::stat(const std::string& path, size_t& size) const
{
    for (auto it = packs.rbegin(); it != packs.rend(); ++it)
    {
        const uint8_t* data = nullptr;
        size_t bytes = 0;
        if ((*it)->read(path, data, bytes))
        {
            size = bytes;
            return true;
        }
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return false;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec)
        return false;
    size = static_cast<size_t>(fileSize);
    return true;
}

bool Mounts::exists(const std::string& path) const
{
    for (auto it = packs.rbegin(); it != packs.rend(); ++it)
    {
        if ((*it)->contains(path))
            return true;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool Mounts::readFile(const std::string& path, std::vector<uint8_t>& out) const
{
    for (auto it = packs.rbegin(); it != packs.rend(); ++it)
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if ((*it)->read(path, data, size))
        {
            out.assign(data, data + size);
            return true;
        }
    }
    return readWholeFile(path, out);
}

Mounts& mounts()
{
    static Mounts instance;
    return instance;
}

// ---------------------------------------------------------------------------
// 自检（--pack-test）：不碰 Vulkan，只在临时目录里走一遍 打包 -> 打开 -> 读回 -> 坏包拒绝。
// ---------------------------------------------------------------------------
namespace
{
    bool sameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
    }
}

bool assetPackSelfTest()
{
    namespace fs = std::filesystem;
    int cases = 0;
    bool ok = true;
    auto expect = [&ok, &cases](bool condition, const char* what)
    {
        ++cases;
        if (!condition)
        {
            ok = false;
            std::cerr << "[AssetPack] selfTest FAIL: " << what << '\n';
        }
    };

    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    // 有些受限环境禁止写系统临时目录（会得到 access denied），所以失败后退回构建目录。
    fs::path root = fs::temp_directory_path(ec) /
        ("tiny_pack_selftest_" + std::to_string(static_cast<long long>(stamp)));
    fs::remove_all(root, ec);
    ec.clear();
    fs::create_directories(root / "src" / "sub", ec);
    if (ec)
    {
        root = fs::current_path(ec) / "build" /
            ("tiny_pack_selftest_" + std::to_string(static_cast<long long>(stamp)));
        fs::remove_all(root, ec);
        ec.clear();
        fs::create_directories(root / "src" / "sub", ec);
    }
    expect(!ec, "临时目录可写");

    const std::string text = "hello tpack";
    std::vector<uint8_t> binary(256);
    for (size_t i = 0; i < binary.size(); ++i)
        binary[i] = static_cast<uint8_t>(i * 7 + 3);

    auto writeAll = [](const fs::path& path, const void* data, size_t size)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    };
    writeAll(root / "src" / "a.txt", text.data(), text.size());
    writeAll(root / "src" / "sub" / "b.bin", binary.data(), binary.size());

    const fs::path packPath = root / "test.tpack";
    std::string error;
    expect(packDirectory((root / "src").string(), packPath.string(), error),
           "packDirectory 打包成功");
    if (!error.empty())
        std::cerr << "[AssetPack] packDirectory error: " << error << '\n';

    auto pack = AssetPack::open(packPath.string(), error);
    expect(pack != nullptr, "打开的包非空");
    if (!pack)
    {
        fs::remove_all(root, ec);
        return false;
    }

    expect(pack->entries().size() == 2, "条目数 == 2");
    bool aligned = true;
    for (const PackEntry& entry : pack->entries())
        aligned = aligned && (entry.offset % 16 == 0);
    expect(aligned, "所有条目 16 字节对齐");

    const uint8_t* data = nullptr;
    size_t size = 0;
    expect(pack->read("a.txt", data, size) && size == text.size() &&
           std::memcmp(data, text.data(), text.size()) == 0, "a.txt 内容一致");
    expect(pack->read("sub/b.bin", data, size) && size == binary.size() &&
           std::memcmp(data, binary.data(), binary.size()) == 0, "sub/b.bin 内容一致");
    expect(!pack->read("missing.txt", data, size), "缺失条目返回 false");

    // 绝对路径按后缀匹配：贴图/模型代码里全是 Resources 全路径。
    const std::string absolute = (root / "src" / "sub" / "b.bin").string();
    expect(pack->contains(absolute), "绝对路径按后缀命中");

    // 坏包：magic 与版本号都必须被拒绝。
    {
        std::vector<uint8_t> bytes;
        {
            std::ifstream in(packPath, std::ios::binary | std::ios::ate);
            bytes.resize(static_cast<size_t>(in.tellg()));
            in.seekg(0);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        std::vector<uint8_t> badMagic = bytes;
        badMagic[0] = 'X';
        writeAll(root / "bad_magic.tpack", badMagic.data(), badMagic.size());
        expect(AssetPack::open((root / "bad_magic.tpack").string(), error) == nullptr,
               "magic 错误被拒绝");

        std::vector<uint8_t> badVersion = bytes;
        badVersion[4] = 99;
        writeAll(root / "bad_version.tpack", badVersion.data(), badVersion.size());
        expect(AssetPack::open((root / "bad_version.tpack").string(), error) == nullptr,
               "版本不支持被拒绝");
    }

    // 挂载后即使源文件被删掉，也要能从包里读出来。
    mounts().unmountAll();
    mounts().mount(pack);
    fs::remove_all(root / "src", ec);
    std::vector<uint8_t> readBack;
    expect(mounts().readFile((root / "src" / "a.txt").string(), readBack) && sameBytes(readBack,
           std::vector<uint8_t>(text.begin(), text.end())), "挂载后从包里读回 a.txt");
    expect(mounts().exists((root / "src" / "sub" / "b.bin").string()), "挂载后 exists 命中");
    mounts().unmountAll();
    expect(!mounts().exists((root / "src" / "a.txt").string()), "卸载后回退文件系统且不再命中");

    fs::remove_all(root, ec);

    if (ok)
        std::cout << "[AssetPack] selfTest PASS | cases=" << cases << " align=16 suffix-lookup=ok\n";
    return ok;
}

} // namespace assets