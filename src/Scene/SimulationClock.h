#pragma once

#include <cstdint>
#include <functional>

#include <glm/glm.hpp>

namespace scene
{

// ---------------------------------------------------------------------------
// M1-C1：固定步长模拟时钟
//
// 为什么需要它：主循环把 glfwGetTime 的帧间隔直接喂给角色控制器与 effects
// 系统，于是"1 秒走多远"取决于这台机器跑多少帧 —— 同一串按键在 60fps 与
// 144fps 下走出不同轨迹，也没法重放（D2 的预测/重放要求同一串输入产生逐位
// 相同的结果）。
//
// 这里把"模拟"和"渲染"拆开：
//   * 渲染 / UI / 相机仍用可变帧间隔，否则鼠标视角会被量化成 60Hz 台阶；
//   * 模拟按固定步长推进，帧间隔只累加到 accumulator。
//
// 三条护栏（细节与风险见 docs/30-systems/simulation-tick.md）：
//   1) 单帧帧间隔上限 kMaxFrameDelta：卡顿时按上限算，避免一次补几百个 tick；
//   2) 单帧最多 kMaxStepsPerFrame 个 tick：宁可让模拟"变慢"，也不要越追越卡；
//   3) 跳跃是边沿事件，用 latch 保存到下一个 tick 消费 —— 144fps 下有些帧
//      一个 tick 都不跑，不锁存的话按下的跳会被直接吞掉。
// ---------------------------------------------------------------------------
struct InputCmd
{
    // 采样时就按相机 / 角色 yaw 转成世界方向：tick 内不再读 ImGui，也不受
    // "这一帧相机转了多少"影响。
    glm::vec3 moveWorld{ 0.0f, 0.0f, 0.0f };
    bool jump = false;      // 边沿：本帧刚按下
    bool sprint = false;    // 电平：按住期间每个 tick 都为 true
    uint32_t tick = 0;      // 由时钟填写，回调里可用来对齐录制数据
};

class SimulationClock
{
public:
    static constexpr float kDefaultFixedStep = 1.0f / 60.0f;
    static constexpr float kMaxFrameDelta = 0.25f;
    static constexpr uint32_t kMaxStepsPerFrame = 8;

    void setFixedStep(float seconds);
    float fixedStep() const { return fixedStep_; }

    // 进入 / 退出播放时清空：accumulator 归零，避免恢复后追帧。
    void reset();

    // 返回本帧实际执行的 tick 数。step 为空时只推进时钟（自检用）。
    uint32_t advance(float frameDelta, const InputCmd& cmd,
                     const std::function<void(const InputCmd&, float)>& step);

    // accumulator / fixedStep，留给渲染插值（C1c）用。
    float alpha() const { return fixedStep_ > 0.0f ? accumulator_ / fixedStep_ : 0.0f; }
    uint32_t tick() const { return tick_; }
    uint32_t droppedTicks() const { return dropped_; }

private:
    float fixedStep_ = kDefaultFixedStep;
    float accumulator_ = 0.0f;
    uint32_t tick_ = 0;
    uint32_t dropped_ = 0;
    bool jumpLatched_ = false;
};

// 时钟契约自检（不需要 Vulkan / 窗口 / 文件系统）。
bool simulationClockSelfTest();

} // namespace scene
