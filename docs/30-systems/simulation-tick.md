---
id: systems/simulation-tick
title: 固定步长模拟时钟
type: system
scope: src/Scene/SimulationClock.h, src/Scene/SimulationClock.cpp, src/Scene/RenderInterpolation.h, src/Scene/RenderInterpolation.cpp, src/Editor/EditorLayer.cpp, src/Render/SceneGpuBatch.cpp, src/Render/GeometrySystem.cpp, main.cpp（--tick-test / --render-interp-test）
read-when: 改角色移动/跳跃、把模拟接到新的循环、排查"高刷屏跳得高/走得快"或重放不一致
size: ~190 行
updated: 2026-11-13
---

# 固定步长模拟时钟

## 1. 它解决什么问题

主循环过去把 `glfwGetTime` 的帧间隔直接喂给模拟：`Scene::update(dt)` 里的
角色控制器、`SystemScheduler` 的 Simulation / PostTransform 两相都拿同一个
`dt`。于是"一秒走多远、跳多高"取决于这台机器跑多少帧 —— 同样的按键在
60fps 与 144fps 下走出不同轨迹，也没法重放（D2 的预测/重放要求同一串输入
产生逐位相同的结果）。

`SimulationClock` 把"模拟"和"渲染"拆开：

| 层 | 用什么时间 | 理由 |
|---|---|---|
| 渲染 / UI / 相机 | 可变帧间隔 `frameTime` | 鼠标视角如果量化到 60Hz 会有台阶感 |
| 模拟（角色、系统相） | 固定 `1/60` 秒 | 轨迹只由输入决定，与帧率无关 |
| 异步流送（地形/体素） | 留在帧轮询 | 它们本来就是后台线程 + 主线程轮询，不属于模拟步 |

## 2. API

```cpp
struct InputCmd
{
    glm::vec3 moveWorld{ 0.0f, 0.0f, 0.0f };  // 采样时已按 yaw 转成世界方向
    bool jump = false;                          // 边沿：本帧刚按下
    bool sprint = false;                        // 电平：按住期间每 tick 都为 true
    uint32_t tick = 0;                          // 由时钟填写
};

class SimulationClock
{
    void setFixedStep(float seconds);           // 非法值忽略，保持上一次的合法步长
    void reset();                               // 进/出播放时清空 accumulator 与 tick
    uint32_t advance(float frameDelta, const InputCmd& cmd,
                     const std::function<void(const InputCmd&, float)>& step);
    float alpha() const;                        // accumulator / fixedStep，留给渲染插值
    uint32_t tick() const;
    uint32_t droppedTicks() const;
};
```

`advance` 返回本帧实际执行的 tick 数；回调收到的第二个参数恒等于
`fixedStep()`，`InputCmd::tick` 是递增的 tick 号。

## 3. 三条护栏

| 护栏 | 值 | 不这么做会怎样 |
|---|---|---|
| 单帧帧间隔上限 | `kMaxFrameDelta = 0.25s` | 断点/加载卡 2 秒后一次补 120 个 tick，角色瞬移穿墙 |
| 单帧最多 tick 数 | `kMaxStepsPerFrame = 8` | 追帧比卡顿更糟：越追越卡，形成死亡螺旋 |
| 跳跃锁存 | `jumpLatched_` | 144fps 下有些帧一个 tick 都不跑，`IsKeyPressed` 的边沿会被直接吞掉 |

超出上限的累积量会计入 `droppedTicks()`，不会悄悄丢掉。

## 4. 三个已知风险（C1 方案评审时列出）

1. **输入边沿**：`ImGui::IsKeyPressed(Space)` 是"每帧一次"的状态。如果在
   tick 循环里直接读，一帧跑 2 个 tick 就会触发两次跳。必须先采样成
   `InputCmd::jump`，由时钟锁存后交给第一个 tick。
2. **回归隔离**：`--selftest` 与全部编辑器用例依赖 `Scene::update` 自己跑
   模拟。接入时必须让"外部驱动模拟"默认关闭（`simulationDrivenExternally`
   默认 false），否则一开就全红。
3. **异步地面**：地形/体素流送在 tick 之间改变地面高度，"同一串输入逐位
   相等"只在"地面不变"的前提下成立。跨 chunk 的确定性留到 D2（预测/重放）
   再处理，`--tick-test` 的重放用例只用固定地面。

## 5. 已拍板的决定

1. 固定步长 60Hz（`1/60`）。
2. 单帧上限 `0.25s`、最多 8 tick，多余丢弃并计数。
3. 相机 / UI 不进 tick。
4. Editing 态不进 tick，只有 Playing 与 Runtime 进。
5. Paused 清空 accumulator（恢复时不追帧）。
6. `--tick-test` 的重放一致性用纯函数验证，不建窗口。
7. 渲染插值（`alpha`）单独一批（C1c），本批只提供 `alpha()`。

## 6. 自检

```powershell
.\build\Debug\TinyEngine.exe --tick-test
```

成功输出（稳定标记，Validate 用例 `m1-c1-tick-clock` 依赖它）：

```
[Sim] clockSelfTest PASS | cases=9 fixed=1/60 max-steps=8 jump-latch=ok replay=ok
```

