---
id: editor/ux-playbook
title: UX 改造的 AI 执行协议
type: meta
scope: 空（纯流程）
read-when: 让 AI 做界面友好性改造之前，作为任务描述的第一份输入
size: ~270 行
updated: 2026-11-03
---

# UX 改造的 AI 执行协议

## 1. 这篇解决什么

LLM 改 UI 的失败方式和改渲染代码完全不同。渲染改错会被 50 项回归拦住，
UI 改错**什么都不会发生**：编译过、回归过、stderr 干净，但界面变成了一堆
没人看得懂的提示。原因是回归只验证"没崩"，不验证"人能不能用"。

所以这份协议只做三件事：

1. 把"优化界面"拆成**一批一件、可回滚**的任务；
2. 给每批一个**机器能判定的完成条件**；
3. 列清楚**改了会破坏既有基线**的动作，提前禁掉。

配套文档：判据在 [ux-rules](ux-rules.md)，现状在 [ux-audit](ux-audit.md)。
本篇不重复它们的内容，只规定"怎么做"。

## 2. 开工前的固定输入

每次任务开始，按顺序读三篇，读完再动手：

| 顺序 | 文档 | 读完要能回答 |
|---|---|---|
| 1 | [ux-audit](ux-audit.md) | 这次要修的编号是什么？现在是否仍然 open？ |
| 2 | [ux-rules](ux-rules.md) | 这个编号对应哪条判据？通过条件是什么？ |
| 3 | [panels](panels.md) | 要改的面板归谁管？布局常量在哪？ |

不要读整个 `src/Editor/`。`EditorLayer.cpp` 已接近 3000 行，全读会挤掉真正需要的上下文。

## 3. 任务卡模板

复制下面这段，把方括号填掉，作为一次任务的完整描述。**一批只填一张卡。**

```text
目标：修复 [UX-01]，让 [冷启动空场景] 的用户知道下一步做什么。
判据：U5（见 docs/90-editor/ux-rules.md#u5-空状态可继续）。
范围：只改 [ViewportPanel 与 HierarchyPanel 的空状态分支]。
不许碰：渲染路径、shader、RHI、命令栈语义、任何自检断言。
验收：
  1) TinyEditor.exe --editor 冷启动，视口与 Hierarchy 各出现一句说明和一个按钮；
  2) 点按钮等价于菜单 Create > Terrain（同一函数）；
  3) 场景里已有实体时，空状态不再出现；
  4) stderr 为 0 字节（selftest 路径不受影响）。
交付：改动文件清单 + 每处改动的 before/after 片段 + 复现步骤。
```

四个空都要填。填不出来，说明这件事还不该开工。

### 3.1 批次启动口令

整批任务（而不是单条）可以直接用这段作为开场，后面再接具体编号：

```text
按 docs/90-editor/ux-playbook.md 执行 B2 批次，本轮只做 UX-01。

开始前依次读：
  docs/90-editor/ux-audit.md 第 3 节的 UX-01
  docs/90-editor/ux-rules.md 的 U5（空状态可继续）
  docs/90-editor/panels.md 的布局与面板归属

约束：
  不改渲染、shader、RHI、命令栈语义、自检断言
  不往 stderr 打任何提示
  不顺手改其他面板

结束时给我：改动文件与行号、before/after 截图、反例验证（场景非空时空状态消失）、
Validate.ps1 的退出码与 validation 计数行。
```

口令里必须带"本轮只做哪一条"。不带编号的"优化一下界面"会得到十个面板一起改，
既无法评审也无法回滚。

## 4. 批次顺序

顺序不是按"哪个好改"，是按**陌生人的失败顺序**：他先遇到什么，就先修什么。

| 批次 | 主题 | 覆盖条目 | 完成后陌生人能做什么 |
|---|---|---|---|
| B1 | 进得来、看得见 | UX-02、UX-04、UX-05、UX-19 | 双击不闪退，缺环境时知道缺什么，小屏上窗口放得下 |
| B2 | 知道下一步 | UX-01、UX-09、UX-16 | 空画面里有可点的起点 |
| B3 | 做了有回应 | UX-03、UX-10、UX-12、UX-17 | 成功失败都看得见，失败有出路 |
| B4 | 找得到 | UX-06、UX-13、UX-14 | 不背快捷键也能用全功能 |
| B5 | 像一个产品 | UX-07、UX-11、UX-15、UX-18 | 术语统一，排查路径清楚 |

**不要跳批。** B3 依赖 B2 的空状态位置（提示要显示在用户正在看的地方），
B5 依赖 B4 的菜单结构（术语统一要在入口确定之后做）。

## 5. 单批执行流程

1. **复现**。先按 [ux-audit](ux-audit.md) 第 7 节的命令确认问题仍存在。
   问题已消失就停手，把状态改 `fixed`，不要"顺手再优化一下"。
2. **定位**。找到唯一的修改点。如果一个需求要在三个文件里各加一段相似代码，
   先抽一个共用函数，再调用三次。
