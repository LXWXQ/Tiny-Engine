---
id: status
title: 当前状态索引
type: status
scope: 空
read-when: 想知道"项目做到哪了""下一步该做什么"
size: ~45 行
updated: 2026-10-30
---

# 70-status 当前状态

| 文档 | 回答的问题 |
|---|---|
| [milestones](milestones.md) | 已经完成了哪些阶段？实测基线是多少？ |
| [tech-debt](tech-debt.md) | 现在欠着什么？什么时候必须还？ |
| [known-limits](known-limits.md) | 哪些事现在做不到？ |

**这三篇是"事实"，不是"计划"**。计划在 [80-roadmap](../80-roadmap/roadmap.md)。

## 30 秒结论

- 渲染主线完整可用：G-Buffer -> SSAO -> HDR 光照 -> Bloom -> Present（ACES + ImGui）。
- 工程基础扎实：线性渲染图 + 自动屏障 + 子资源语义校验 + 2 帧在飞 + 双队列 +
  bindless 材质 + 完整自动化回归（20 项）。
- 技术债 TD-01 ~ TD-10 已全部关闭。
- 主要缺口是**内容规模能力**：GPU-driven、地形、体素、glTF 节点层级。
