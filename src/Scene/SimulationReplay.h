#pragma once

// 输入录制与回放（D2）：把"每 tick 吃了什么输入"记下来，再用同样的输入重跑，
// 逐 tick 比对状态哈希。
//
// 为什么需要它：C1 保证了 tick 数一致，D2a 保证了单 tick 的角色步进是纯函数，
// 但"两遍运行的轨迹真的逐位相同"仍然要靠一次录制 + 一次回放来证明。编辑器在
// 播放/运行期间录制，停止时打印 `[Sim] replay | ticks=N hash=0x…` —— 手动跑两遍
// 同样的操作，哈希相同就说明这一局是可复现的。

#include "Scene/CharacterSimulation.h"
#include "Scene/SimulationClock.h"

#include <cstdint>
#include <vector>

namespace scene
{

struct ReplayTick
{
    uint32_t tick = 0;
    glm::vec3 moveWorld{ 0.0f };
    bool jump = false;
    bool sprint = false;
};

class ReplayRecorder
{
public:
    void clear();
    void record(const InputCmd& cmd);

    std::size_t size() const { return ticks_.size(); }
    const std::vector<ReplayTick>& ticks() const { return ticks_; }
    uint64_t hash() const { return hash_; }

private:
    std::vector<ReplayTick> ticks_;
    uint64_t hash_ = 1469598103934665603ull;
};

// 状态哈希：位置/竖直速度/接地标记 + tick 号，FNV-1a。
uint64_t hashCharacterState(const CharacterSimState& state, uint32_t tick);

// 契约自检：录一段（144Hz 抖动帧驱动）再回放，逐 tick 哈希必须一致；
// 同一串输入录两次，录制器哈希也必须一致。
bool simulationReplaySelfTest();

} // namespace scene