3. **最小改动**。只改这一条判据涉及的代码。看到旁边有别的 UX 问题，
   记进 audit，不要在这批里改。
4. **自测**。先跑 `TinyEditor.exe --editor` 人工走一遍，再跑回归。
5. **取证**。截图 + 关键日志 + 改动行号。没有证据的"已完成"不算完成。
6. **回写**。改的是行为，就要回写文档：audit 状态、`updated`、必要时 panels。
   跑 `DocCheck.ps1`。

## 6. 每批的完成条件

完成条件必须是**可执行命令的输出**，不能是"界面看起来更好了"。

### B1

这两条已经进回归（`tools/Validate.ps1` 的 `b1-help` / `b1-unknown-arg`），判据写在用例里：

| 用例 | 命令 | 期望 |
|---|---|---|
| `b1-help` | `TinyEngine.exe --help` | 退出码 0；stdout 有 `--selftest [N]` 等分组表；不创建窗口/设备 |
| `b1-unknown-arg` | `TinyEngine.exe --not-a-flag` | 退出码 2；stderr 有 `unknown argument: --not-a-flag`；stdout 保持干净 |

错误路径用例靠三个开关给基线开口子：`ExpectedExitCode`、`AllowStderr`、`SkipValidationMarker`。
默认仍然是「退出码 0 + stderr 0 字节」，其余用例没有放宽。

实现注意：`--selftest` 的帧数**不是**用 `++i` 消费的（它是唯一一个），所以「未知参数」检测不能
简单补一个 `else` —— 否则 `--selftest 60` 里的 `60` 会被判成未知参数，整套回归立刻全红。
现在的做法是一个一次性标志（`selfTestFramesPending`）先放行它。

再加两条人工项：

- 在缺 `VK_EXT_device_fault` 的机器上双击启动，必须出现可读的失败说明，
  且同目录生成 `startup-error.log`。
- 把桌面分辨率设为 1366x768，启动后菜单栏与状态栏必须完整在屏幕内；
  再把缩放改为 150%，文字不得小于原来的可视高度。

UX-19 的窗口尺寸与 DPI 部分**单独一个提交**：它会改变 `--screenshot` 的像素尺寸，
必须与截图基线一起更新。和语言、提示类改动混在一起，回滚时会带坏另一边。

### B2

人工项，三条全过：

- 冷启动后 3 秒内，视口内有字，且字里有一个可点按钮；
- 点该按钮后场景出现实体，空状态消失；
- 删掉 `Resources/Models` 再启动，Assets 面板说明目录缺失，并给出期望路径。

### B3

人工项 + 一条可复现的破坏实验：

```powershell
# 把场景目录设为只读后保存，验证错误可见
attrib +R Resources\Scenes
```

要求：1 秒内状态栏出现错误摘要；Toast 出现且可点击；Console 里对应行可定位。

### B4

```powershell
# 快捷键表是唯一数据源：条数与冲突都在自检里
./build/Debug/TinyEditor.exe --selftest 5 --empty-scene | Select-String "shortcut sheet selfTest"
```

已实现（B4）：`kShortcutEntries` 同时驱动 `handleShortcuts()` 与速查表，自检打印 `entries=20 conflicts=0`；Edit 菜单补齐 Duplicate/Delete/Focus/Select All（UX-06）；所有面板（含视口）改用 `FirstUseEver`，可拖动并用 `View -> Reset Layout` 复位（UX-14）。

已实现（B6）：Console 关键字搜索（`matchesSearch`，`Ctrl+F`）、地形创建两阶段提示
（`advanceTerrainCreateRequest`，先画忙碌帧再 build）、字体与布局 DPI 缩放（`computeUiScale`）。
回归命令：`tools/Validate.ps1 -Id b6-console-search`；已知坑：截图验证忙碌帧时不要在 `drawToolbar` 里重复置
`terrainCreatePending`（会在同一帧里连续建地形），临时钩子用完必须 `grep -c "TEMP-SHOT"` 归零。

人工项：不按 `?`，仅用菜单完成"复制一个对象并聚焦到它"。

