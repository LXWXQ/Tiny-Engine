#include "PipelineStats.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// 计数是普通的非原子整数：管线只在启动阶段由主线程创建，没有并发写入。
// 真出现并发建管线时（例如将来的后台预热）再加锁——现在加是凭空的复杂度。
namespace tiny::pipelinestats
{
    namespace
    {
        struct Totals
        {
            uint32_t graphics = 0;
            uint32_t compute = 0;
            uint32_t modules = 0;
            uint32_t failures = 0;
            uint64_t spirvBytes = 0;
            double   graphicsMs = 0.0;
            double   computeMs = 0.0;
            uint64_t cacheLoadedBytes = 0;
        };

        // 判重表：hash -> 第一次见到它的那条管线。存名字是为了报告能指出
        // "这两条状态相同"，否则只给出一个数字，读者仍然不知道该不该合并。
        struct StateEntry
        {
            std::string vertPath;
            std::string fragPath;
            uint32_t    count = 1;
        };

        Totals g_totals;
        bool   g_enabled = false;

        // 两张表，回答两个不同的问题：
        //   g_states    —— 状态字 -> 第一次见到的管线（"一共有几种状态"）；
        //   g_identical —— 状态字 + 两个 shader 名（"有哪些管线完全一样"）。
        // 分开是因为"状态相同但 shader 不同"完全正常（IBL 的 4 条管线共用同一套
        // 固定状态，只有片元着色器不同），把它算成重复会得出错误结论。
        std::unordered_map<uint64_t, StateEntry> g_states;
        std::unordered_map<std::string, StateEntry> g_identical;

        // 只保留文件名：报告里 "geometry.vert/geometry.frag" 比一长串绝对路径
        // 好读，而绝对路径在不同机器上还会变（Chrome trace 里的路径同理）。
        std::string shortName(const char* path)
        {
            if (path == nullptr) return std::string("<null>");
            const std::string full(path);
            const size_t slash = full.find_last_of("/\\");
            return slash == std::string::npos ? full : full.substr(slash + 1);
        }

        void hashU64(uint64_t& h, uint64_t value)
        {
            // FNV-1a，按字节喂：避免大小端差异，也让不同宽度字段可以混在一起。
            for (int i = 0; i < 8; ++i)
            {
                h ^= static_cast<uint64_t>((value >> (i * 8)) & 0xFFu);
                h *= 1099511628211ull;
            }
        }
    }

    GraphicsStateKey makeEmptyKey()
    {
        GraphicsStateKey key{};
        key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        key.cullMode = VK_CULL_MODE_NONE;
        key.frontFace = VK_FRONT_FACE_CLOCKWISE;
        key.depthCompare = VK_COMPARE_OP_ALWAYS;
        key.srcColorBlend = VK_BLEND_FACTOR_ONE;
        key.dstColorBlend = VK_BLEND_FACTOR_ZERO;
        key.colorBlendOp = VK_BLEND_OP_ADD;
        key.srcAlphaBlend = VK_BLEND_FACTOR_ONE;
        key.dstAlphaBlend = VK_BLEND_FACTOR_ZERO;
        key.alphaBlendOp = VK_BLEND_OP_ADD;
        key.colorFormat = VK_FORMAT_UNDEFINED;
        key.depthFormat = VK_FORMAT_UNDEFINED;
        key.pipelineLayout = 0;
        return key;
    }

    uint64_t hashGraphicsState(const GraphicsStateKey& key)
    {
        uint64_t h = 1469598103934665603ull;

        hashU64(h, static_cast<uint64_t>(key.topology));
        hashU64(h, static_cast<uint64_t>(key.cullMode));
        hashU64(h, static_cast<uint64_t>(key.frontFace));
        hashU64(h, static_cast<uint64_t>(key.depthCompare));
        // 三个 bool 压成一个整数：单独喂 1 字节也能做，但压成一位字段能保证
        // "只差一个开关"一定得到不同 hash（否则得靠累积巧合）。
        hashU64(h, (key.depthTest ? 1ull : 0ull) |
                   (key.depthWrite ? 2ull : 0ull) |
                   (key.depthBias ? 4ull : 0ull) |
                   (key.blend ? 8ull : 0ull));
        hashU64(h, static_cast<uint64_t>(key.srcColorBlend));
        hashU64(h, static_cast<uint64_t>(key.dstColorBlend));
        hashU64(h, static_cast<uint64_t>(key.colorBlendOp));
        hashU64(h, static_cast<uint64_t>(key.srcAlphaBlend));
        hashU64(h, static_cast<uint64_t>(key.dstAlphaBlend));
        hashU64(h, static_cast<uint64_t>(key.alphaBlendOp));
        hashU64(h, static_cast<uint64_t>(key.colorFormat));
        hashU64(h, static_cast<uint64_t>(key.depthFormat));
        hashU64(h, key.colorAttachmentCount);
        hashU64(h, key.pipelineLayout);
        hashU64(h, key.bindingCount);
        hashU64(h, key.attributeCount);
        return h;
    }

    void setEnabled(bool value) { g_enabled = value; }
    bool enabled() { return g_enabled; }

