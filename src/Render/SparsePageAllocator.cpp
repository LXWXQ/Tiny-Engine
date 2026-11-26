#include "Render/SparsePageAllocator.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace tiny
{

SparsePageAllocator::SparsePageAllocator(uint32_t pageCount)
{
    if (pageCount == 0)
        pageCount = 1;
    ring.resize(pageCount);
    liveBits.resize(wordsFor(pageCount), 0);
    dirtyBits.resize(wordsFor(pageCount), 0);
    versions.resize(pageCount, 0);
    dirtyList.reserve(pageCount);
    reset();
}

void SparsePageAllocator::reset()
{
    const uint32_t count = pageCount();
    for (uint32_t i = 0; i < count; ++i)
        ring[i] = i;             // 首次分配按 0..N-1 顺序给出，缓存友好
    ringHead = 0;
    ringCount = count;
    std::fill(liveBits.begin(), liveBits.end(), 0ull);
    std::fill(dirtyBits.begin(), dirtyBits.end(), 0ull);
    std::fill(versions.begin(), versions.end(), 0u);
    dirtyList.clear();
    livePages = 0;
    dirtyCount = 0;
    stats = Stats{};
    stats.pageCount = count;
    stats.freePages = count;
}

uint32_t SparsePageAllocator::allocate()
{
    ++stats.allocCalls;
    if (ringCount == 0)
    {
        ++stats.exhaustedCount;
        return kInvalidPage;
    }

    const uint32_t page = ring[ringHead];
    ringHead = (ringHead + 1) % pageCount();
    --ringCount;

    setBit(liveBits, page, true);
    ++livePages;
    stats.livePages = livePages;
    stats.freePages = ringCount;
    if (livePages > stats.peakLivePages)
        stats.peakLivePages = livePages;
    // 版本号只在「页从空闲变成活跃」时自增：0 是「从没分发过」，
    // 复用过的页从此带上非零版本，页表拿它判定内容是否还有效。
    if (versions[page] != 0)
        ++stats.reuseCount;
    ++versions[page];
    return page;
}

bool SparsePageAllocator::release(uint32_t page)
{
    if (page >= pageCount() || !testBit(liveBits, page))
        return false;

    ++stats.freeCalls;
    setBit(liveBits, page, false);
    --livePages;
    stats.livePages = livePages;

    // 脏标记不属于「谁在用」：页被复用时本帧仍会被重画，所以这里不清脏位，
    // 由 clearDirty() 按帧统一清。清在这里会让「本帧先释放再复用」的页漏画。
    ring[(ringHead + ringCount) % pageCount()] = page;
    ++ringCount;
    stats.freePages = ringCount;
    return true;
}

bool SparsePageAllocator::isLive(uint32_t page) const
{
    return page < pageCount() && testBit(liveBits, page);
}

void SparsePageAllocator::markDirty(uint32_t page)
{
    if (page >= pageCount() || testBit(dirtyBits, page))
        return;
    setBit(dirtyBits, page, true);
    ++dirtyCount;
    stats.dirtyPages = dirtyCount;
    dirtyList.push_back(page);
}

void SparsePageAllocator::markAllDirty()
{
    const uint32_t count = pageCount();
    for (uint32_t page = 0; page < count; ++page)
        markDirty(page);
}

void SparsePageAllocator::clearDirty()
{
    std::fill(dirtyBits.begin(), dirtyBits.end(), 0ull);
    dirtyList.clear();
    dirtyCount = 0;
    stats.dirtyPages = 0;
}

bool SparsePageAllocator::isDirty(uint32_t page) const
{
    return page < pageCount() && testBit(dirtyBits, page);
}

uint32_t SparsePageAllocator::versionOf(uint32_t page) const
{
    return page < pageCount() ? versions[page] : 0u;
}

bool SparsePageAllocator::testBit(const std::vector<uint64_t>& bits, uint32_t page) const
{
    return (bits[page / kBitsPerWord] >> (page % kBitsPerWord)) & 1ull;
}

void SparsePageAllocator::setBit(std::vector<uint64_t>& bits, uint32_t page, bool value)
{
    const uint64_t mask = 1ull << (page % kBitsPerWord);
    if (value)
        bits[page / kBitsPerWord] |= mask;
    else
        bits[page / kBitsPerWord] &= ~mask;
}

uint32_t SparsePageAllocator::countBits(const std::vector<uint64_t>& bits) const
{
    uint32_t total = 0;
    for (uint64_t word : bits)
        while (word != 0ull)
        {
            word &= word - 1ull;
            ++total;
        }
    return total;
}

bool SparsePageAllocator::validate(std::string* why) const
{
    const uint32_t count = pageCount();
    auto fail = [why](const char* text) {
        if (why != nullptr)
            *why = text;
        return false;
    };

    if (ring.size() != count || liveBits.size() != wordsFor(count) || versions.size() != count)
        return fail("buffer size mismatch");
    if (ringCount + livePages != count)
        return fail("free ring + live pages != page count");
    if (countBits(liveBits) != livePages)
        return fail("live bitmap popcount != live count");
    if (dirtyCount != dirtyList.size() || countBits(dirtyBits) != dirtyCount)
        return fail("dirty bitmap/list/count disagree");
    if (stats.livePages != livePages || stats.freePages != ringCount)
        return fail("stats drift from live counters");

    // 队列里不能有重复页，也不能有活跃页（后者就是「同一页被分配两次」）。
    std::vector<uint8_t> seen(count, 0);
    for (uint32_t i = 0; i < ringCount; ++i)
    {
        const uint32_t page = ring[(ringHead + i) % count];
        if (page >= count)
            return fail("ring entry out of range");
        if (seen[page] != 0)
            return fail("duplicate page in free ring");
        seen[page] = 1;
        if (testBit(liveBits, page))
            return fail("live page also sits in free ring");
    }
    if (static_cast<uint32_t>(std::count(seen.begin(), seen.end(), static_cast<uint8_t>(0))) != livePages)
        return fail("ring/live partition mismatch");
    if (why != nullptr)
        why->clear();
    return true;
}

namespace
{
const char* pageAllocFail(const char* what)
{
    std::printf("[PageAlloc] FAIL | %s\n", what);
    return what;
}
} // namespace

bool SparsePageAllocator::selfTest()
{
    constexpr uint32_t kPages = 64;
    SparsePageAllocator alloc(kPages);
    std::string why;
    if (!alloc.validate(&why))
        return pageAllocFail("fresh allocator is invalid");

    if (alloc.freeCount() != kPages || alloc.liveCount() != 0)
        return pageAllocFail("fresh allocator is not fully free");

    // 1) 全部分配：空闲态按 0..N-1 线性给出，第 N+1 次返回 kInvalidPage。
    for (uint32_t i = 0; i < kPages; ++i)
    {
        const uint32_t page = alloc.allocate();
        if (page != i)
            return pageAllocFail("allocation order is not linear while the ring is fresh");
        if (alloc.versionOf(page) != 1u)
            return pageAllocFail("first allocation should leave version at 1");
    }
    if (alloc.allocate() != kInvalidPage)
        return pageAllocFail("exhausted allocator should return kInvalidPage");
    if (alloc.getStats().exhaustedCount != 1u)
        return pageAllocFail("exhausted counter not updated");
    if (!alloc.validate(&why))
        return pageAllocFail("fully allocated allocator is invalid");

    // 2) 非法释放一律返回 false，不静默吞掉双重释放。
    if (alloc.release(kPages))
        return pageAllocFail("release(out of range) must fail");
    if (alloc.release(kInvalidPage))
        return pageAllocFail("release(kInvalidPage) must fail");
    if (!alloc.release(7u) || alloc.release(7u))
        return pageAllocFail("double release must fail");

    // 3) 页复用：环形队列先把刚释放的 7 号还回来，版本 1 -> 2。
    const uint32_t reused = alloc.allocate();
    if (reused != 7u)
        return pageAllocFail("free ring should hand back the just-freed page first");
    if (alloc.versionOf(reused) != 2u)
        return pageAllocFail("reusing a page should bump its version");
    if (alloc.getStats().reuseCount != 1u)
        return pageAllocFail("reuse counter not updated");

    // 4) FIFO：依次释放 5、6、7 之后，分配顺序必须是 5、6、7。
    if (!alloc.release(5u) || !alloc.release(6u) || !alloc.release(7u))
        return pageAllocFail("release of live pages failed");
    if (alloc.allocate() != 5u || alloc.allocate() != 6u || alloc.allocate() != 7u)
        return pageAllocFail("free ring is not FIFO");

    // 5) 脏页去重、计数、清空。
    alloc.clearDirty();
    alloc.markDirty(3u);
    alloc.markDirty(3u);
    alloc.markDirty(9u);
    if (alloc.dirtyPages().size() != 2u || !alloc.isDirty(3u) || !alloc.isDirty(9u))
        return pageAllocFail("dirty marking should dedupe");
    if (!alloc.validate(&why))
        return pageAllocFail("dirty state broke invariants");
    alloc.clearDirty();
    if (!alloc.dirtyPages().empty() || alloc.isDirty(3u) || alloc.getStats().dirtyPages != 0u)
        return pageAllocFail("clearDirty should reset the bitmap and the list");

    // 6) 无泄漏：全部释放后必须回到初始态，且能再次全量分配。
    for (uint32_t page = 0; page < kPages; ++page)
        alloc.release(page);
    if (alloc.liveCount() != 0 || alloc.freeCount() != kPages)
        return pageAllocFail("releasing everything should return to the initial state");
    if (!alloc.validate(&why))
        return pageAllocFail("emptied allocator is invalid");
    return true;
}

bool SparsePageAllocator::stressTest(uint32_t ops, uint32_t pages, bool verbose)
{
    if (ops == 0)
        ops = 1;
    if (pages == 0)
        pages = kDefaultPageCount;

    SparsePageAllocator alloc(pages);
    std::string why;
    if (!alloc.validate(&why))
        return pageAllocFail("stress allocator starts invalid");

    // 独立口径：一张「谁在用」的朴素数组 + 活跃页列表。分配器说的每一句话
    // 都要能在这份口径里对上；对不上就是它在骗自己。
    std::vector<uint8_t> inUse(pages, 0u);
    std::vector<uint32_t> liveList;
    liveList.reserve(pages);

    uint32_t rng = 0x2545F491u;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };

    const auto start = std::chrono::steady_clock::now();
    uint32_t durableChecks = 0;
    for (uint32_t op = 0; op < ops; ++op)
    {
        // 释放概率 = 当前占用率：占用低就多分配、占用高就多释放，让活跃页数
        // 在容量一半附近做无界随机游走 —— 于是既会撞到「分配器空了」的边界，
        // 也会撞到「几乎全满、只剩环形队列里那几个碎片页」的情形。
        const uint32_t fillPercent =
            static_cast<uint32_t>(100.0f * static_cast<float>(liveList.size()) / static_cast<float>(pages));
        const bool wantFree = !liveList.empty() && ((next() % 100u) < fillPercent);
        if (wantFree)
        {
            const uint32_t slot = next() % static_cast<uint32_t>(liveList.size());
            const uint32_t page = liveList[slot];
            liveList[slot] = liveList.back();
            liveList.pop_back();
            if (inUse[page] == 0u)
                return pageAllocFail("oracle thinks the page is free before release");
            if (!alloc.release(page))
                return pageAllocFail("release(live page) returned false");
            inUse[page] = 0u;
        }
        else
        {
            const uint32_t page = alloc.allocate();
            if (liveList.size() == pages)
            {
                if (page != kInvalidPage)
                    return pageAllocFail("allocated beyond capacity");
            }
            else
            {
                if (page == kInvalidPage)
                    return pageAllocFail("unexpected exhaustion");
                if (page >= pages || inUse[page] != 0u)
                    return pageAllocFail("allocator handed out a page that is already in use");
                inUse[page] = 1u;
                liveList.push_back(page);
            }
        }

        if ((op & 0xFFFu) == 0xFFFu)
        {
            if (!alloc.validate(&why))
                return pageAllocFail(why.c_str());
            if (alloc.freeCount() != pages - static_cast<uint32_t>(liveList.size()))
                return pageAllocFail("free count drifted from the oracle");
            ++durableChecks;
        }
    }
    // 第二段：按到接近满。环形队列在「只剩几十个碎页」时容易把 head/count 算错，
    // 而这条路径在随机游走里出现得太少，所以单独压一遍。
    uint32_t satReuse = 0;
    for (uint32_t round = 0; round < pages * 2u; ++round)
    {
        if ((round % 97u) == 0u && !liveList.empty())
        {
            const uint32_t slot = next() % static_cast<uint32_t>(liveList.size());
            const uint32_t page = liveList[slot];
            liveList[slot] = liveList.back();
            liveList.pop_back();
            if (!alloc.release(page))
                return pageAllocFail("near-full: release(live page) returned false");
            inUse[page] = 0u;
            ++satReuse;
            continue;
        }
        const uint32_t page = alloc.allocate();
        if (page == kInvalidPage)
        {
            if (liveList.size() != pages)
                return pageAllocFail("near-full: allocator reported exhaustion while pages were free");
            continue;
        }
        if (page >= pages || inUse[page] != 0u)
            return pageAllocFail("near-full: allocator handed out a page already in use");
        inUse[page] = 1u;
        liveList.push_back(page);
    }
    if (satReuse == 0u)
        return pageAllocFail("near-full phase never freed a page");
    if (!alloc.validate(&why))
        return pageAllocFail(why.c_str());

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const Stats afterStress = alloc.getStats();

    // 收尾一：全部释放后必须回到初始态（任何泄漏都会让 freeCount 差一点）。
    for (uint32_t page : liveList)
    {
        if (!alloc.release(page))
            return pageAllocFail("drain release failed");
    }
    if (alloc.liveCount() != 0u || alloc.freeCount() != pages)
        return pageAllocFail("leak after stress: draining did not return every page");
    if (!alloc.validate(&why))
        return pageAllocFail(why.c_str());

    // 收尾二：再全量分配一遍，必须是 pages 个互不相同的页 —— 无重叠的硬证据。
    std::vector<uint8_t> seen(pages, 0u);
    for (uint32_t i = 0; i < pages; ++i)
    {
        const uint32_t page = alloc.allocate();
        if (page == kInvalidPage)
            return pageAllocFail("allocator never came back to a fully free state");
        if (page >= pages || seen[page] != 0u)
            return pageAllocFail("duplicate page handed out after the stress round");
        seen[page] = 1u;
    }
    if (alloc.allocate() != kInvalidPage)
        return pageAllocFail("allocator over-allocated after the stress round");
    if (!alloc.validate(&why))
        return pageAllocFail(why.c_str());

    if (verbose)
    {
        const double perOp = (ops > 0) ? (ms * 1000000.0 / static_cast<double>(ops)) : 0.0;
        std::printf("[PageAlloc] stress PASS | ops=%u pages=%u alloc+free=%.1f ns/op | checks=%u peak=%u reuse=%u exhausted=%u\n",
            ops, pages, perOp, durableChecks, afterStress.peakLivePages, afterStress.reuseCount,
            afterStress.exhaustedCount);
    }
    return true;
}

} // namespace tiny
