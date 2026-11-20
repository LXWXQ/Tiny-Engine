#pragma once
#include "IRenderPass.h"
#include "RHI/RenderGraph.h"
#include "Core/AppEvents.h"
#include "Core/EventBus.h"

// 有序持有若干 IRenderPass，逐通道执行。
// D-1(b) 起改为「注册进渲染图」，而不是各自录制命令。
class RenderPipeline
{
public:
    void addPass(std::unique_ptr<IRenderPass> pass)
    {
        passes.push_back(std::move(pass));
    }

    void initAll()
    {
        for (auto& pass : passes) pass->init();
    }

    // Pass registration order matters: Geometry updates imported G-Buffer
    // views, then SSAO/Lighting resize their targets, then Bloom/Present rewrite
    // descriptors that reference those targets.
    void resizeAll(VkExtent2D extent)
    {
        for (auto& pass : passes) pass->resize(extent);
    }

    // TM-17：订阅"尺寸变了"，不再由 Application 手写 resizeAll。
    // 必须在 initAll() 之后调用：Pass 的 resize 会重建附件与导入视图，
    // 早于 init 调等于拿半初始化的对象干活。订阅晚也能拿到已经发生过的事件
    // （latched），所以顺序由这里显式控制，而不是靠"谁先构造"。
    void attachResizeEvents()
    {
        swapchainSubscription = tiny::events::bus().subscribe<tiny::events::SwapchainRecreated>(
            [this](const tiny::events::SwapchainRecreated& event)
            {
                if (passes.empty())
                    return;
                resizeAll(VkExtent2D{ event.renderWidth, event.renderHeight });
            });

        targetsSubscription = tiny::events::bus().subscribe<tiny::events::RenderTargetsResized>(
            [this](const tiny::events::RenderTargetsResized& event)
            {
                if (passes.empty())
                    return;
                resizeAll(VkExtent2D{ event.width, event.height });
            });
    }

    // 显式销毁所有 Pass。之所以不依赖成员的自动析构：
    // Pass 里持有 VMA 分配（SSAO 的 UBO 等），而销毁顺序一旦排错，
    // 这些分配就会活到 vmaDestroyAllocator 之后，触发 VMA 的断言崩溃
    // （断言只说"有分配没释放"，不说是谁，非常难查）。
    // 让调用方显式控制这个时机，比"藏在成员析构顺序里"安全得多。
    void clearAll() { passes.clear(); }

    // 把所有 Pass 注册进渲染图（渲染图内部按注册顺序执行）
    void registerAll(RenderGraph& graph, FrameInfo& frameInfo)
    {
        for (auto& pass : passes) pass->addToGraph(graph, frameInfo);
    }

    const std::vector<std::unique_ptr<IRenderPass>>& getPasses() const
    {
        return passes;
    }

    const IRenderPass* findPass(const std::string& name) const
    {
        for (auto& p : passes) if (p->getName() == name) return p.get();
        return nullptr;
    }

private:
    std::vector<std::unique_ptr<IRenderPass>> passes;
    tiny::events::Subscription swapchainSubscription;
    tiny::events::Subscription targetsSubscription;
};