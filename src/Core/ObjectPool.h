#pragma once

// 分块对象池：一次对齐分配一大块内存，对象从块里 placement-new，析构后的指针进
// vacants 栈复用。用它的理由不是"省内存"，而是省掉每帧几百次 new/delete 的
// 堆锁与元数据开销 —— 池化之后建/拆一个对象是两次指针操作。
//
// 契约（和通用容器不同，必须显式回收）：
//   - 对象必须通过 destroy() 归还，池析构时不会替你调 T 的析构函数；
//   - Debug 下若析构时还有存活对象，会断言，避免"以为池管了生命周期"的误用。
//
// 块大小从 64 个对象起，每开新块翻倍（64 / 128 / 256 ... 上限 64<<5），
// 这样小池不浪费、大池也不会退化成逐对象 malloc。

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace tiny::containers
{

template <typename T>
class ObjectPool
{
    struct Block
    {
        Block* nextBlock = nullptr;
    };

    // 块头要按 T 的对齐补齐，否则 block + 1 之后的第一个对象可能没对齐。
    static constexpr std::size_t kHeaderBytes =
        (sizeof(Block) + alignof(T) - 1) / alignof(T) * alignof(T);
    static constexpr std::size_t kFirstBlockObjects = 64;
    static constexpr unsigned kMaxGrowth = 5;

public:
    ObjectPool() = default;
    ~ObjectPool()
    {
        assertEmpty();
        releaseBlocks();
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* create(Args&&... args)
    {
        void* memory = acquire();
        T* object = new (memory) T(std::forward<Args>(args)...);
        ++liveCount_;
        return object;
    }

    void destroy(T* object)
    {
        if (object == nullptr)
            return;
        object->~T();
        --liveCount_;
        vacants_.push_back(object);
    }

    // 池里当前存活对象数。TemporaryHashmap 的 selfTest 与"每帧构造次数"计数器都用它。
    std::size_t liveCount() const { return liveCount_; }
    std::size_t vacantCount() const { return vacants_.size(); }
    std::size_t blockCount() const { return blockCount_; }

private:
    void* acquire()
    {
        if (!vacants_.empty())
        {
            void* memory = vacants_.back();
            vacants_.pop_back();
            return memory;
        }
        if (blockRemaining_ == 0)
            addBlock();
        void* memory = static_cast<void*>(cursor_);
        cursor_ += sizeof(T);
        --blockRemaining_;
        return memory;
    }

    void addBlock()
    {
        const std::size_t count = kFirstBlockObjects << growth_;
        if (growth_ < kMaxGrowth)
            ++growth_;
        const std::size_t bytes = kHeaderBytes + count * sizeof(T);
        Block* block = static_cast<Block*>(::operator new(bytes, std::align_val_t{ alignof(T) }));
        block->nextBlock = blocks_;
        blocks_ = block;
        cursor_ = reinterpret_cast<unsigned char*>(block) + kHeaderBytes;
        blockRemaining_ = count;
        ++blockCount_;
    }

    void releaseBlocks()
    {
        while (blocks_ != nullptr)
        {
            Block* next = blocks_->nextBlock;
            ::operator delete(blocks_, std::align_val_t{ alignof(T) });
            blocks_ = next;
        }
        blockCount_ = 0;
        blockRemaining_ = 0;
        cursor_ = nullptr;
    }

    void assertEmpty() const
    {
        // 池不管对象生命周期：活着就说明有人漏了 destroy()，Debug 下直接停下来。
        // NDEBUG 下退化成"只释放内存"，宁可泄漏也不在发布版崩溃。
#ifndef NDEBUG
        if (liveCount_ != 0)
            __debugbreak();
#endif
    }

    Block* blocks_ = nullptr;
    unsigned char* cursor_ = nullptr;
    std::size_t blockRemaining_ = 0;
    std::size_t blockCount_ = 0;
    std::size_t liveCount_ = 0;
    std::size_t growth_ = 0;
    std::vector<void*> vacants_;
};

} // namespace tiny::containers