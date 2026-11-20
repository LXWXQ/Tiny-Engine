#include "RHI/ShaderReload.h"

#include "Core/AssetPath.h"
#include "RHI/Pipeline.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace tiny::hotreload
{
    namespace
    {
        // 一条登记 = 一个 Pipeline 或一个 ComputePipeline。持有的是**原始路径**
        // （构建期写进宏的那种），每次取值都走 assets::resolve，所以引擎换了目录
        // 之后同一份登记依然有效。
        struct Entry
        {
            Pipeline* graphics = nullptr;
            ComputePipeline* compute = nullptr;
            std::string vertPath;
            std::string fragPath;
            int64_t vertStamp = -1;
            int64_t fragStamp = -1;
        };

        std::vector<Entry> g_entries;
        bool g_enabled = false;
        VkDevice g_device = VK_NULL_HANDLE;
        uint32_t g_reloadCount = 0;
        std::string g_lastResult = "（还没有重载过）";

        // 单独抽出来是为了能被 selfTest 直接测：热重载的判定只有这一条规则，
        // 但它有三个边界（第一次见到、文件消失、同一个 mtime 连续看到），
        // 每一个写错都会表现为"启动就重建一遍"或"改了文件没反应"。
        struct StampTracker
        {
            int64_t stored = -1;

            // 返回 true 表示"文件真的变了"。第一次见到只记录不触发；
            // 文件消失（-1）不触发也不清空记录，否则 IDE 的"先删后写"会误报两次。
            bool observe(int64_t stamp)
            {
                if (stamp < 0)
                    return false;
                if (stored < 0)
                {
                    stored = stamp;
                    return false;
                }
                if (stamp == stored)
                    return false;
                stored = stamp;
                return true;
            }
        };

        int64_t stampOf(const std::string& path)
        {
            std::error_code ec;
            const std::filesystem::path resolved(tiny::assetpath::resolve(path));
            const auto stamp = std::filesystem::last_write_time(resolved, ec);
            if (ec)
                return -1;
            return static_cast<int64_t>(stamp.time_since_epoch().count());
        }

        void eraseGraphics(Pipeline* pipeline)
        {
            g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                           [pipeline](const Entry& entry) { return entry.graphics == pipeline; }),
                            g_entries.end());
        }

        void eraseCompute(ComputePipeline* pipeline)
        {
            g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                           [pipeline](const Entry& entry) { return entry.compute == pipeline; }),
                            g_entries.end());
        }
    }

    void setEnabled(bool value)
    {
        g_enabled = value;
    }

    bool enabled()
    {
        return g_enabled;
    }

    void setDevice(VkDevice device)
    {
        g_device = device;
    }

    void registerGraphics(Pipeline* pipeline, const std::string& vertPath, const std::string& fragPath)
    {
        if (pipeline == nullptr)
            return;

        // 同一条管线重复登记（不该发生，但构造函数被复制时会）直接替换，
        // 否则 poll 会对同一个对象连续重建两次。
        eraseGraphics(pipeline);

        Entry entry;
        entry.graphics = pipeline;
        entry.vertPath = vertPath;
        entry.fragPath = fragPath;
        entry.vertStamp = stampOf(vertPath);
        entry.fragStamp = stampOf(fragPath);
        g_entries.push_back(std::move(entry));
    }

    void unregisterGraphics(Pipeline* pipeline)
    {
        eraseGraphics(pipeline);
    }

    void registerCompute(ComputePipeline* pipeline, const std::string& compPath)
    {
        if (pipeline == nullptr)
            return;

        eraseCompute(pipeline);

        Entry entry;
        entry.compute = pipeline;
        entry.fragPath = compPath;
        entry.fragStamp = stampOf(compPath);
        g_entries.push_back(std::move(entry));
    }

    void unregisterCompute(ComputePipeline* pipeline)
    {
        eraseCompute(pipeline);
    }

    bool poll()
    {
        if (!g_enabled || g_entries.empty())
            return false;

        // 先把"谁变了"全找出来，再统一等一次设备空闲。逐条等会卡成 N 次 idle。
        // 同时记下**是哪个文件**变了 —— 日志里报顶点着色器而实际改的是片元，
        // 会让人以为重载认错了对象。
        std::vector<std::pair<Entry*, std::string>> dirty;
        for (Entry& entry : g_entries)
        {
            StampTracker vert;
            vert.stored = entry.vertStamp;
            StampTracker frag;
            frag.stored = entry.fragStamp;

            std::string changedName;
            if (!entry.vertPath.empty() && vert.observe(stampOf(entry.vertPath)))
                changedName = entry.vertPath;
            if (!entry.fragPath.empty() && frag.observe(stampOf(entry.fragPath)))
                changedName = entry.fragPath;

            entry.vertStamp = vert.stored;
            entry.fragStamp = frag.stored;

            if (!changedName.empty())
                dirty.emplace_back(&entry, changedName);
        }

        if (dirty.empty())
            return false;

        if (g_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_device);

        uint32_t rebuilt = 0;
        for (const auto& item : dirty)
        {
            Entry* entry = item.first;
            const std::string& name = item.second;
            try
            {
                if (entry->graphics != nullptr)
                    entry->graphics->reloadShaders();
                if (entry->compute != nullptr)
                    entry->compute->reloadShaders();
                ++rebuilt;
                std::cout << "[HotReload] 重建管线：" << name << "\n";
            }
            catch (const std::exception& e)
            {
                // 热重载失败不能把引擎带走：旧管线已经在 reloadShaders 里被销毁，
                // 这条管线本帧起就是空的，下一帧仍是空 —— 报错里说清是哪条。
                std::cout << "[HotReload] 重建失败：" << name << " | " << e.what() << "\n";
            }
        }

        g_reloadCount += rebuilt;
        g_lastResult = rebuilt > 0 ? "成功" : "失败（见上一行）";
        return rebuilt > 0;
    }

    void report(std::FILE* out)
    {
        if (out == nullptr)
            return;

        std::fprintf(out, "[HotReload] 登记 %zu 条管线 | 开关 %s | 累计重建 %u 次 | 上次结果 %s\n",
                     g_entries.size(), g_enabled ? "开" : "关", g_reloadCount, g_lastResult.c_str());
    }

    bool selfTest()
    {
        // 1. 第一次见到只记录
        StampTracker tracker;
        bool ok = !tracker.observe(100) && tracker.stored == 100;

        // 2. 同一个 mtime 不重复触发
        ok = ok && !tracker.observe(100);

        // 3. 变了要触发，且只触发一次
        ok = ok && tracker.observe(200);
        ok = ok && !tracker.observe(200);
        ok = ok && tracker.stored == 200;

        // 4. 文件消失（-1）不触发、也不丢记录
        ok = ok && !tracker.observe(-1) && tracker.stored == 200;

        // 5. 消失后再出现（mtime 不同）算变化
        ok = ok && tracker.observe(300);

        // 6. 从"没见过"直接消失，仍然保持 -1（不能让 stored 变成 0 之外的哨兵）
        StampTracker missing;
        ok = ok && !missing.observe(-1) && missing.stored == -1;

        if (ok)
            std::cout << "[HotReload] selfTest PASS | mtime 变化检测（首次 / 重复 / 消失 / 复现）\n";
        return ok;
    }
}