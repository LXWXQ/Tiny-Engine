---
id: meta/doc-rules
title: 文档管理规则
type: meta
scope: 空（纯规则）
read-when: 新增 / 删除 / 重命名 / 拆分文档，或改变某篇文档的职责范围
size: ~200 行
updated: 2026-10-30
---

# 文档管理规则

## 1. 根本目标

文档服务的是**新会话的 LLM**：它要在有限上下文里，用最少的阅读量，准确知道
"项目是什么、现在什么状态、我要改的地方有什么约束"。

因此所有规则的判据都是同一个：**这条规则是否降低了"读错/漏读/白读"的概率**。
不服务于这个目标的规则不该存在。

## 2. 目录与命名

### 2.1 根目录

**只允许一个文档：`README.md`。** 任何其它 `.md` 出现在仓库根目录都算违规，
`DocCheck.ps1` 会失败。

### 2.2 分类目录

`docs/` 下用数字前缀控制阅读顺序：`00-meta` / `10-architecture` / `20-pipeline` /
`30-systems` / `40-build` / `50-validation` / `60-debug-profiling` / `70-status` / `80-roadmap`。

新增分类需要同时改：`docs/README.md` §1 类型表、§2 目录树、根 `README.md` §4 索引表。

### 2.3 文件名

- 全小写、连字符分隔、`.md` 结尾：`resource-lifetime.md`，不要 `ResourceLifetime.md`。
- 名字描述**读者的问题**，不是代码类名：写 `pass-order.md` 而不是 `IRenderPass.md`。
- 每个目录必须有 `README.md` 作为索引。

## 3. front-matter（强制）

每个 `.md` 开头必须是：

```markdown
---
id: <分类>/<文件名不含扩展名>     # 根与 docs/README.md 用 meta / docs 之类短 id
title: <中文短标题>
type: meta|architecture|pipeline|system|build|validation|debug|status|roadmap
scope: <覆盖的代码路径，多个用逗号；纯规则/纯说明写 "空（...）">
read-when: <触发条件，写"什么时候必须读"，不写"内容是什么">
size: ~N 行
updated: YYYY-MM-DD
---
```

校验规则（`DocCheck.ps1` 实施）：

- 七个字段缺一不可，顺序不限但建议如上。
- `id` 必须与路径匹配（`docs/10-architecture/rendergraph.md` -> `architecture/rendergraph`）。
- `type` 必须在枚举内。
- `read-when` 不能为空、不能写成"介绍XX"这类描述内容的句子。
- `updated` 必须是 `YYYY-MM-DD`。

## 4. 体量控制

| 体量 | 处理 |
|---|---|
| < 60 行 | 太短：要么并入相邻文档，要么确实只是个索引（索引可以短） |
| 60~450 行 | 正常 |
| 450~600 行 | 警告：考虑拆分 |
| > 600 行 | 违规：必须拆成子文档，并在父索引登记 |

拆分时**不建子目录**，直接在分类目录里加文件即可（例如 `rendergraph.md` 拆成
`rendergraph.md` + `rendergraph-subresource.md`）。只有某分类超过 8 篇时才建二级目录。

## 5. 变更同步契约（最重要）

### 5.1 改文档时

| 你做的事 | 必须同时更新 |
|---|---|
| 新增文档 | ① 所在分类的 `README.md` 索引 ② `docs/README.md` §2 树 + §5 完整索引 ③ 根 `README.md` §4 |
| 删除文档 | 同上三处（移除条目），并全局搜索删除指向它的链接 |
| 重命名 / 移动 | 同上三处 + 所有引用它的链接 + 它的 `id` |
| 改职责范围 | 更新它自己的 `scope` / `read-when` + 父索引里的描述 + [doc-map](doc-map.md) |
| 仅改正文内容 | 只需更新 `updated` |

### 5.2 改代码时

1. 在 [doc-map](doc-map.md) 里查受影响的源文件 -> 得到文档清单；
2. 改代码；
3. 按清单逐篇回写（**改行为就必须改文档**，改注释/重构不必）；
4. 更新该篇的 `updated`；
5. 跑 `tools/DocCheck.ps1`；
6. 跑 `tools/Validate.ps1`（文档检查是其中一项）。

### 5.3 "同一事实只有一个出处"

如果 A 文档需要描述 B 文档已经描述过的内容，**写链接 + 一句摘要**，不要复制正文。
复制是漂移的源头：两边改一边就会不一致，而不一致比缺失更有害。

例外：`70-status/milestones.md` 的实测基线数字可以出现在根 `README.md` §6，
但必须带一句"成对更新"的提示（已写在根 README 里）。

## 6. 写作约定

1. **语言**：中文正文；代码标识符、命令、路径、日志标记保持原文。
2. **解释"为什么"而不是"做了什么"**：代码已经说明做了什么。
3. **给数字**：`desc binds 104 -> 1`、`stderr 0 字节`、`455 FPS -> 531 FPS`。
   没有数字的结论在评审时无法验证。
4. **不写"将来可能"**：未做的进 [roadmap](../80-roadmap/roadmap.md)，
   做了一半的进 [tech-debt](../70-status/tech-debt.md)。
5. **命令必须可复制执行**：用 ```powershell 代码块，路径写全（含仓库内相对路径）。
6. **踩过的坑要写现象 + 判据 + 修法**，只写结论等于没写（见 [pitfalls](../60-debug-profiling/pitfalls.md)）。
7. **不用表情符号**；表格用于对比，列表用于步骤。
8. **相对链接**，不用绝对路径（换机器会失效）。

## 7. 自动校验

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/DocCheck.ps1
```

检查项：

| 检查 | 失败示例 |
|---|---|
| 根目录只允许 `README.md` | 根目录出现 `NOTES.md` |
| 每个 `.md` 有合法 front-matter | 缺 `read-when` |
| `id` 与路径一致 | `docs/10-architecture/x.md` 写 `id: arch/x` |
| 父索引覆盖所有子文档 | 新增 `docs/30-systems/audio.md` 但没在 `30-systems/README.md` 登记 |
| `docs/README.md` §2 树覆盖所有分类 | 新增分类没登记 |
| 相对链接目标存在 | 链接到已删除的 `HANDOFF.md` |
| 单文档不超过 600 行 | 某篇涨到 900 行未拆 |

该脚本已被 `tools/Validate.ps1` 调用，因此**文档不合规会让整条回归失败**。
这是刻意的：文档漂移的成本会在下一次新会话里以"读错代码"的形式付出，比一次构建失败贵得多。
