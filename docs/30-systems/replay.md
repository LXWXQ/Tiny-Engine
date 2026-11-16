---
id: systems/replay
title: 输入录制与回放（D2）
type: system
scope: src/Scene/CharacterSimulation.*, src/Scene/SimulationReplay.*, src/Editor/EditorLayer.cpp
read-when: 要确认"同一串操作跑两遍结果是否一致"、排查角色轨迹分叉，或改角色步进/固定 tick
size: ~90 行
updated: 2026-11-16
---

# 输入录制与回放（D2）

C1 保证了"模拟按 1/60 固定步长推进"，但那只解决 tick 数一致。要回答"这一局能不能
复现"，还需要两件事：

1. 一 tick 的角色步进必须是**纯函数**（`scene::stepCharacter`）；
2. 每 tick 吃了什么输入要**录下来**，并且能只凭这份记录重跑出同样的状态
   （`scene::ReplayRecorder`）。

## 1. 纯函数：stepCharacter

`src/Scene/CharacterSimulation.h`：

```cpp
CharacterSimStep stepCharacter(const CollisionWorld& world,
                               const CharacterSimSettings& settings,
                               const CharacterSimState& previous,
                               const glm::vec3& moveDirection,
                               bool jump,
                               float dt,
                               float speedMultiplier = 1.0f);
```

它不读 ImGui、不读时间、不碰 registry，顺序固定：

1. `dt` 非有限或 <= 0 直接返回原状态；
2. `dt = min(dt, maxStepDt)`（默认 0.1s，与旧实现一致）；
3. 水平方向归一化；
4. 跳跃只在接地时生效（`jumped = jump && grounded`）；
5. 接地时竖直速度归零，否则 `-= gravity * dt`；
6. 合成位移交给 `moveCharacter`（D1a 的扫掠盒求解器）；
7. 回写位置/接地状态，算 `horizontalSpeed` 与 `justLanded`。

`Scene::updateCharacterController` 现在只负责"组装输入 → 调它 → 写回组件 → 推进动画"，
不再自己算重力与贴地。

## 2. 录制器：ReplayRecorder

```cpp
struct ReplayTick { uint32_t tick; glm::vec3 moveWorld; bool jump; bool sprint; };

class ReplayRecorder
{
public:
    void clear();
    void record(const InputCmd& cmd);
    std::size_t size() const;
    const std::vector<ReplayTick>& ticks() const;
    uint64_t hash() const;      // FNV-1a，覆盖 tick 号 + 移动方向 + jump/sprint
};
```

编辑器在播放/运行期间，`simulationClock.advance(...)` 的 tick 回调里每 tick 记一条；
`startPlay()` 清空，`stopPlay()` 打印：

```
[Sim] replay | ticks=1284 hash=0x9f3c1a7d5e2b4806
```

同样的操作跑两遍，哈希相同就说明这一局可复现（不同则说明有非确定性输入源）。

## 3. 自检：--replay-test

```
build\Debug\TinyEngine.exe --replay-test
```

两块断言：

| 断言 | 内容 |
|---|---|
| `[Replay] characterSimulation PASS` | 3 段输入（走、跳、冲刺斜走）分别用 60Hz 与 144Hz（帧长在 1/60 与 1/100 之间抖动）驱动，逐 tick 状态哈希零 mismatch，公共 tick ≥100，两侧 tick 数相差 ≤1 |
| `[Replay] replaySelfTest PASS` | 144Hz 抖动帧下录一段，再只凭录下的 `ReplayTick` 以固定 1/60 重放，逐 tick 哈希必须与录制时一致；同一串输入录两遍，录制器哈希也必须一致 |

`Validate.ps1` 的 `m1-d2-replay` 用例同时要求这两行。

## 4. 已知边界

- **只有角色是纯函数**。地形流送、体素异步构建、粒子系统仍按帧推进，它们变化时
  "逐 tick 相等"只在"世界不变"的前提下成立 —— 跨 chunk 的确定性留给后续。
- 录制器只记输入，不记世界状态；回放要求场景从同一个初始快照开始
  （编辑器里就是 `startPlay()` 时的 `playSnapshot`）。
- 蒙皮骨骼矩阵不参与录制：动画是按 `dt` 推进的表现层，不进入状态哈希。
- 哈希是 64 位 FNV-1a，用于发现差异，不做密码学用途。
