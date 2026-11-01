---
id: build
title: 构建层索引
type: build
scope: CMakeLists.txt, cmake/, tools/SetupDependencies.ps1, 3rdParty/
read-when: 每次构建；或构建失败、换机器、加依赖
size: ~45 行
updated: 2026-10-31
---

# 40-build 构建与环境

| 文档 | 回答的问题 |
|---|---|
| [dependencies](dependencies.md) | 有哪些第三方依赖？固定哪个版本？目录什么结构？ |
| [build](build.md) | 怎么构建？有哪几条隐形规则？ |
| [troubleshooting](troubleshooting.md) | 编不过 / 跑不起来怎么查？ |

## 一句话构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Validate.ps1
```

它做四件事：准备依赖 -> 全新 configure/build -> 跑 21 项回归 -> 检查文档一致性。
