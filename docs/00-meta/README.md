---
id: meta
title: 元文档索引
type: meta
scope: docs/00-meta
read-when: 写文档、找代码对应文档、确定本次该读什么
size: ~60 行
updated: 2026-10-30
---

# 00-meta 元文档

这一层描述**文档系统自身**，不描述引擎代码。四篇的分工是刻意分开的：

| 文档 | 回答 | 什么时候用 |
|---|---|---|
| [doc-rules](doc-rules.md) | 怎么组织、怎么写、怎么同步 | 写/改任何文档前 |
| [doc-template](doc-template.md) | 新文档长什么样 | 新建文档时直接复制 |
| [doc-map](doc-map.md) | 某个源文件对应哪几篇文档 | **改代码前必查** |
| [reading-paths](reading-paths.md) | 本次任务最小阅读集 | **新会话第一步** |

## 为什么不合并成一篇

因为它们的使用时机不同且互斥：

- 新会话只需要 `reading-paths`，不需要读规则；
- 改代码时只需要 `doc-map` 的几行；
- 只有真的在动文档时，才需要 `doc-rules` 的全部内容。

合并成一篇会让前两种场景白读 200 行，正好违背这套系统"可裁剪"的目标。

## 与其它层的关系

`00-meta` 是**唯一允许引用所有其它层**的目录；其它层之间只允许按下图方向引用：

```
00-meta  ->  10-architecture  ->  20-pipeline
                     |                  |
                     +-->  30-systems   |
                     |                  |
                     v                  v
               50-validation  <--  60-debug-profiling
                     |
                     v
               70-status  ->  80-roadmap
```

反向引用只允许"指向更具体的文档"（例如 `60-debug-profiling/pitfalls.md` 可以引
`10-architecture/rendergraph.md` 的某个小节），不允许把内容和规则复制过来。
**同一事实只能有一个权威出处。**
