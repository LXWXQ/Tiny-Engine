#pragma once

// 环形临时表：一个"跨帧缓存"的固定形态。
//
// 要解决的问题：很多每帧查询是"同一个 key 上一帧算过，这帧还能用"（视锥裁剪结果、
// 材质解析结果、贴图描述符、玻璃块状态……）。常见写法是 std::unordered_map 每帧
// clear + 重填，代价不是查找，而是每帧几百次节点 delete/new —— 也就是把时间花在
// 堆上。这里的做法是：
//   - 对象从 ObjectPool 来，环被回收时放回池，指针复用；
//   - 每个环是一张侵入式哈希表（固定桶 + 每桶侵入式链表），换环不产生任何分配。
//
// RingSize 的含义是"一个条目最多活几帧"。取 2 正好覆盖两帧在飞：上一帧写入的条目
// 这一帧仍能查到，第三帧的 beginFrame() 才把它回收。要覆盖更长的历史（例如 TAA
// 的历史帧）就调大，但每个环都是独立的一张表，RingSize 增大只会多占桶数组。
//
// 契约：T 必须公有继承 ListHook，并实现
//     std::uint64_t hashValue() const;
//     void setHashValue(std::uint64_t hash);
// 注意 hash 由调用方保证"同 key 同值"，表内部不做重新哈希。

#include "Core/IntrusiveList.h"
#include "Core/ObjectPool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace tiny::containers
{

// 固定桶的侵入式哈希表：桶数在编译期确定，插入/删除/查找都不分配内存。
template <typename T, unsigned BucketBits = 8>
class IntrusiveHashMap
{
    static_assert(BucketBits >= 1 && BucketBits <= 16, "桶位数应在 1~16 之间");
    static constexpr std::size_t kBucketCount = std::size_t(1) << BucketBits;
    static constexpr std::uint64_t kBucketMask = kBucketCount - 1;

public:
    T* find(std::uint64_t hash)
    {
        IntrusiveList& bucket = bucketOf(hash);
        for (ListHook* hook = bucket.front(); hook != nullptr; hook = bucket.nextOf(hook))
        {
            T* entry = static_cast<T*>(hook);
            if (entry->hashValue() == hash)
                return entry;
        }
        return nullptr;
    }

    void insert(T* entry)
    {
        bucketOf(entry->hashValue()).pushBack(entry);
        ++size_;
    }

    void erase(T* entry)
    {
        bucketOf(entry->hashValue()).erase(entry);
        --size_;
    }

    T* first()
    {
        for (std::size_t i = 0; i < kBucketCount; ++i)
        {
            if (ListHook* hook = buckets_[i].front())
                return static_cast<T*>(hook);
        }
        return nullptr;
    }

    // 遍历顺序是"桶 0 到桶 N"，不是插入顺序。回调里删除当前条目是安全的：
    // 下一个 hook 在回调之前就取好了。
    template <typename Fn>
    void forEach(Fn&& fn)
    {
        for (std::size_t i = 0; i < kBucketCount; ++i)
        {
            IntrusiveList& bucket = buckets_[i];
            ListHook* hook = bucket.front();
            while (hook != nullptr)
            {
                ListHook* next = bucket.nextOf(hook);
                fn(*static_cast<T*>(hook));
                hook = next;
            }
        }
    }

    // 只断链，不析构对象（对象归 TemporaryHashmap 的池管）。
    void clear()
    {
        for (std::size_t i = 0; i < kBucketCount; ++i)
            buckets_[i].clear();
        size_ = 0;
    }

    std::size_t size() const { return size_; }

private:
    IntrusiveList& bucketOf(std::uint64_t hash) { return buckets_[hash & kBucketMask]; }

    std::array<IntrusiveList, kBucketCount> buckets_;
    std::size_t size_ = 0;
};

template <typename T, unsigned RingSize = 4>
class TemporaryHashmap
{
    static_assert(RingSize >= 2, "RingSize 至少要 2：一帧在写、一帧还要能读到");

public:
    TemporaryHashmap() = default;
    TemporaryHashmap(const TemporaryHashmap&) = delete;
    TemporaryHashmap& operator=(const TemporaryHashmap&) = delete;
    // 条目归本表所有：销毁时自己回收，调用方不需要（也不应该）再手动 clear。
    ~TemporaryHashmap() { clear(); }

    // 每帧开头调一次：推进到下一个环，把这个环（RingSize 帧前写下的那一批）全回收。
    // 常用的 key 因为每帧都被 request() 提升到当前环，所以不会过期；只有连续
    // RingSize 帧都没被访问过的条目才会在这里被销毁并归还对象池。
    void beginFrame()
    {
        currentRing_ = (currentRing_ + 1) % RingSize;
        IntrusiveHashMap<T>& ring = rings_[currentRing_];
        while (T* entry = ring.first())
        {
            ring.erase(entry);
            pool_.destroy(entry);
        }
    }

    // 从最新环往回找。命中老环时会把条目"提升"到当前环 —— 这一步是必需的：
    // 环回收只看条目待在哪一环，如果命中后不提升，一个每帧都在用的 key 会每
    // RingSize 帧被销毁重建一次（表现为缓存每两帧失效一次，比不用缓存更糟）。
    // 提升只是链表的两条 erase/insert，不分配内存、不拷贝对象。
    T* request(std::uint64_t hash)
    {
        for (unsigned i = 0; i < RingSize; ++i)
        {
            const unsigned ring = (currentRing_ + RingSize - i) % RingSize;
            if (T* entry = rings_[ring].find(hash))
            {
                if (ring != currentRing_)
                {
                    rings_[ring].erase(entry);
                    rings_[currentRing_].insert(entry);
                }
                return entry;
            }
        }
        return nullptr;
    }


    // 查不到就从池里造一个放进当前环（entry 的 hash 由这里写，不需要再 setHashValue）。
    template <typename... Args>
    T* getOrCreate(std::uint64_t hash, Args&&... args)
    {
        if (T* existing = request(hash))
            return existing;
        T* entry = pool_.create(std::forward<Args>(args)...);
        entry->setHashValue(hash);
        rings_[currentRing_].insert(entry);
        return entry;
    }

    // 全清：所有环的条目都还回池。
    void clear()
    {
        for (IntrusiveHashMap<T>& ring : rings_)
        {
            while (T* entry = ring.first())
            {
                ring.erase(entry);
                pool_.destroy(entry);
            }
        }
        currentRing_ = 0;
    }

    std::size_t size() const
    {
        std::size_t total = 0;
        for (const IntrusiveHashMap<T>& ring : rings_)
            total += ring.size();
        return total;
    }

    std::size_t liveCount() const { return pool_.liveCount(); }
    unsigned currentRing() const { return currentRing_; }

private:
    std::array<IntrusiveHashMap<T>, RingSize> rings_;
    ObjectPool<T> pool_;
    unsigned currentRing_ = 0;
};

// 纯逻辑自检：环轮转、跨环不串、池回收。不依赖 Vulkan，无头模式可直接跑。
bool selfTest();

} // namespace tiny::containers
