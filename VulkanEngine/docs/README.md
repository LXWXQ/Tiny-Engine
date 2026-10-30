---
id: docs
title: 文档系统总纲
type: meta
scope: docs/
read-when: 第一次接触本项目、或要新增/移动/删除文档
size: ~200 行
updated: 2026-10-30
---
# 文档系统总纲

> 这是 `docs/` 的根文档。它定义**这套文档怎么组织、怎么写、怎么维护**，并给出完整索引。
> 项目定位与启动方式见仓库根 [`../README.md`](../README.md)。

## 0. 为什么要有这一层

本项目由 LLM 驱动开发：每次新对话的上下文是**有限预算**，而代码规模（约 100 个源文件、
51KB 的 `RenderGraph.cpp`、24 个 shader）远超一次能全读的量。

所以文档系统的首要目标不是"写得多"，而是：

1. **可裁剪**：新会话只读"根 README + 本文 + 1~3 个相关子文档"就能开工，其余按需下钻。
2. **可定位**：从"我要改 X"能在两步内找到该读哪篇，而不是全文搜索。
3. **不可漂移**：代码改了文档没改，要被机器检查出来（`tools/DocCheck.ps1`）。

推论：**每个文档必须自带 front-matter、必须被父索引登记、必须控制体量**。
一篇 3000 行的巨型文档等于没有文档——它读不进上下文，也没法定位。

---

## 1. 文档类型

| 类型 | 目录 | 回答什么问题 | 典型体量 |
|---|---|---|---|
| **元文档** | `00-meta/` | 文档怎么写、代码在哪、这次任务该读哪几篇 | 小 |
| **架构** | `10-architecture/` | 为什么这样设计、不变量是什么、改动会碰坏什么 | 中 |
| **管线** | `20-pipeline/` | 数据怎么流过 GPU、每个 Pass 的输入输出 | 中 |
| **系统** | `30-systems/` | 某个独立子系统的接口与实现要点 | 中 |
| **构建** | `40-build/` | 怎么装、怎么编、编不过怎么办 | 小 |
| **验证** | `50-validation/` | 怎么证明没坏、验收标准是什么 | 中 |
| **调试** | `60-debug-profiling/` | 出问题怎么查、有哪些坑 | 中 |
| **状态** | `70-status/` | 现在做到哪、欠了什么 | 小 |
| **规划** | `80-roadmap/` | 下一步做什么、为什么是这个顺序 | 小 |

**分类原则**：按"读者的问题"分，不按"代码目录"分。`RenderGraph` 的实现细节在
`10-architecture/rendergraph.md`，但它在回归里的验收标准在 `50-validation/`——因为读它们的人
问的是不同问题。

---

## 2. 层次规则（像文件系统一样）

```
README.md                        根：是什么 / 怎么跑 / 特性 / 索引 / 管理规则摘要
└── docs/
    ├── README.md                本文件：类型、层次、规则、完整索引
    ├── 00-meta/
    │   ├── README.md            本分类索引
    │   ├── doc-rules.md         文档管理规则（详细版）
    │   ├── doc-template.md      新文档模板
    │   ├── doc-map.md           代码 <-> 文档 反查表
    │   └── reading-paths.md     按任务给出的最小阅读集
    ├── 10-architecture/
    │   ├── README.md
    │   ├── layering.md          分层与依赖方向
    │   ├── rendergraph.md       渲染图：屏障推导 / 子资源 / 语义校验
    │   ├── resource-lifetime.md 谁创建谁销毁 / 帧槽位 / 销毁顺序
    │   ├── synchronization.md   多帧在飞 / 双队列 / timeline / present
    │   ├── descriptors.md       Set0 全局 + Set1 bindless
    │   ├── decisions.md         21 条关键设计决策与取舍
    │   └── weak-points.md       架构薄弱点（按影响排序）
    ├── 20-pipeline/
    │   ├── README.md
    │   ├── pass-order.md        Pass 顺序 + 为什么是这个顺序
    │   ├── gbuffer.md           5 个 G-Buffer 附件与格式选择
    │   ├── intermediate.md      中间目标 / Bloom 金字塔 / HDR
    │   ├── ibl.md               split-sum 烘焙三步
    │   └── shaders.md           24 个 shader 清单与编译管线
    ├── 30-systems/
    │   ├── README.md
    │   ├── scene-ecs.md         EnTT / Transform 层级 / 空间索引
    │   ├── jobsystem.md         worker pool / parallelFor / 限制
    │   ├── config.md            EngineConfig v2 / 迁移 / 原子写
    │   └── imgui.md             面板 / 后端 / ASCII 约束
    ├── 40-build/
    │   ├── README.md
    │   ├── dependencies.md      8 个固定版本 / 目录布局 / setup 脚本
    │   ├── build.md             构建流程 / 三条隐形规则
    │   └── troubleshooting.md   构建期与运行期故障排查
    ├── 50-validation/
    │   ├── README.md
    │   ├── cli-reference.md     全部命令行参数
    │   ├── test-matrix.md       20 项回归矩阵与判据
    │   └── acceptance.md        验收标准与"结果可信度"三规则
    ├── 60-debug-profiling/
    │   ├── README.md
    │   ├── zero-overhead.md     两层开关模型与 9 条实现规则
    │   ├── tools.md             工具速查（自研 + 外部）
    │   ├── diagnostics.md       Diagnostics / GpuTimer / Screenshot 实现
    │   └── pitfalls.md          坑清单（改代码前必读）
    ├── 70-status/
    │   ├── README.md
    │   ├── milestones.md        里程碑与实测基线
    │   ├── tech-debt.md         当前技术债
    │   └── known-limits.md      已知边界
    └── 80-roadmap/
        ├── README.md
        └── roadmap.md           F1~F7 主功能路线
```

