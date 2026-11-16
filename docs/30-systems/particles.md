---
id: systems/particles
title: 粒子系统与模拟成本
type: system
scope: src/Scene/FeatureComponents.h, src/Scene/FeatureComponents.cpp, src/Render/ParticlePass.*
read-when: 要改粒子模拟、调粒子上限，或评估"要不要改成 SoA / GPU 模拟"
size: ~110 行
updated: 2026-11-16
---

# 粒子系统与模拟成本

## 1. 数据与上限

- `scene::Particle`（`src/Scene/FeatureComponents.h:111`）是 16 个属性里的 AoS 结构：
  `position` / `velocity` / `age` / `lifetime` / `sizeScale`，`std::vector<Particle>` 存在
  `ParticleEmitterComponent::particles` 里。
- 全局上限 `kMaxParticlesTotal = 16384`（`FeatureComponents.h:122`），模拟侧先截断，
  保证不超过 `ParticlePass` 的实例缓冲容量。
- 单发射器默认 `maxParticles = 512`，属性上限 4096；超出全局上限后新发射器只能等别人淘汰。

## 2. 模拟路径

`particleSystem(entt::registry&, float)`（`src/Scene/FeatureComponents.cpp`，注册在
`SystemPhase::PostTransform`，所以世界变换已经更新过）：

1. `dt` 先夹到 `[0, 0.1]`，卡顿/断点恢复时不会让粒子一步飞出画面；
2. 先推进、再发射（避免新粒子被本帧淘汰）：`age += dt`，超寿命用"尾部换位 + pop_back"
   做 O(1) 淘汰；存活粒子 `velocity += gravity * dt`、`position += velocity * dt`；
3. `enabled == false` 只推进不发射；
4. 发射：`spawnAccumulator += emissionRate * dt`（上限 256，防止长时间卡住后爆发），
   再按 `min(请求数, 单发射器余量, 全局余量)` 生成，方向用锥内均匀采样、半径用 `cbrt`
   保证体积均匀。

渲染侧是独立的 `ParticlePass`（Water 之后、TAA 之前，前向混合 HDR、按相机距离排序），
每帧把存活粒子写成 host-visible 实例。

## 3. 实测成本（`--particle-bench`）

```powershell
.\build\Release\TinyEngine.exe --particle-bench
```

在本机（16 线程，Release，两次运行的区间）测满 `kMaxParticlesTotal`：

| 项目 | 结果 | 换算 |
|---|---|---|
| 积分（16384 粒子，600 帧均值） | **0.024–0.025 ms/帧** | 1.5–1.6 ns/粒子 |
| 1 ms 预算可容纳 | 约 65–68 万粒子 | — |
| 发射（256 个/帧，300 帧均值） | 0.011–0.012 ms/帧 | 0.044–0.049 µs/粒子 |

Debug 构建同一测量是 0.76 ms/帧（46 ns/粒子），比 Release 慢约 30 倍 —— 再次印证
tech-debt #17：性能结论只认 Release。

## 4. 结论：暂不改 SoA（C3 决策）

迁移计划里 C3 的验收是"4 万粒子更新 <1ms"。实测在**当前 16384 上限**下模拟只花
0.025 ms（一个 60Hz tick 预算的 1.5%），按同一 per-particle 成本线性外推，4 万粒子
约 0.06 ms —— 验收条件早已满足，**改造没有收益**。

因此 C3 不做结构改造，改为保留 `--particle-bench` 作为回归尺子。触发重新评估的条件：

- 需要单场景 **> 10 万** 粒子（按当前成本约 0.15 ms，仍在预算内，但实例上传/排序会成为新瓶颈）；
- 或模拟单帧超过 **0.1 ms**（约 6% tick 预算）；
- 或要加逐粒子碰撞、生命周期曲线、软粒子这类每粒子分支更多的特性 —— 那时 SoA + 属性分离
  才有明确收益。

真正该先量的是**渲染侧**：`ParticlePass` 每帧的实例写入 + 排序 + 上传，以及
`kMaxParticlesTotal` 提到 10 万后实例缓冲与带宽的开销。本次基准只覆盖模拟。

## 5. 相关限制

见 [known-limits](../70-status/known-limits.md)：无碰撞、子发射器、生命周期曲线、GPU 模拟、
软粒子、阴影，不进 G-Buffer/平面反射/CSM，TAA 没有粒子运动向量；sprite 在启动时预加载
（最多 64 张），不支持运行时热重载。
