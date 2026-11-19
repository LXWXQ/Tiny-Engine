#include "Core/TemporaryHashmap.h"

#include <cstdint>
#include <cstdio>

// 纯逻辑自检（不需要 Vulkan 设备）：目的不是"证明表中能存东西"，而是把三个
// 容易写错、写错了又不会立刻崩的行为钉住：
//   1) 环轮转的生死线 —— RingSize=2 时"上一帧还在、第三帧必须消失"；
//   2) 跨环不串 —— 不同 key 落进不同环，不能互相查到；
//   3) 池回收 —— 条目被回收后池的存活数必须归零，否则说明有对象漏归还。

namespace tiny::containers
{
namespace
{

struct TestEntry : ListHook
{
    std::uint64_t hash = 0;
    int value = 0;

    std::uint64_t hashValue() const { return hash; }
    void setHashValue(std::uint64_t h) { hash = h; }
};

int gFailures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("[Containers] FAIL: %s\n", what);
        ++gFailures;
    }
}

// 第 1 项：没人再访问的条目会过期 —— RingSize=2 时，两帧不碰就该被回收。
void testExpiry()
{
    TemporaryHashmap<TestEntry, 2> map;

    map.beginFrame(); // 第 0 帧写入，之后两帧都不碰它
    TestEntry* entry = map.getOrCreate(7);
    entry->value = 42;
    check(map.size() == 1 && map.liveCount() == 1, "写入后表内应有 1 条");

    map.beginFrame(); // 第 1 帧
    map.beginFrame(); // 第 2 帧：第 0 帧所在的环被回收
    check(map.request(7) == nullptr, "两帧无人访问的条目应在环轮转时被回收");
    check(map.size() == 0, "回收后表应为空");
    check(map.liveCount() == 0, "回收后对象池存活数应归零");
}


// 第 1 项（补充）：每帧都被访问的 key 不能过期 —— 提升语义就是为了这个。
// 没有提升时，同一个 key 会每 RingSize 帧被销毁重建，缓存反而变成"每两帧失效一次"。
void testPromotion()
{
    TemporaryHashmap<TestEntry, 2> map;
    TestEntry* first = nullptr;

    map.beginFrame();
    first = map.getOrCreate(11);
    first->value = 7;

    for (int frame = 0; frame < 32; ++frame)
    {
        map.beginFrame();
        TestEntry* entry = map.request(11);
        check(entry == first, "常用 key 应始终命中同一个对象（说明被提升而不是重建）");
        if (entry == nullptr)
            break;
        check(entry->value == 7, "提升不应丢失数据");
        check(map.liveCount() == 1, "提升不应产生新对象");
    }
}

// 第 2 项：跨环不串。
void testRingIsolation()
{
    TemporaryHashmap<TestEntry, 4> map;

    map.beginFrame();
    TestEntry* a = map.getOrCreate(11);
    a->value = 1;

    map.beginFrame();
    TestEntry* b = map.getOrCreate(22);
    b->value = 2;

    check(map.request(11) == a, "旧环的 key 仍应命中原对象");
    check(map.request(22) == b, "新环的 key 应命中新对象");
    check(map.request(33) == nullptr, "未写入的 key 不应命中");
    check(a != b, "不同 key 应是不同对象");

    map.clear();
    check(map.size() == 0, "clear 后表应为空");
    check(map.liveCount() == 0, "clear 后对象池存活数应归零");
}

// 第 3 项：反复建拆不泄漏（每个环轮一圈都归还一次）。
void testPoolRecycle()
{
    TemporaryHashmap<TestEntry, 2> map;

    for (int frame = 0; frame < 64; ++frame)
    {
        map.beginFrame();
        for (std::uint64_t key = 0; key < 32; ++key)
        {
            TestEntry* entry = map.getOrCreate(key);
            entry->value = int(key);
        }
    }

    // 两帧在飞，最多 2 * 32 个对象同时存活；池不该无限增长。
    check(map.liveCount() <= 64, "对象池不应随帧数无限增长");

    map.clear();
    check(map.liveCount() == 0, "clear 后对象池存活数应归零");

    // 对象池本身：批量建拆后存活数归零，且复用指针（不重新 malloc）。
    ObjectPool<TestEntry> pool;
    TestEntry* kept[16] = {};
    for (int i = 0; i < 16; ++i)
        kept[i] = pool.create();
    check(pool.liveCount() == 16, "池存活数应等于创建数");
    for (int i = 0; i < 16; ++i)
        pool.destroy(kept[i]);
    check(pool.liveCount() == 0, "全部归还后存活数应为 0");

    TestEntry* reused = pool.create();
    bool foundReused = false;
    for (int i = 0; i < 16; ++i)
        foundReused = foundReused || (reused == kept[i]);
    check(foundReused, "归还的指针应被复用，而不是新分配");
    pool.destroy(reused);
}

} // namespace

bool selfTest()
{
    gFailures = 0;
    testExpiry();
    testPromotion();

    testRingIsolation();
    testPoolRecycle();
    if (gFailures != 0)
    {
        std::printf("[Containers] selfTest FAIL | failures=%d\n", gFailures);
        return false;
    }
    std::printf("[Containers] selfTest PASS | 环轮转 + 跨环不串 + 池回收\n");
    return true;
}

} // namespace tiny::containers
