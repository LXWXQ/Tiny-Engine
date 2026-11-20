#include "Core/EventBus.h"
#include "Core/AppEvents.h"

#include <cstring>
#include <vector>

namespace tiny::events {

Subscription::Subscription(EventBus* owner, std::type_index type, uint64_t subscriptionId)
    : bus(owner), eventType(type), id(subscriptionId)
{
}

Subscription::Subscription(Subscription&& other) noexcept
    : bus(other.bus), eventType(other.eventType), id(other.id)
{
    other.bus = nullptr;
    other.id = 0;
}

Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        reset();
        bus = other.bus;
        eventType = other.eventType;
        id = other.id;
        other.bus = nullptr;
        other.id = 0;
    }
    return *this;
}

Subscription::~Subscription()
{
    reset();
}

void Subscription::reset()
{
    if (bus != nullptr && id != 0)
        bus->unsubscribe(eventType, id);
    bus = nullptr;
    id = 0;
}

void EventBus::unsubscribe(std::type_index eventType, uint64_t subscriptionId)
{
    const auto it = latches.find(eventType);
    if (it == latches.end())
        return;

    std::vector<Entry>& entries = it->second.entries;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].id == subscriptionId)
        {
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

EventBus& bus()
{
    static EventBus instance;
    return instance;
}

bool selfTest()
{
    // 用一个只有这里会用的事件类型，避免污染引擎真实事件的统计。
    struct Probe
    {
        int sequence = 0;
    };

    EventBus local;
    bool ok = true;

    // 1) 没发布过时 latest() 必须是空。
    ok = ok && local.latest<Probe>() == nullptr;

    int first = 0;
    int second = 0;
    std::vector<int> order;

    Subscription a = local.subscribe<Probe>([&](const Probe& e) { first += e.sequence; });
    Subscription b = local.subscribe<Probe>([&](const Probe& e) { second += e.sequence; });

    // 2) 未发布前，两个订阅者都没被调用。
    ok = ok && first == 0 && second == 0;

    local.publish(Probe{ 7 });
    ok = ok && first == 7 && second == 7;

    // 3) 后订阅者立刻收到补发（latched 的核心语义）。
    int late = 0;
    Subscription c = local.subscribe<Probe>([&](const Probe& e) { late += e.sequence; });
    ok = ok && late == 7;

    // 4) 退订之后不再收到。
    a.reset();
    local.publish(Probe{ 3 });
    ok = ok && first == 7 && second == 10 && late == 10;

    // 5) 多个订阅者按注册顺序回调。
    EventBus ordered;
    Subscription o1 = ordered.subscribe<Probe>([&](const Probe&) { order.push_back(1); });
    Subscription o2 = ordered.subscribe<Probe>([&](const Probe&) { order.push_back(2); });
    ordered.publish(Probe{ 0 });
    ok = ok && order.size() == 2 && order[0] == 1 && order[1] == 2;

    // 6) 回调里退订自己，不允许把这一轮剩下的订阅者漏掉。
    EventBus reentrant;
    int preserved = 0;
    Subscription r1 = reentrant.subscribe<Probe>([&](const Probe&) { r1.reset(); });
    Subscription r2 = reentrant.subscribe<Probe>([&](const Probe&) { ++preserved; });
    reentrant.publish(Probe{ 0 });
    ok = ok && preserved == 1;

    // 7) 订阅计数与 type 数对得上。
    ok = ok && local.subscriberCount<Probe>() == 2 && local.latchedTypeCount() == 1;

    std::printf(ok ? "[Events] selfTest PASS | latched 补发 + 退订失效 + 顺序稳定\n"
                   : "[Events] selfTest FAIL\n");
    return ok;
}

void print(std::FILE* out)
{
    if (out == nullptr)
        return;

    const EventBus& events = bus();
    std::fprintf(out, "[Events] latched 事件（%zu 类）\n", events.latchedTypeCount());

    const SwapchainRecreated* swapchain = events.latest<SwapchainRecreated>();
    if (swapchain != nullptr)
    {
        std::fprintf(out,
            "  SwapchainRecreated      值=%ux%u（渲染 %ux%u，%u 张，firstCreation=%d）订阅=%zu\n",
            swapchain->swapchainWidth, swapchain->swapchainHeight,
            swapchain->renderWidth, swapchain->renderHeight,
            swapchain->imageCount, swapchain->firstCreation ? 1 : 0,
            events.subscriberCount<SwapchainRecreated>());
    }
    else
    {
        std::fprintf(out, "  SwapchainRecreated      值=（还没发布过）订阅=%zu\n",
            events.subscriberCount<SwapchainRecreated>());
    }

    const RenderTargetsResized* targets = events.latest<RenderTargetsResized>();
    if (targets != nullptr)
    {
        std::fprintf(out, "  RenderTargetsResized    值=%ux%u 订阅=%zu\n",
            targets->width, targets->height, events.subscriberCount<RenderTargetsResized>());
    }
    else
    {
        std::fprintf(out, "  RenderTargetsResized    值=（还没发布过）订阅=%zu\n",
            events.subscriberCount<RenderTargetsResized>());
    }
}

} // namespace tiny::events