**层次约束**

- 单篇目标体量：**150~450 行**。超过 500 行就该拆成子文档并在父索引登记。
- 层级最深 3 级（`docs/<分类>/<文档>`）。再深说明分类没分对。
- 每篇必须能被"父索引 + 本页 front-matter"**唯一定位**。

---

## 3. 每个文档的 front-matter

所有 `.md`（含各级 `README.md`）开头必须有：

```markdown
---
id: architecture/rendergraph
title: 渲染图
type: architecture
scope: src/RHI/RenderGraph.{h,cpp}
read-when: 修改 Pass 声明、屏障、子资源校验时
size: ~380 行
updated: 2026-10-30
---
```

| 字段 | 作用 |
|---|---|
| `id` | 稳定标识，跨文档引用用 |
| `title` | 人读标题 |
| `type` | 九种类型之一（见 §1） |
| `scope` | 覆盖的代码路径；**空 scope 表示纯说明文档** |
| `read-when` | **关键字段**：让 LLM 判断"这次要不要读这篇" |
| `size` | 体量提示，帮助估算上下文成本 |
| `updated` | 最后修改日期 |

`read-when` 是这套系统省上下文的核心：新会话只看索引表里的 title + read-when 就能决定读不读正文。

---

## 4. 管理规则（摘要，详细见 [doc-rules](00-meta/doc-rules.md)）

1. **根目录只允许 `README.md`**。其余一律进 `docs/`。
2. **改动子文档 -> 必须回写父索引**。新增/删除/改名/改职责，在**同一次改动**里更新
   本分类 `README.md`、本文 §2 的树、以及根 `README.md` 的索引表。
3. **改代码 -> 必须查 [doc-map](00-meta/doc-map.md)**，按表回写受影响文档。
4. **`tools/DocCheck.ps1` 必须通过**（已接入 `tools/Validate.ps1`）。
5. **文档语言：中文**；代码标识符、命令、路径保持原文。ImGui 可见文本必须是 ASCII（见
   [imgui](30-systems/imgui.md)），但文档本身不受此限。
6. **不写"将来可能"**：未做的写进 [roadmap](80-roadmap/roadmap.md)，
   说"已做但没做完"的写进 [tech-debt](70-status/tech-debt.md)。正文只描述**当前事实**。

---

## 5. 完整索引

| 分类 | 文档 | read-when |
|---|---|---|
| meta | [doc-rules](00-meta/doc-rules.md) | 写/改任何文档前 |
| meta | [doc-template](00-meta/doc-template.md) | 新建文档时复制 |
| meta | [doc-map](00-meta/doc-map.md) | **改代码前必查** |
| meta | [reading-paths](00-meta/reading-paths.md) | **新会话第一步** |
| architecture | [layering](10-architecture/layering.md) | 判断改动的归属层 |
| architecture | [rendergraph](10-architecture/rendergraph.md) | 改 Pass 声明 / 屏障 / 子资源 |
| architecture | [resource-lifetime](10-architecture/resource-lifetime.md) | 新增资源 / 改销毁顺序 / 帧槽位 |
| architecture | [synchronization](10-architecture/synchronization.md) | 多帧、双队列、timeline、resize |
| architecture | [descriptors](10-architecture/descriptors.md) | 改描述符布局 / 材质槽位 |
| architecture | [decisions](10-architecture/decisions.md) | **质疑现有设计前** |
| architecture | [weak-points](10-architecture/weak-points.md) | 评估改动风险 |
| pipeline | [pass-order](20-pipeline/pass-order.md) | 调整 Pass 顺序 / 新增 Pass |
| pipeline | [gbuffer](20-pipeline/gbuffer.md) | 改 G-Buffer 布局或格式 |
| pipeline | [intermediate](20-pipeline/intermediate.md) | 改中间目标 / Bloom 级数 |
| pipeline | [ibl](20-pipeline/ibl.md) | 改环境光照 |
| pipeline | [shaders](20-pipeline/shaders.md) | 增删 shader / 编译报错 |
| systems | [scene-ecs](30-systems/scene-ecs.md) | 改场景 / 变换 / 剔除 |
| systems | [jobsystem](30-systems/jobsystem.md) | 用或改并行任务 |
| systems | [config](30-systems/config.md) | 改可调参数 / 配置格式 |
| systems | [imgui](30-systems/imgui.md) | 改面板 |
| build | [dependencies](40-build/dependencies.md) | 换机器 / 加依赖 |
| build | [build](40-build/build.md) | 每次构建 |
| build | [troubleshooting](40-build/troubleshooting.md) | 编不过 / 跑不起来 |
| validation | [cli-reference](50-validation/cli-reference.md) | 加参数 / 写脚本 |
| validation | [test-matrix](50-validation/test-matrix.md) | 加回归项 |
| validation | [acceptance](50-validation/acceptance.md) | 判断"算不算改坏" |
| debug | [zero-overhead](60-debug-profiling/zero-overhead.md) | 加任何调试能力 |
| debug | [tools](60-debug-profiling/tools.md) | 排查性能/画质 |
| debug | [diagnostics](60-debug-profiling/diagnostics.md) | 改验证层/计时/截图 |
| debug | [pitfalls](60-debug-profiling/pitfalls.md) | **改代码前必读** |
| status | [milestones](70-status/milestones.md) | 对照基线 |
| status | [tech-debt](70-status/tech-debt.md) | 决定下一步 |
| status | [known-limits](70-status/known-limits.md) | 判断需求可行性 |
| roadmap | [roadmap](80-roadmap/roadmap.md) | 开新主功能前 |
