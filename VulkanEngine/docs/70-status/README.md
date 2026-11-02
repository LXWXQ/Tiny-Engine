---
id: status
title: 当前状态索引
type: status
scope: 空
read-when: 想知道"项目做到哪了""下一步该做什么"
size: ~45 行
updated: 2026-11-02
---

# 70-status 当前状态

| 文档 | 回答的问题 |
|---|---|
| [milestones](milestones.md) | 已经完成了哪些阶段？实测基线是多少？ |
| [milestones-archive](milestones-archive.md) | D-8～D-24 的历史逐轮记录是什么？ |
| [backlog](backlog.md) | 下一项做什么？完成判据是什么？ |
| [tech-debt](tech-debt.md) | 现在欠着什么？什么时候必须还？ |
| [known-limits](known-limits.md) | 哪些事现在做不到？ |

**这三篇是"事实"，不是"计划"**。计划在 [80-roadmap](../80-roadmap/roadmap.md)。

## 30 秒结论

- 渲染主线完整可用：G-Buffer -> SSAO -> HDR 光照 -> Bloom -> Present（ACES + ImGui）。
- 工程基础扎实：线性渲染图 + 自动屏障 + 子资源语义校验 + 2 帧在飞 + 双队列 +
  bindless 材质 + 完整自动化回归（21 项）。
- 技术债 TD-01 ~ TD-12 已全部关闭。
- 主要缺口是**内容规模能力**：多 chunk GPU 合批、体素、glTF 节点层级；地形 GPU 选层/culling、chunk 流送、异步高度场、单 chunk GPU 顶点/索引组装与 4 槽异步 GPU mesh 上传、组件私有字段、统一销毁、streaming chunk 区域索引、kinematic 角色控制器与运行期区域雕刻、4 个 Set2 材质集/按需外部 16 PNG/程序化回退已落地（多材质合批与雕刻持久化仍待做）。