9 组用例覆盖：默认 1/60、一秒 60 tick、抖动帧只由总时间决定、不足一 tick
的帧不推进且 `alpha ∈ (0,1)`、跳跃锁存恰好消费一次、卡顿帧被 8 tick 截断
并记 dropped、步长可配 + reset 语义、回调 tick 号与时钟一致，以及**重放
一致性**：同一段时间线用 60fps 与 144fps 抖动帧各跑一遍，tick 序列逐项
相同（2 秒的 float 帧序列可能差 1 个 tick，用例允许末尾差一个），单次跳跃
边沿在两种帧率下都只被消费一次。

## 7. 接入现状

- **C1a（已完成）**：`SimulationClock` + `--tick-test` + Validate 用例。
- **C1b（已完成）**：
  - `EditorLayer::beginFrame` 每帧设 `Scene::setSimulationDrivenExternally()`
    —— 只有 `Editing` 且非 Runtime 时为 false，其余（Playing / Paused /
    Runtime）由编辑器驱动；
  - `updatePlayState` 与 `updateRuntimeMode` 把输入采样成 `InputCmd`
    （moveWorld 已按相机/runtimeYaw 转成世界方向、jump 是边沿、sprint 是
    电平），交给 `simulationClock.advance()`，在 tick 回调里用固定 `1/60`
    调 `updateCharacterController()` 再调 `Scene::stepSimulation(fixedDt)`；
  - `Scene::stepSimulation()` 只跑 SystemScheduler 两相 + 世界矩阵刷新，
    `Scene::update` 在"被外部驱动"时跳过这三行（相机、流送、可见性仍在
    帧上跑）；
  - `startPlay` / `pausePlay` / `resumePlay` / `stopPlay` 都 `reset()` 时钟。
- **C1c（已完成）**：渲染插值，见下一节。

## 8. 渲染插值（C1c）

固定 60Hz 的模拟配上 144Hz 的显示器时，一帧往往只推进 0 或 1 个 tick，
直接把"当前 tick 的姿态"交给渲染，同一姿态会连画两三帧 —— 看上去就是台阶感。
C1c 用时钟的 `alpha()` 在**上一 tick** 与**当前 tick**的世界矩阵之间插值，
渲染端因此画的是连续姿态，代价是最多晚一个 tick（16.7ms）看到最新状态。

- **历史矩阵从哪来**：`WorldTransformComponent::previousMatrix`。
  `updateNodeRecursive()` 在改写 `matrix` 之前先把它滚进 `previousMatrix`
  —— 只有 dirty 实体走这条分支，静止实体的 `previous == current`，
  插值是恒等变换，不产生额外开销。
- **为什么是 TRS 分解 + slerp**：对两个旋转矩阵做逐元素 lerp，两个正交基的
  线性组合长度小于 1，模型在转身时会肉眼可见地"瘪"一下。分解出
  平移/缩放/旋转后，平移与缩放走 `mix`、旋转走 `slerp`，刚体才不缩水。
  分解需要 `<glm/gtx/matrix_decompose.hpp>`，它是实验性扩展，**必须先
  `#define GLM_ENABLE_EXPERIMENTAL`** 再包含，否则编译直接报 C1189。
- **作用范围**：只替换渲染用的实例矩阵（`SceneGpuBatch::buildInstances`
  的 `InstanceRecord::model/normal` 与 `GeometrySystem` CPU 路径的
  `SimplePushConstantData::modelMatrix/normalMatrix`，节点矩阵照旧相乘）。
  物理、拾取、包围盒与剔除仍然读 `matrix`，即权威姿态 —— 否则剔除会用
  一个"还没到"的位置做判断，边缘模型会闪。
- **怎么开**：`EditorLayer` 在 `updatePlayState` / `updateRuntimeMode` 推进
  完时钟后调用 `Scene::setRenderInterpolationAlpha(simulationClock.tick() > 0
  ? alpha() : 0.0f)`；`pausePlay` / `stopPlay` 归零。`Application` 每帧把
  `Scene::renderInterpolationAlpha()` 搬进 `FrameInfo::renderAlpha`。
  **默认 0 = 不插值**，所以 `--selftest`、编辑器自检与所有无头回归看到的
  画面与加 C1c 之前逐字一致（Sponza bounds 与 434.7MB 纹理预算均未变）。
- **契约自检**：`--render-interp-test` → `[Render] interpolationSelfTest
  PASS | cases=6 endpoints=ok slerp=ok fallback=ok`。6 组用例覆盖
  alpha=0/1 端点、平移中点、90° 旋转的 45° 中点且基向量长度为 1、
  缩放中点、非有限矩阵回退、奇异（零缩放）矩阵回退。
- **已知边界**：蒙皮骨骼矩阵仍按 tick 更新，快速动作时手脚可能比身体
  晚一帧；地形/体素 chunk 的顶点本身不动，不受影响。

## 9. 限制

- 只做主线程推进，没有多线程确定性保证。
- 重放一致性目前只覆盖"输入投递"这一层：角色位移/重力仍写在
  `Scene::updateCharacterController()` 里（依赖地形查询），要等 D2 把它抽成
  纯函数后才能断言"逐位相同的轨迹"。
- 时钟本身不记录输入，录制/回放是 D2 的事。
- 流送中的地面变化不参与 tick 内确定性（见风险 3）。
