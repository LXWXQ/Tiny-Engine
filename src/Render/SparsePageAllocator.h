#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tiny
{

// P4-1：稀疏图集的「页」分配器 —— 虚拟阴影图的第一块砖。
//
// 范围与形状（有意保持最小）：
//   - 单图集、定长 128^2 页，页号即物理页索引；
//   - 空闲队列是 uint 环形队列（FIFO 复用），分配/释放都是 O(1)、不扫描；
//   - 每页一个版本号，页被复用时自增。页表里存 (page, version)，版本不匹配
//     即「内容已失效」，是「未标记的页复用上帧内容」的反面条件；
//   - 脏位图 = 本帧被投影到的页，供 P4-4 的 markPages 与「跳过未标记页」使用。
//
// 为什么不与 P7 的距离场共用（2026-11-26 决定）：那边要两套图集、两种页尺寸、
// 可能两级页表；共用会把这个类撑成两个使用者都别扭的折中。复制一份 200 行的
// 分配器，比抽象成「通用稀疏容器」便宜，也不会让阴影侧等距离场的接口定型。
class SparsePageAllocator
{
public:
    static constexpr uint32_t kInvalidPage = 0xFFFFFFFFu;
    // 8192^2 图集 / 128^2 页 = 4096 个物理页（docs/80-roadmap/ue-borrowing-plan.md 2.9）。
    static constexpr uint32_t kDefaultPageCount = 4096;

    struct Stats
    {
        uint32_t pageCount = 0;
        uint32_t livePages = 0;
        uint32_t peakLivePages = 0;
        uint32_t freePages = 0;
        uint32_t dirtyPages = 0;
        uint32_t reuseCount = 0;       // 分配到「以前用过」的页的次数
        uint32_t exhaustedCount = 0;   // 因为没空闲页而返回 kInvalidPage 的次数
        uint64_t allocCalls = 0;
        uint64_t freeCalls = 0;
    };

    explicit SparsePageAllocator(uint32_t pageCount = kDefaultPageCount);

    // 回到「全部空闲」的初始态（版本号与统计清零）。
    void reset();

    // 空闲队列空时返回 kInvalidPage。
    uint32_t allocate();
    // 页不合法或本来就空闲时返回 false（不静默吞掉双重释放）。
    bool release(uint32_t page);
    bool isLive(uint32_t page) const;

    // ── 脏页（P4-4 的「本帧被投影到的页」）─────────────────────────────
    void markDirty(uint32_t page);
    void markAllDirty();
    void clearDirty();
    bool isDirty(uint32_t page) const;
    // 升序？不 —— 是标记顺序，去重。P4-4 按它遍历要画的页。
    const std::vector<uint32_t>& dirtyPages() const { return dirtyList; }

    // 页被复用时自增；0 表示「还从没被分配过」。
    uint32_t versionOf(uint32_t page) const;

    uint32_t pageCount() const { return static_cast<uint32_t>(ring.size()); }
    uint32_t liveCount() const { return livePages; }
    uint32_t freeCount() const { return ringCount; }
    const Stats& getStats() const { return stats; }

    // 全部不变式：环形队列长度 + 活跃页数 == 总页数、队列无重复、位图与计数一致。
    bool validate(std::string* why = nullptr) const;

    // 确定性场景自检（每次启动都跑，毫秒级）。
    static bool selfTest();

    // 压力自检（--page-alloc-selftest）：随机分配/释放，与一份独立的口径对账。
    static bool stressTest(uint32_t ops, uint32_t pages, bool verbose);

private:
    static constexpr uint32_t kBitsPerWord = 64;
    uint32_t wordsFor(uint32_t count) const { return (count + kBitsPerWord - 1) / kBitsPerWord; }

    bool testBit(const std::vector<uint64_t>& bits, uint32_t page) const;
    void setBit(std::vector<uint64_t>& bits, uint32_t page, bool value);
    uint32_t countBits(const std::vector<uint64_t>& bits) const;

    std::vector<uint32_t> ring;       // 环形空闲队列（容量 = 页数，不做扩容）
    uint32_t ringHead = 0;
    uint32_t ringCount = 0;

    std::vector<uint64_t> liveBits;
    std::vector<uint64_t> dirtyBits;
    std::vector<uint32_t> versions;
    std::vector<uint32_t> dirtyList;

    uint32_t livePages = 0;
    uint32_t dirtyCount = 0;
    Stats stats{};
};

} // namespace tiny
