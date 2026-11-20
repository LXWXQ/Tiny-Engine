#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class Pipeline;
class ComputePipeline;

// ---------------------------------------------------------------------------
// TM-16：着色器热重载（.spv 变了就重建管线，不重启引擎）
//
// 为什么只监控 .spv、不在运行期编译 GLSL：
//   引擎的着色器是构建期由 glslangValidator 编译 + spirv-val 校验的，
//   运行期再塞一份编译器进来，等于让"运行时行为"和"CI 验证过的行为"分家。
//   现在的迭代闭环是：改 .frag -> 增量构建（只编一个 shader，1 秒内）-> 引擎
//   在下一帧发现 .spv 的 mtime 变了 -> 重建管线 -> 画面更新。引擎不用重启。
//
// 为什么是轮询而不是 ReadDirectoryChangesW：
//   .spv 一共几十个文件，一次 stat 只花几十微秒；轮询没有线程、没有重叠 IO、
//   没有关闭句柄的时序问题，而且行为在任何机器上完全一致。等到要监控上千个
//   文件时再换通知机制，接口不用变（poll 内部换实现即可）。
//
// 边界：只重建 VkPipeline / VkShaderModule。管线布局、描述符集布局、渲染图的
//   附件格式都不动 —— 改了 binding 数量或附件格式必须重启（接口变了）。
// ---------------------------------------------------------------------------
namespace tiny::hotreload
{
    void setEnabled(bool value);
    bool enabled();

    // vkDeviceWaitIdle 需要设备句柄；Application 建完 Device 后调一次即可。
    // 不把 Device& 塞进 register* 是为了让 Pipeline 的登记保持"只关心路径"。
    void setDevice(VkDevice device);

    // Pipeline / ComputePipeline 的构造函数会自动登记，析构自动注销
    void registerGraphics(Pipeline* pipeline, const std::string& vertPath, const std::string& fragPath);
    void unregisterGraphics(Pipeline* pipeline);
    void registerCompute(ComputePipeline* pipeline, const std::string& compPath);
    void unregisterCompute(ComputePipeline* pipeline);

    // 每帧调用（Application::run 的循环里）。发现了变化才会 vkDeviceWaitIdle 并重建，
    // 返回 true 表示这一帧确实重建过，调用方可以据此打日志/暂停统计。
    bool poll();

    // 打印当前登记了多少条管线、最近一次重载的结果（--print-assets 里顺带打）
    void report(std::FILE* out);

    // 纯逻辑自检：mtime 变化检测（含"文件消失"与"第一次见到"两种边界）
    bool selfTest();
}