---
id: validation
title: 验证层索引
type: validation
scope: tools/Validate.ps1, main.cpp 的 CLI
read-when: 加回归项、判断改动是否算"改坏"、写自动化脚本
size: ~50 行
updated: 2026-10-31
---

# 50-validation 验证与回归

| 文档 | 回答的问题 |
|---|---|
| [cli-reference](cli-reference.md) | 全部命令行参数是什么？怎么组合？ |
| [test-matrix](test-matrix.md) | 21 项回归各测什么？判据是什么？ |
| [acceptance](acceptance.md) | 什么算"通过"？测量本身怎么保证可信？ |

## 唯一权威入口

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1
```

**任何"改好了"的结论都必须以它为准**，不能只看单次 `--selftest`。
