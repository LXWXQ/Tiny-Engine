---
id: status/tech-debt
title: 当前技术债
type: status
scope: 空
read-when: 决定下一步做什么之前
size: ~160 行
updated: 2026-11-02
---

# 当前技术债

## 1. 已关闭（TD-01 ~ TD-12）

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
| TD-11 | 地形 GPU mesh 同步 submit + fence wait | 4 槽异步 submit/poll + worker picking mirror；257² 主线程 poll 约 1ms |
| TD-12 | 组件 public 字段可直写 + 生产销毁入口不一致 + 无区域索引 | 字段私有/getter/setPose；XZ region bucket；clearEntities/相机复用走 Scene::destroyEntity |

## 2. 未关闭（按"何时必须处理"排序）

| # | 内容 | 影响 | 触发条件 |
|---|---|---|---|
| 1 | 跨队列资源走 `CONCURRENT` sharing，未实现 EXCLUSIVE ownership transfer | 不是最优路径 | 接入 GPU culling / 体素间接缓冲时 |
| 2 | glTF 内部节点层级被烘焙掉 | 局部动态物体不可动 | 资产管线升级时 |
| 3 | `releaseMaterialSet()` 没有生产路径调用方，场景反复加载会吃满 256 槽 | 长时间运行/换场景会耗尽槽位 | 做"场景切换"或"材质流送"之前 |
| 4 | bindless `normalMaps` 绑了没采样 | 每材质白占 1/4 描述符 | 启用切线空间法线贴图时 |
| 5 | JobSystem 无 work stealing / 依赖图 / 核心取消令牌 | 大量短任务负载不均；地形目前使用 Scene 级 atomic 协作取消 | 地形/体素接入后按 profile 升级 |
| 6 | 自检开关只覆盖 bloom / ssao | 新 Pass 的可关闭分支是验证盲区 | 加新 Pass 时照 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 14 做 |
| 7 | 长时间运行未验证（回归都在 120 帧内） | 可能存在缓慢泄漏 | 做"长时运行"需求前 |
| 8 | `Pass::writeDepth` 静默改写 `depth==0` 清屏值 | 想清 0 的调用方被坑 | 引入阴影贴图时 |
| 9 | `RenderGraph::setPassEnabled(name, ...)` 实际不可用 | 死接口 | 清理窗口期 |
| 10 | `LightingPass` 兼任"导入交换链" | 职责不对称 | 清理窗口期 |
| 11 | 地形 LOD 全部预生成并常驻显存；B-04 只把单 chunk 顶点/索引组装迁到 GPU（流送 chunk 均为 1×1，仍走 GPU mesh；仅单实体 chunkGrid>1 回退 CPU builder） | 大世界显存仍随层级线性增长；GPU mesh 尚未覆盖多 chunk/流送合批 | 大世界/多 chunk GPU 合批时 |
| 12 | 流送 chunk 各持独立 Model；GPU selection 已按 entity 输出 indirect count，B-07 笔刷可局部重建单个 chunk，但选择/撤销仍以 manager 为单位 | chunk 数量增加后 model bind/上传开销上升；chunk 编辑撤销粒度不足 | 多 chunk 编辑/撤销需求时 |
| 13 | GPU mesh 路径为兼容既有 picking API 仍保留全分辨率 CPU positions/indices mirror | 单 chunk 额外 CPU 内存约与 HeightField 同量级；mountain/chunk 多时累计 | 做 GPU/HeightField 统一 picking 时 |
| 14 | 地形雕刻只改运行期 HeightField/Model，不写场景 JSON；CharacterControllerComponent 已序列化但 Play 快照不保存雕刻 | 保存重载/Play 停止会丢失雕刻成果；编辑器无法持久化地形修改 | 后续/地形编辑持久化需求 |
| 15 | 地形材质最多 4 个 Set2 set，但渲染按实体顺序遇到变化即重绑，未按材质集排序/合批 | 交替材质会增加 descriptor switch，材质集切换次数不设上限 | 多材质 chunk profiling 时 |

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

## TD-09 ~~编辑器视口仍是整窗渲染~~ （已完成）

> 已实现，见 [editor/viewport](../90-editor/viewport.md)。以下保留原始分析作为背景。


- **现状**：3D 画面直接铺满交换链，面板浮在它上面。`EditorState` 里已经算好
  "未被遮挡的中央矩形"（`viewportX/Y/Width/Height`），但没有离屏目标。
- **影响**：面板无法真正"嵌入"视口区；截图会带上 UI；不能做多视口。
- **为什么先这样**：离屏视口要求渲染图的最终目标从交换链换成一个纹理，
  再把该纹理作为 `ImGui::Image` 的描述符 —— 属于对渲染图输出契约的改动，
  不是编辑器层能独立完成的。先把手写布局这条路走通，避免一次改两层。
- **解决方向**：`PresentPass` 增加"输出到指定 image"的模式；
  `EngineServices` 增加 `requestViewportTarget(w,h)`。
