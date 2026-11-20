#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

// ============================================================================
// TM-17：latched 事件（只做 latched 这一类，immediate 类不上）
//
// 为什么要有它：交换链重建那条路上，谁先谁后是写死在
// Application::recreateSwapchain() 的十一步里的 —— 加一个 Pass 就得去那里
// 记得补一行，忘了就是花屏或崩。改成"发布一次事件、订阅者自己响应"之后，
// 新 Pass 只要订阅就自动跟上，Application 那边不再认识每个 Pass。
//
// latched 的含义：事件值会被记住。**后订阅的人立刻收到一次**，不用等下一次
// 发布。这正是"谁先初始化"的解药 —— 订阅晚的人也能拿到已经发生的事实。
//
// 只做 latched 的原因：immediate（键盘、鼠标）需要每帧分发、要处理顺序与
// 消费，属于输入系统的活；这里要的只是"状态变化广播"。
// ============================================================================

namespace tiny::events {

class EventBus;

// 订阅句柄：RAII。析构即退订，所以订阅者不用在析构里记得清理；
// 拷贝被禁掉，避免同一份订阅被退订两次。
class Subscription
{
public:
    Subscription() = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    ~Subscription();

    void reset();
    bool valid() const { return bus != nullptr && id != 0; }

private:
    friend class EventBus;
    Subscription(EventBus* owner, std::type_index eventType, uint64_t subscriptionId);

    EventBus* bus = nullptr;
    std::type_index eventType = std::type_index(typeid(void));
    uint64_t id = 0;
};

class EventBus
{
public:
    // 订阅。若此前已经发布过同一类型的事件，handler 会立刻被调用一次（latched）。
    // 补发的是**一份拷贝**：handler 里再发布同类事件也不会让这次回调读到野值。
    template <typename Event>
    Subscription subscribe(std::function<void(const Event&)> handler)
    {
        Latch& latch = latchFor(std::type_index(typeid(Event)));
        const uint64_t subscriptionId = latch.nextId++;
        latch.entries.push_back(Entry{
            subscriptionId,
            [handler](const void* payload) { handler(*static_cast<const Event*>(payload)); }
        });

        if (latch.value)
        {
            const Event replay = *static_cast<const Event*>(latch.value.get());
            handler(replay);
        }
        return Subscription(this, std::type_index(typeid(Event)), subscriptionId);
    }

    // 发布。先存值（供后订阅者补发），再按订阅顺序回调。
    // 回调期间允许订阅/退订：回调列表先拷一份，这一轮按拷贝走。
    template <typename Event>
    void publish(const Event& event)
    {
        Latch& latch = latchFor(std::type_index(typeid(Event)));
        latch.value = std::make_shared<Event>(event);
        const void* payload = latch.value.get();

        const std::vector<Entry> snapshot = latch.entries;
        for (const Entry& entry : snapshot)
            entry.invoke(payload);
    }

    // 最近一次发布的值；从没发布过返回 nullptr。
    template <typename Event>
    const Event* latest() const
    {
        const auto it = latches.find(std::type_index(typeid(Event)));
        if (it == latches.end() || !it->second.value)
            return nullptr;
        return static_cast<const Event*>(it->second.value.get());
    }

    template <typename Event>
    std::size_t subscriberCount() const
    {
        const auto it = latches.find(std::type_index(typeid(Event)));
        return it == latches.end() ? 0u : it->second.entries.size();
    }

    // 打印用（非模板部分在 EventBus.cpp）：有 latched 值的类型数。
    std::size_t latchedTypeCount() const { return latches.size(); }

private:
    friend class Subscription;

    struct Entry
    {
        uint64_t id = 0;
        std::function<void(const void*)> invoke;
    };

    struct Latch
    {
        std::shared_ptr<void> value;   // 最近一次发布的值（latched 的核心）
        std::vector<Entry> entries;
        uint64_t nextId = 1;
    };

    Latch& latchFor(std::type_index eventType)
    {
        return latches[eventType];
    }

    void unsubscribe(std::type_index eventType, uint64_t subscriptionId);

    std::unordered_map<std::type_index, Latch> latches;
};

// 全局唯一实例（函数内静态，避免静态初始化顺序问题）。
EventBus& bus();

// 纯逻辑自检：补发、退订失效、多订阅按序、未发布时 latest() 为空。
bool selfTest();

// 打印每个已登记事件的：是否有值、订阅者数量（--print-events 用）。
void print(std::FILE* out);

} // namespace tiny::events
