#pragma once

// 侵入式双向链表：链接信息放在对象自己身上（继承 ListHook），而不是链表节点里。
// 这么做的两个理由：
//   1) 对象可以在释放后直接进"空闲池"，不需要为它在链表里再分配一次节点；
//   2) 同一个对象可以同时挂在多条链上（每条链一个 Hook 成员），
//      TemporaryHashmap 的每个环就是一个链表，对象本身同时还在对象池里。
//
// 用法：T 公有继承 ListHook，然后 IntrusiveList::pushBack(&object) / erase(&object)。
// 链表本身持有哨兵节点，空表时 head_.next == head_.prev == &head_。

#include <cstddef>

namespace tiny::containers
{

struct ListHook
{
    ListHook* prev = nullptr;
    ListHook* next = nullptr;
};

class IntrusiveList
{
public:
    IntrusiveList() { head_.next = &head_; head_.prev = &head_; }
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    bool empty() const { return head_.next == &head_; }
    std::size_t size() const { return size_; }

    void pushBack(ListHook* hook)
    {
        hook->prev = head_.prev;
        hook->next = &head_;
        head_.prev->next = hook;
        head_.prev = hook;
        ++size_;
    }

    // 从链上摘下但不触碰对象本身（谁负责析构由调用方决定）。
    void erase(ListHook* hook)
    {
        hook->prev->next = hook->next;
        hook->next->prev = hook->prev;
        hook->prev = nullptr;
        hook->next = nullptr;
        --size_;
    }

    void clear()
    {
        ListHook* hook = head_.next;
        while (hook != &head_)
        {
            ListHook* next = hook->next;
            hook->prev = nullptr;
            hook->next = nullptr;
            hook = next;
        }
        head_.next = &head_;
        head_.prev = &head_;
        size_ = 0;
    }

    ListHook* front() const { return empty() ? nullptr : head_.next; }
    // 走到表尾返回 nullptr，因此可以直接写 for (auto* h = list.front(); h; h = list.nextOf(h))。
    ListHook* nextOf(const ListHook* hook) const
    {
        return (hook->next == &head_) ? nullptr : hook->next;
    }

private:
    ListHook head_{};
    std::size_t size_ = 0;
};

} // namespace tiny::containers