### B5（已实现，2026-11-03）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\Validate.ps1 -Id b5-language-console
```

- 判据：`[Editor] status bar selfTest PASS`（`formatCount()` 的 k/M 边界 + `currentLogClock()` 形状）。
- 人工项：截图三张（工具栏、状态栏、Console），同一概念不出现第二种叫法；
  用 [ux-rules](ux-rules.md) U7 的语言分层表逐项对照（命令中文 / 区域名英文 / 行业参数英文 / 日志中文）。
- 已知坑：状态栏统计必须读 `Scene::getDisplayTelemetry()`，读 `getTelemetry()` 会永远是 0。

## 7. 禁区

这些动作会破坏既有基线，或者把 UI 改坏。违反任意一条，这批就算失败。

| 禁止 | 原因 |
|---|---|
| 往 `stderr` 打任何 UX 提示 | 验收基线是 stderr 0 字节，一行提示就能让整轮回归失败 |
| 在 headless / `--hidden` 路径执行 UI 代码 | 自检不带窗口，UI 代码会读空指针或拖慢回归 |
| 用 Modal 显示普通成功/失败 | Modal 抢焦点、吞输入，只留给"必须二选一" |
| 一次改两个以上面板 | 出问题无法定位是哪一处，回滚也会带走好的改动 |
| 引入新的 ImGui 扩展或第三方 UI 库 | 依赖是 8 个固定版本，加一个就要改构建矩阵 |
| 自创配色、圆角、动画 | 主题只有一套，自创样式会和现有面板割裂 |
| 改动 `Command` 语义来"顺便"支持新操作 | 撤销栈是编辑器最强的不变量，改它要单独立项 |
| 删除或放宽自检断言 | 断言是唯一能挡住"改好了但改坏了"的东西 |
| 用 `ImGui::Text(userString)` 直接打印可变字符串 | 格式串漏洞；必须 `TextUnformatted` 或 `Text("%s", s)` |
| 硬编码绝对路径 | 换机器即失效；用 `TINY_RESOURCE_DIR` 等编译期常量 |
| 在未知参数/帮助路径上创建窗口或设备 | 参数错误必须立刻退（退出码 2），否则用户看到的是"程序启动了但参数没生效" |

## 8. 证据要求

一批交付时必须包含下面四项，缺一项就退回：

1. **改动清单**：文件 + 函数 + 行号，一句话说明改了什么。
2. **before / after**：同一操作、同一窗口尺寸的截图各一张。
3. **反例验证**：证明"不该出现的时候不出现"。例如空状态在场景非空时必须消失。
4. **回归结果**：`Validate.ps1` 的退出码与 `[Validation] errors=0 warnings=0` 那一行。

截图请拍**默认布局**，不要为了好看关掉面板。默认布局才是陌生人看到的布局。

## 9. 反模式

以下写法在本项目里出现过或极易出现，逐条对应真实风险。

### 9.1 把提示写成"系统状态播报"

```cpp
// 差：说的是实现，不是用户能做的事
ImGui::TextDisabled("Multi-entity editing is not supported yet;");
ImGui::TextDisabled("use the Hierarchy panel to pick one entity.");

// 好：先说现状，再给出路，出路是可点的
ImGui::Text("%d entities selected", count);
if (ImGui::Button("Duplicate")) duplicateSelection(state);
ImGui::SameLine();
if (ImGui::Button("Delete")) deleteSelection(state);
ImGui::TextDisabled("改属性请在 Hierarchy 里点其中一个");
```

判断标准：这句话是在描述代码能力，还是在描述用户下一步？后者才算合格。

### 9.2 为了"反馈"而新增一条日志

日志不是反馈。日志只有打开 Console 的人能看到，而陌生人不知道 Console 在哪。
反馈必须出现在**用户当前正在看的位置**（通常是视口或状态栏）。

### 9.3 用禁用代替解释

把按钮变灰是对的，但只变灰不解释，用户只会反复点。灰按钮旁边要有一句
为什么现在不能点（`SetTooltip` 或紧邻的 `TextDisabled`）。

### 9.4 把空状态做成一次性弹窗

空状态要一直存在到条件不成立为止。做成"首次启动弹一次"，第二次启动的人
就再也找不到入口了。

### 9.5 修改判据去迁就实现

改不动就改文档，是这类任务里最隐蔽的失败。判据只能因为"判据本身错了"而改，
不能因为"这次来不及"而改。来不及就把条目留在 `open`，写清卡在哪。

## 10. 提交信息

```text
ux(B2): 冷启动空场景给出可点击起点

- ViewportPanel: 空场景时显示说明与"创建地形"按钮（复用 createTerrainFromUi）
- HierarchyPanel: 空树时显示同一入口，避免两处入口行为不一致
- 修复 UX-01，判据 U5
- 验证: TinyEditor.exe --editor 冷启动 / 删除 Resources\Models 后重试 /
        Validate.ps1 退出码 0，stderr 0 字节
```

`ux(<批次>)` 前缀让这类提交可以和功能提交分开检索。批次号必须与第 4 节一致。

## 11. 改不动的时候

三种情况，处理方式不同：

| 情况 | 处理 |
|---|---|
| 判据要求与现有架构冲突 | 停下，把冲突写进 [tech-debt](../70-status/tech-debt.md)，不要强行绕 |
| 需要改渲染或 RHI 才能实现 | 拆成两个任务：先改能力，再做 UI；不要混在一个提交里 |
| 只能部分满足 | 交付满足的部分，审计条目保持 `open`，在条目下写"已完成 X，缺 Y" |

不要交付"看起来完成"的半成品。半成品比没做更贵：下一个人会以为这里已经处理过了。

## 12. 相关

- 判据：[ux-rules](ux-rules.md)
- 现状：[ux-audit](ux-audit.md)
- 面板与布局：[panels](panels.md)
- 文档规则：[doc-rules](../00-meta/doc-rules.md)
- 验收标准：[acceptance](../50-validation/acceptance.md)
- 回归矩阵：[test-matrix](../50-validation/test-matrix.md)
