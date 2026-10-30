---
id: status/tech-debt
title: 当前技术债
type: status
scope: 空
read-when: 决定下一步做什么之前
size: ~160 行
updated: 2026-10-30
---

# 当前技术债

## 1. 已关闭（TD-01 ~ TD-10）

| ID | 问题 | 修法要点 |
|---|---|---|
| TD-01 | 依赖与验证脚本不可复现 | `SetupDependencies.ps1` 固定 8 个版本 + `Validate.ps1` 一条命令 |
| TD-02 | 跨队列 producer-consumer 缺失 | `crossQueue` CONCURRENT + 单向依赖推导 + `--cross-queue-test` |
| TD-03 | 窗口缩放 / swapchain 重建缺失 | 帧边界原地重建 + RenderGraph 原位 resize + `--resize-test` |
| TD-04 | 材质槽位归还用 `vkDeviceWaitIdle` | graphics timeline 延迟回收 + `UPDATE_AFTER_BIND` + `--material-stress` |
| TD-05 | 语义校验只到整张图 | `SubresourceRange` / `BufferRange` 独立跟踪 + 正反例 |
| TD-06 | Bloom 关闭仍有 7 个全屏 pass | 真跳过 + PresentPass 双布局 + `present_no_bloom.frag` |
| TD-07 | ComputeProbe 固定开销 | 默认关闭，`--compute-test` 显式启用 |
| TD-08 | 遗留代码 / 废弃 shader | 删除 + `Validate.ps1` dead-file guard |
| TD-09 | `EngineConfig` 无版本迁移 | v0->v1->v2 + 备份 + 损坏保护 + `--config-test` |
| TD-10 | RenderGraph 同步语义缺口 | `LOAD` 补读访问位 + `imageAvailable` 等 `ALL_COMMANDS` |

## 2. 未关闭（按"何时必须处理"排序）

| # | 内容 | 影响 | 触发条件 |
|---|---|---|---|
| 1 | 跨队列资源走 `CONCURRENT` sharing，未实现 EXCLUSIVE ownership transfer | 不是最优路径 | 接入 GPU culling / 体素间接缓冲时 |
| 2 | glTF 内部节点层级被烘焙掉；`SpatialIndex` 每帧重建 | 局部动态物体不可动；实体多了 CPU 热点 | 地形/体素主功能前或资产管线升级时 |
| 3 | `releaseMaterialSet()` 没有生产路径调用方，场景反复加载会吃满 256 槽 | 长时间运行/换场景会耗尽槽位 | 做"场景切换"或"材质流送"之前 |
| 4 | bindless `normalMaps` 绑了没采样 | 每材质白占 1/4 描述符 | 启用切线空间法线贴图时 |
| 5 | JobSystem 无 work stealing / 依赖图 / 取消 | 大量短任务负载不均 | 地形/体素接入后按 profile 升级 |
| 6 | 自检开关只覆盖 bloom / ssao | 新 Pass 的可关闭分支是验证盲区 | 加新 Pass 时照 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 14 做 |
| 7 | 长时间运行未验证（回归都在 120 帧内） | 可能存在缓慢泄漏 | 做"长时运行"需求前 |
| 8 | `Pass::writeDepth` 静默改写 `depth==0` 清屏值 | 想清 0 的调用方被坑 | 引入阴影贴图时 |
| 9 | `RenderGraph::setPassEnabled(name, ...)` 实际不可用 | 死接口 | 清理窗口期 |
| 10 | `LightingPass` 兼任"导入交换链" | 职责不对称 | 清理窗口期 |

## 3. 明确"不打算做"的

以下**不是债**，是刻意的取舍（理由见 [decisions](../10-architecture/decisions.md)）：

- 完整 DAG 渲染图（决策 2）
- 渲染图管采样器（决策 4）
- 用日志库替换 `std::cout`（决策 18，spdlog 已就位但未接）
- 持久化"场景/资源"状态（决策 19）
- 多套画质预设

## 4. 处理顺序建议

1. 先看 [roadmap](../80-roadmap/roadmap.md) 的下一阶段，确认哪些债是该阶段的前置；
2. 修一项就补一项回归（[test-matrix](../50-validation/test-matrix.md)）；
3. 修完在本文件 §1 加一行、从 §2 删一行，并更新 `updated`。