    void recordGraphics(const GraphicsStateKey& key, const char* vertPath, const char* fragPath, double milliseconds)
    {
        g_totals.graphics += 1;
        g_totals.graphicsMs += milliseconds;

        if (!g_enabled) return;

        const uint64_t hash = hashGraphicsState(key);
        auto it = g_states.find(hash);
        if (it == g_states.end())
        {
            StateEntry entry;
            entry.vertPath = shortName(vertPath);
            entry.fragPath = shortName(fragPath);
            g_states.emplace(hash, std::move(entry));
        }
        else
        {
            it->second.count += 1;
        }

        const std::string identity = std::to_string(hash) + "|" + shortName(vertPath) + "|" + shortName(fragPath);
        auto identical = g_identical.find(identity);
        if (identical == g_identical.end())
        {
            StateEntry entry;
            entry.vertPath = shortName(vertPath);
            entry.fragPath = shortName(fragPath);
            g_identical.emplace(identity, std::move(entry));
        }
        else
        {
            identical->second.count += 1;
        }
    }

    void recordCompute(const char* compPath, double milliseconds)
    {
        (void)compPath;
        g_totals.compute += 1;
        g_totals.computeMs += milliseconds;
    }

    void recordShaderModule(uint64_t spirvBytes)
    {
        g_totals.modules += 1;
        g_totals.spirvBytes += spirvBytes;
    }

    void recordFailure() { g_totals.failures += 1; }

    void setCacheLoadedBytes(uint64_t bytes) { g_totals.cacheLoadedBytes = bytes; }

    void report()
    {
        const Totals& t = g_totals;
        const uint32_t total = t.graphics + t.compute;
        const double totalMs = t.graphicsMs + t.computeMs;
        const double averageMs = total == 0 ? 0.0 : totalMs / static_cast<double>(total);

        std::cout << "[Pipelines] 图形 " << t.graphics << " 条 / compute " << t.compute
                  << " 条 / shader 模块 " << t.modules << " 个 / SPIR-V 读取 "
                  << std::fixed << std::setprecision(2)
                  << (static_cast<double>(t.spirvBytes) / (1024.0 * 1024.0)) << " MB\n";

        std::cout << "[Pipelines] 建管线 total=" << std::setprecision(1) << totalMs
                  << "ms（图形 " << t.graphicsMs << " / compute " << t.computeMs
                  << "，平均 " << std::setprecision(2) << averageMs << "ms/条）\n";

        if (t.failures > 0)
            std::cout << "[Pipelines] 失败 " << t.failures << " 次\n";

        if (!g_enabled)
        {
            std::cout << "[Pipelines] 状态判重未开启（未调用 setEnabled）\n";
            return;
        }

        uint32_t identicalExtra = 0;
        for (const auto& pair : g_identical)
            identicalExtra += pair.second.count - 1;

        std::cout << "[Pipelines] 状态种类 " << g_states.size() << " 种（图形管线 " << t.graphics
                  << " 条）；完全相同的管线 " << identicalExtra << " 条\n";

        // 状态种类远少于管线数，说明"状态字"这套抽象能共享；完全相同才是真浪费。
        // 示例最多 3 组：报告是给人看的，列全了反而找不到重点。
        uint32_t shown = 0;
        for (const auto& pair : g_identical)
        {
            if (pair.second.count < 2 || shown >= 3) continue;
            std::cout << "[Pipelines]   完全相同：" << pair.second.vertPath << "/" << pair.second.fragPath
                      << " 出现 " << pair.second.count << " 次\n";
            ++shown;
        }

        std::cout << "[Pipelines] 管线缓存：本次载入 " << t.cacheLoadedBytes
                  << " 字节（退出时另存）\n";
    }

    bool selfTest()
    {
        bool ok = true;
        const GraphicsStateKey base = makeEmptyKey();
        const uint64_t baseHash = hashGraphicsState(base);

        // 同样的值再算一次必须一致：哈希里不能掺指针或随机量。
        if (hashGraphicsState(makeEmptyKey()) != baseHash)
        {
            std::cout << "[Pipelines] selfTest FAIL: 相同状态得到不同 hash\n";
            return false;
        }

        // 每个语义字段单独改一次，hash 都必须变——漏一个字段就意味着两条真正
        // 不同的管线会被判成同一条。
        GraphicsStateKey key = base;
        key.cullMode = VK_CULL_MODE_BACK_BIT;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: cullMode 未参与 hash\n"; ok = false; }
        key = base;
        key.depthCompare = VK_COMPARE_OP_LESS;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: depthCompare 未参与 hash\n"; ok = false; }
        key = base;
        key.blend = true;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: blend 未参与 hash\n"; ok = false; }
        key = base;
        key.depthWrite = true;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: depthWrite 未参与 hash\n"; ok = false; }
        key = base;
        key.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: colorFormat 未参与 hash\n"; ok = false; }
        key = base;
        key.depthFormat = VK_FORMAT_D32_SFLOAT;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: depthFormat 未参与 hash\n"; ok = false; }
        key = base;
        key.bindingCount = 2;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: bindingCount 未参与 hash\n"; ok = false; }
        key = base;
        key.pipelineLayout = 0x1234;
        if (hashGraphicsState(key) == baseHash) { std::cout << "[Pipelines] selfTest FAIL: pipelineLayout 未参与 hash\n"; ok = false; }

        // 只改完全不影响管线派生的字段：这里没有这种字段（viewport 根本不在 key 里），
        // 所以反过来验一次"两个不同 key 不会撞"。
        GraphicsStateKey a = base; a.cullMode = VK_CULL_MODE_FRONT_BIT;
        GraphicsStateKey b = base; b.cullMode = VK_CULL_MODE_BACK_BIT;
        if (hashGraphicsState(a) == hashGraphicsState(b)) { std::cout << "[Pipelines] selfTest FAIL: 前/背面剔除撞 hash\n"; ok = false; }

        if (ok)
            std::cout << "[Pipelines] selfTest PASS | 状态字对 8 个语义字段敏感 + 重复构造稳定\n";
        return ok;
    }
}
