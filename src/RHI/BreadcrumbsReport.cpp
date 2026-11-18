// ============================================================================
// GPU breadcrumbs —— 报告与格式化
//
// 从 Breadcrumbs.cpp 拆出来只为一件事：那个文件承担「往命令缓冲里写检查点」的
// 全部逻辑，再塞进「读游标 + 查表 + 排版 + 落盘」就过 400 行了。两者改动理由
// 完全不同（前者跟着渲染图走，后者跟着日志/故障排查走），拆开更划算。
// ============================================================================
#include "RHI/Breadcrumbs.h"
#include "RHI/Device.h"
#include "Core/StartupLog.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

void Breadcrumbs::collectCursorsFromQueue(VkQueue queue, uint32_t* cursors) const
{
    if (queue == VK_NULL_HANDLE || pfnGetQueueCheckpointData2NV == nullptr)
        return;

    uint32_t count = 0;
    pfnGetQueueCheckpointData2NV(queue, &count, nullptr);
    if (count == 0)
        return;

    std::vector<VkCheckpointData2NV> data(count);
    for (VkCheckpointData2NV& item : data)
    {
        item.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
        item.pNext = nullptr;
        item.pCheckpointMarker = nullptr;
    }

    pfnGetQueueCheckpointData2NV(queue, &count, data.data());

    // 返回顺序不可信，但 counter 是单调递增的，所以"每槽取最大"就是
    // "最后执行到的那一条"。
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t marker = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker));
        const uint32_t slot = static_cast<uint32_t>(marker >> 32);
        const uint32_t counter = static_cast<uint32_t>(marker & 0xFFFFFFFFull);
        if (slot < kSlotCount)
            cursors[slot] = std::max(cursors[slot], counter);
    }
}

void Breadcrumbs::collectCursorsFromMemory(uint32_t* cursors) const
{
    if (cursorMapped == nullptr)
        return;

    for (uint32_t i = 0; i < kSlotCount; ++i)
        cursors[i] = std::max(cursors[i], cursorMapped[i]);
}

std::string Breadcrumbs::formatSlot(uint32_t slot, uint32_t cursor) const
{
    const Slot& s = slots[slot];
    const char* queueName = (slot % 2u == 0u) ? "graphics" : "compute";

    char head[256];

    if (!s.recorded)
    {
        std::snprintf(head, sizeof(head), "  槽 %u (%s)：本次运行没有开始过", slot, queueName);
        return head;
    }

    if (s.counter == 0)
    {
        std::snprintf(head, sizeof(head), "  槽 %u (%s, frame %llu)：命令缓冲已开始，但没有任何检查点",
                      slot, queueName, static_cast<unsigned long long>(s.frameNumber));
        return head;
    }

    // entries 里的 counter 单调递增，找到最后一个 counter <= cursor 的条目。
    int hitIndex = -1;
    for (size_t i = 0; i < s.entries.size(); ++i)
    {
        if (s.entries[i].counter <= cursor)
            hitIndex = static_cast<int>(i);
        else
            break;
    }

    std::string out;

    if (cursor == kCompleted)
    {
        std::snprintf(head, sizeof(head), "  槽 %u (%s, frame %llu)：已跑完，共 %u 个检查点 —— 崩溃不在这里",
                      slot, queueName, static_cast<unsigned long long>(s.frameNumber), s.counter);
        out += head;
        out += "\n";
        return out;
    }

    if (hitIndex < 0)
    {
        std::snprintf(head, sizeof(head), "  槽 %u (%s, frame %llu)：崩溃发生在第一个检查点之前",
                      slot, queueName, static_cast<unsigned long long>(s.frameNumber));
        out += head;
        out += "\n";
        return out;
    }

    const Entry& hit = s.entries[static_cast<size_t>(hitIndex)];
    std::snprintf(head, sizeof(head), "  槽 %u (%s, frame %llu)：崩溃点 #%u \"%s\"（共记录 %u 个检查点）",
                  slot, queueName, static_cast<unsigned long long>(s.frameNumber),
                  hit.counter, hit.label, s.counter);
    out += head;
    out += "\n";

    // 崩溃点之前的最后几条，等于一张"崩溃前的调用栈"。
    const int trailStart = std::max(0, hitIndex - 8);
    if (hitIndex > trailStart)
    {
        out += "      之前：";
        for (int i = trailStart; i < hitIndex; ++i)
        {
            out += "#";
            out += std::to_string(s.entries[static_cast<size_t>(i)].counter);
            out += " ";
            out += s.entries[static_cast<size_t>(i)].label;
            if (i + 1 < hitIndex)
                out += " -> ";
        }
        out += "\n";
    }

    return out;
}

std::string Breadcrumbs::reportAfterDeviceLost(VkQueue queue, const std::string& directory)
{
    if (!isActive() || device == nullptr)
        return std::string();

    std::vector<uint32_t> cursors(kSlotCount, 0u);

    if (mode == Mode::NvCheckpoint)
    {
        // 两条队列都查：出问题的那条给"崩溃点"，另一条给"它跑完了"的对照。
        VkQueue handles[3] = { queue, device->getGraphicsQueue(), device->getComputeQueue() };
        for (uint32_t i = 0; i < 3; ++i)
        {
            if (handles[i] == VK_NULL_HANDLE)
                continue;

            bool duplicate = false;
            for (uint32_t j = 0; j < i; ++j)
            {
                if (handles[j] == handles[i])
                {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate)
                collectCursorsFromQueue(handles[i], cursors.data());
        }
    }
    else
    {
        collectCursorsFromMemory(cursors.data());
    }

    std::string text;
    text += "标记模式：";
    text += getModeName();
    text += "\n";

    for (uint32_t i = 0; i < kSlotCount; ++i)
        text += formatSlot(i, cursors[i]);

    text += "（对照同一份日志里 device_fault 的地址信息一起看：本段给出"
            "是哪一帧的哪个 pass，device_fault 给出是哪个地址越界。）\n";

    std::cout << "[Breadcrumbs] device lost 报告：\n" << text;

    return tiny::writeStartupErrorLog("GPU breadcrumbs", text, directory);
}
