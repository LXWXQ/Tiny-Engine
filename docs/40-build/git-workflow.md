---
id: build/git-workflow
title: 提交与推送流程（git bash）
type: build
scope: 空（仓库根与远程配置，不对应 src/ 下的代码）
read-when: 用户说"提交代码"/"推送到远程"，或需要确认仓库根、远程地址与推送失败的处理方式
size: ~70 行
updated: 2026-11-02
---

# 提交与推送流程（git bash）

## 1. 仓库根就是项目根

| 项 | 值 |
|---|---|
| 仓库根 | `C:\Users\Admin\Desktop\Vs code Demo` |
| 远程 | `origin` = `https://github.com/LXWXQ/Tiny-Engine.git` |
| 分支 | `main`（upstream `origin/main`） |
| 提交身份 | `LXWXQ <2920928423@qq.com>`（仓库本地配置） |

2026-11-02 起目录已平铺：原 `VulkanEngine/**` 整体上提到仓库根，`01Triangle/` 已删除。
不再有嵌套 `.git`，也不再有 `VulkanEngine/` 前缀 —— 所有 git / 构建 / 工具命令都直接在仓库根执行。
要取平铺前的旧路径文件，用 `git show <commit>:VulkanEngine/<path>`。

## 2. 标准流程

```bash
cd "C:/Users/Admin/Desktop/Vs code Demo"
git status --short            # 复核：只应包含本次要提交的文件
git add -A
git commit -m "docs/code: <一句话说明>"
git push origin main
git status -sb                # 期望 ## main...origin/main，且没有 ahead
```

## 3. 凭据与网络

- 走 HTTPS + Windows 凭据管理器（URL 形如 `https://LXWXQ@github.com/...`）。
  SSH 在本机**未授权**：`ssh -T git@github.com` 返回
  `git@github.com: Permission denied (publickey)`，不要改用 SSH。
- 国内到 GitHub 不稳定，`push` 可能超时、断流或返回 `fatal: unable to access ...`。
- 自动化执行前先 `export GIT_TERMINAL_PROMPT=0`，否则命令会卡在交互式
  用户名/密码提示上。

## 4. 推送失败时怎么做

1. 原样报告 git 的 `fatal:` 行与退出码，不要只说"失败了"。
2. **不自动重试**、不改写历史（禁止 `--force` / `--force-with-lease`）、不换远程。
3. 本地 commit 保留（不要 `reset`），等用户决定什么时候重推。

## 5. 提交前检查

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/DocCheck.ps1
```

只改文档时跑这一条就够；改了代码再跑 `tools/Validate.ps1 -Incremental -SkipSetup`。

## 6. `3rdParty/` 不入库

依赖由 `tools/SetupDependencies.ps1` 按钉死的版本下载到 `3rdParty/`（见 `.gitignore`）。新 clone 第一次构建前先跑一次：

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/SetupDependencies.ps1
```
