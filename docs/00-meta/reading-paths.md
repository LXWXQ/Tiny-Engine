---
id: meta/reading-paths
title: 任务导向的最小阅读集
type: meta
scope: 空（纯索引）
read-when: 新会话开始，或接到一个具体任务时
size: ~125 行
updated: 2026-11-03
---

# 阅读路径

**核心原则**：不要通读 `docs/`。先按下面的任务表选路径，只读列出的文档。
每篇都先看 front-matter 的 `read-when`，判断不符就跳过。

## 0. 无论做什么都先读

| 顺序 | 文档 | 目的 | 成本 |
|---|---|---|---|
| 1 | [根 README](../../README.md) | 项目定位、构建命令、特性总表 | 小 |
| 2 | [pitfalls](../60-debug-profiling/pitfalls.md) | **已知的 16 个坑**，能直接避免几小时的排查 | 中 |

> 例外：纯文档改动可以只读 [doc-rules](doc-rules.md)。

## 1. 按任务

### 1.1 "我要跑起来 / 换机器"

1. [build](../40-build/build.md)
2. [dependencies](../40-build/dependencies.md)
3. 出问题再看 [troubleshooting](../40-build/troubleshooting.md)

### 1.2 "我要改渲染图 / 加一个 Pass / 调屏障"

1. [rendergraph](../10-architecture/rendergraph.md) ← 主体
2. [resource-lifetime](../10-architecture/resource-lifetime.md) ← 资源归属与帧槽位
3. [pass-order](../20-pipeline/pass-order.md) ← 新 Pass 插在哪
4. [test-matrix](../50-validation/test-matrix.md) ← 要给新 Pass 加回归

### 1.3 "我要改某个 Pass 的着色器 / 画质参数"

1. [pass-order](../20-pipeline/pass-order.md)
2. [gbuffer](../20-pipeline/gbuffer.md) 或 [intermediate](../20-pipeline/intermediate.md)
3. [shaders](../20-pipeline/shaders.md)（改完要确认清单）
4. [acceptance](../50-validation/acceptance.md)（怎么证明画面没坏）

### 1.4 "我要改同步 / 多帧 / 队列"

1. [synchronization](../10-architecture/synchronization.md)
2. [rendergraph](../10-architecture/rendergraph.md) 的"屏障推导"节
3. [decisions](../10-architecture/decisions.md) 决策 2/3

### 1.5 "我要改材质 / 描述符"

1. [descriptors](../10-architecture/descriptors.md)
2. [decisions](../10-architecture/decisions.md) 决策 12/21
3. [pitfalls](../60-debug-profiling/pitfalls.md) 坑 4（哨兵值）

### 1.6 "我要改场景 / ECS / 剔除"

1. [scene-ecs](../30-systems/scene-ecs.md)
2. [known-limits](../70-status/known-limits.md)（glTF 节点层级、空间索引每帧重建）

### 1.6.1 "我要改程序化地形"

1. [terrain](../30-systems/terrain.md) ← 算法、ECS、渲染、编辑器与存读
   + [terrain-materials](../30-systems/terrain-materials.md) ← Set2 与四层 PBR 混合
2. [scene-ecs](../30-systems/scene-ecs.md) ← Mesh/Bounds/材质槽位与剔除
3. [gbuffer](../20-pipeline/gbuffer.md) ← 顶点色从 mrt 写入同一组附件
4. [test-matrix](../50-validation/test-matrix.md) ← `terrain-render` 回归

### 1.7 "我要加一个调试能力 / 剖析工具"

1. [zero-overhead](../60-debug-profiling/zero-overhead.md) ← **先读这个，否则会破坏零开销原则**
2. [diagnostics](../60-debug-profiling/diagnostics.md)
3. [cli-reference](../50-validation/cli-reference.md)
4. [test-matrix](../50-validation/test-matrix.md)

### 1.8 "我要加回归测试"

1. [test-matrix](../50-validation/test-matrix.md)
2. [cli-reference](../50-validation/cli-reference.md)
3. [acceptance](../50-validation/acceptance.md)

### 1.9 "我要开一个新主功能（地形 / 体素 / GPU-driven）"

1. [roadmap](../80-roadmap/roadmap.md) ← 顺序与前置条件
2. [tech-debt](../70-status/tech-debt.md) ← 前置债是否已还
3. [weak-points](../10-architecture/weak-points.md) ← 会撞到什么
4. 再按 1.2 / 1.4 深入

### 1.10 "我怀疑有回归"

1. [acceptance](../50-validation/acceptance.md) ← **先确认测量本身可信**
2. [troubleshooting](../40-build/troubleshooting.md)
3. [tools](../60-debug-profiling/tools.md)

### 1.11 "我要把界面改得更友好 / 别人上手用不了"

1. [ux-audit](../90-editor/ux-audit.md) ← **先按编号认领问题**，不要自己发明需求
2. [ux-rules](../90-editor/ux-rules.md) ← 判据 U1~U10 与对应实现模式
3. [ux-playbook](../90-editor/ux-playbook.md) ← 任务卡、批次顺序、禁区、验收命令
4. [panels](../90-editor/panels.md) ← 要动的面板归属与布局常量

> 不要通读 `EditorLayer.cpp`（近 3000 行）。按 audit 的编号定位到函数即可。
> 也不要"顺手把界面整体美化一遍"：一批只动一个编号，见 playbook 第 5 节。

## 2. 上下文预算参考

| 阅读组合 | 约行数 | 适合 |
|---|---|---|
| 根 README | ~150 | 只需要知道项目是什么 |
| + pitfalls | ~450 | 准备动手改代码 |
| + 一个 architecture 文档 | ~800 | 做该子系统内的改动 |
| + 两个 architecture + 一个 pipeline | ~1400 | 跨子系统改动 |

超过约 1500 行后，建议**先只读 front-matter 和目录**，定位到具体小节再展开。

## 3. 不允许的做法

- 一次性读 `docs/` 下所有 `.md`（约 6000+ 行，会挤掉代码本身的上下文）。
- 只读根 README 就开始改 `RenderGraph`（会漏掉子资源/语义校验的不变量）。
- 读 [decisions](../10-architecture/decisions.md) 后凭记忆改代码而不读对应的实现文档
  —— 决策文档写的是"为什么"，具体接口在实现文档里。
