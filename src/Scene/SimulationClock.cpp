#include "SimulationClock.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace scene
{

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "[Sim] clockSelfTest FAIL: " << message << "\n";
    return condition;
}

bool near(float a, float b, float epsilon = 1.0e-4f)
{
    return std::fabs(a - b) <= epsilon;
}
} // namespace

void SimulationClock::setFixedStep(float seconds)
{
    // 非法步长直接忽略：宁可保持上一次的合法值，也不要让 accumulator 除零。
    if (!std::isfinite(seconds) || seconds <= 0.0f)
        return;
    fixedStep_ = seconds;
}

void SimulationClock::reset()
{
    accumulator_ = 0.0f;
    tick_ = 0;
    dropped_ = 0;
    jumpLatched_ = false;
}

uint32_t SimulationClock::advance(float frameDelta, const InputCmd& cmd,
                                  const std::function<void(const InputCmd&, float)>& step)
{
    if (!std::isfinite(frameDelta) || frameDelta < 0.0f)
        frameDelta = 0.0f;
    if (frameDelta > kMaxFrameDelta)
        frameDelta = kMaxFrameDelta;   // 护栏 1：卡顿时不追补几百个 tick

    accumulator_ += frameDelta;
    // 护栏 3：本帧按下的跳锁存下来，等下一个真正执行的 tick 消费。
    jumpLatched_ = jumpLatched_ || cmd.jump;

    // 浮点容差：1.0f / 60.0f 累加 60 次会略小于 1 秒（或略大于），直接用
    // >= 比较会稳定地少跑一个 tick。这里允许千分之一步的误差，并在减完后
    // 把负数夹到 0，避免误差累积。
    const float tickEpsilon = fixedStep_ * 1.0e-3f;

    uint32_t steps = 0;
    while (accumulator_ >= fixedStep_ - tickEpsilon)
    {
        if (steps >= kMaxStepsPerFrame)
        {
            // 护栏 2：丢掉超出的累积量，让模拟变慢而不是越追越卡。
            dropped_ += static_cast<uint32_t>(accumulator_ / fixedStep_);
            accumulator_ = std::fmod(accumulator_, fixedStep_);
            break;
        }

        accumulator_ -= fixedStep_;
        if (accumulator_ < 0.0f)
            accumulator_ = 0.0f;
        ++tick_;

        if (step)
        {
            InputCmd current = cmd;
            current.tick = tick_;
            current.jump = jumpLatched_;
            jumpLatched_ = false;
            step(current, fixedStep_);
        }
        else
        {
            // 没有回调时也要消费掉 latch，避免它一直挂到未来的某一帧。
            jumpLatched_ = false;
        }
        ++steps;
    }
    return steps;
}

bool simulationClockSelfTest()
{
    bool ok = true;

    // 1) 默认参数：60Hz，未推进时一切归零。
    {
        SimulationClock clock;
        ok = expect(near(clock.fixedStep(), 1.0f / 60.0f), "default fixed step is 1/60") && ok;
        ok = expect(clock.tick() == 0u && near(clock.alpha(), 0.0f), "fresh clock is idle") && ok;
    }

    // 2) 一秒的帧（60 x 1/60）刚好 60 个 tick。
    //    注意不能一次 advance(1.0f)：护栏 1 会把单帧截到 kMaxFrameDelta。
    {
        SimulationClock clock;
        for (int i = 0; i < 60; ++i)
            clock.advance(1.0f / 60.0f, InputCmd{}, nullptr);
        ok = expect(clock.tick() == 60u, "one second is 60 ticks") && ok;
        ok = expect(clock.alpha() < 0.01f, "one second leaves no remainder") && ok;
    }

    // 3) 抖动帧：30 帧 x 1/30 秒，tick 数仍然只由总时间决定。
    {
        SimulationClock clock;
        for (int i = 0; i < 30; ++i)
            clock.advance(1.0f / 30.0f, InputCmd{}, nullptr);
        ok = expect(clock.tick() == 60u, "jittered frames still yield 60 ticks") && ok;
    }

    // 4) 不足一个 tick 的帧：不推进，alpha 落在 (0,1)。
    {
        SimulationClock clock;
        const uint32_t steps = clock.advance(0.001f, InputCmd{}, nullptr);
        ok = expect(steps == 0u && clock.tick() == 0u, "sub-tick frame runs nothing") && ok;
        ok = expect(clock.alpha() > 0.0f && clock.alpha() < 1.0f, "alpha stays in [0,1)") && ok;
    }

    // 5) 跳跃锁存：0 tick 的帧里按下跳，下一个 tick 必须收到，且只收到一次。
    {
        SimulationClock clock;
        InputCmd press;
        press.jump = true;
        clock.advance(0.001f, press, nullptr);          // 这一帧一个 tick 都没跑

        int jumpsSeen = 0;
        int ticksSeen = 0;
        InputCmd release;
        clock.advance(0.05f, release,
                      [&](const InputCmd& current, float)
                      {
                          ++ticksSeen;
                          if (current.jump)
                              ++jumpsSeen;
                      });
        ok = expect(ticksSeen >= 2, "two ticks ran in the second frame") && ok;
        ok = expect(jumpsSeen == 1, "queued jump is consumed exactly once") && ok;
    }

    // 6) 卡顿上限：一次给 1 秒也只跑 kMaxStepsPerFrame 个 tick，其余记 dropped。
    {
        SimulationClock clock;
        const uint32_t steps = clock.advance(1.0f, InputCmd{}, nullptr);
        ok = expect(steps == SimulationClock::kMaxStepsPerFrame, "long frame is capped by max steps") && ok;
        ok = expect(clock.droppedTicks() > 0u, "capped frame reports dropped ticks") && ok;
        ok = expect(clock.alpha() < 1.0f, "capped frame leaves accumulator below one step") && ok;
    }

    // 7) 步长可配 + reset 清空全部状态。
    {
        SimulationClock clock;
        clock.setFixedStep(0.02f);
        for (int i = 0; i < 50; ++i)
            clock.advance(0.02f, InputCmd{}, nullptr);
        ok = expect(clock.tick() == 50u, "50Hz clock yields 50 ticks per second") && ok;

        clock.reset();
        ok = expect(clock.tick() == 0u && clock.droppedTicks() == 0u && near(clock.alpha(), 0.0f),
                    "reset clears tick / dropped / accumulator") && ok;
        ok = expect(near(clock.fixedStep(), 0.02f), "reset keeps the configured step") && ok;
    }

    // 8) 回调拿到的 tick 号与时钟一致，且逐 tick 递增。
    {
        SimulationClock clock;
        std::vector<uint32_t> seen;
        clock.advance(0.05f, InputCmd{},
                      [&](const InputCmd& current, float dt)
                      {
                          expect(near(dt, 1.0f / 60.0f), "callback receives the fixed step");
                          seen.push_back(current.tick);
                      });
        bool increasing = true;
        for (std::size_t i = 0; i < seen.size(); ++i)
        {
            if (seen[i] != static_cast<uint32_t>(i + 1))
                increasing = false;
        }
        ok = expect(!seen.empty() && increasing && seen.back() == clock.tick(),
                    "callback tick numbers match the clock") && ok;
    }

    if (ok)
        std::cout << "[Sim] clockSelfTest PASS | cases=8 fixed=1/60 max-steps=8 jump-latch=ok\n";
    return ok;
}

} // namespace scene
