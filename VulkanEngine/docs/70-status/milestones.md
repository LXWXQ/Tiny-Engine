---
id: status/milestones
title: 里程碑与实测基线
type: status
scope: 空
read-when: 对照历史进度、或需要"已知良好基线数字"
size: ~170 行
updated: 2026-10-30
---

# 里程碑与实测基线

## 1. 已完成阶段

| 阶段 | 内容 | 关键成果 |
|---|---|---|
| baseline | 修改前的原始状态 | — |
| A | Vulkan 正确性修复 | 动态状态、视图空间 SSAO、法线接缝 |
| B-prep | 材质槽位可回收 + Model 子网格 | `kInvalidMaterialSlot` 哨兵 |
| B | 接入 tinygltf，加载 Sponza | 几何烘焙到世界空间 + 逐子网格材质 |
| C | IBL 预过滤（split-sum） | 过曝像素 95.9% -> 7.1% |
| D-1(a) | 引入 EnTT/spdlog、升 Vulkan 1.3、落地线性渲染图 | dynamicRendering + sync2 |
| D-1(b) | 全部 Pass 迁移到动态渲染 + 渲染图 | 删除 `PostProcessPass` |
| D-2 | 线性 HDR + 独立 PresentPass + BloomPass | 9 个新 Pass 没写一行屏障 |
| D-2 补充 | Pass 开关纳入命令行自检 | 四种组合回归 |
| D-2 修复 | 修掉"关闭 SSAO 让环境光乘未定义内容" | 开关下沉到着色器 |
| W1 | 运行时配置持久化（`EngineConfig`） | 顺带修掉析构顺序导致的 VMA 崩溃 |
| W2 | 渲染图语义校验（`validateWriteCoverage`） | 把"没人写"变成启动即报错 |
| D-3 | Bindless 材质描述符 | `desc binds` 104 -> 1，FPS 452 -> 498 |
| D-4 | 多帧并行 + timeline semaphore | 2 帧在飞 |
| D-5 | Compute + 异步计算队列 | `ComputeProbePass` GPU 写 + CPU 读回 |
| D-6 | Job System | worker pool + `parallelFor` |
| D-7 | EnTT ECS + Transform 层级 + 空间索引 | 视锥剔除接入渲染 |
| TD-01~10 | 技术债清理 | 见 [tech-debt](tech-debt.md) |
| P0~P3 | 调试/剖析基础设施 | 验证层结构化、GPU 计时、截图、深度校验、基线 diff、证据包 |

## 2. 当前实测基线

环境：Windows / RTX 2070 / Vulkan 1.4 驱动 / Debug 构建 / Sponza 场景。
**只适合同机相对比较，不可跨机对比。**

| 项 | 值 | 证据来源 |
|---|---|---|
| 回归 | 20 项全 PASS，stderr 全 0 字节 | `tools/Validate.ps1` |
| 验证层 | `errors=0 warnings=0` | 每次运行退出打印 |
| syncval | 0 errors / 0 warnings | `--validation-features sync` |
| 场景 | 103 子网格 + 天空盒 = `draw calls 104` | `[Perf]` 行 |
| 三角形 / 顶点 | 270459 / 811377（`vertices` 实际统计的是索引数） | 同上 |
| 描述符绑定 | `desc binds 1` | 同上 |
| 帧率 | 约 500 FPS（Debug） | 同上 |
| 材质槽位 | `26/256` 占用 | 启动日志 |
| GPU 时间分布 | Geometry ~1.0ms、Present ~1.1ms、SSAO ~0.58ms、其余 < 0.3ms | `--gpu-timing` |

## 3. 关键性能对照（同机 Debug）

| 对照 | 数字 | 结论 |
|---|---|---|
| Bloom 开 vs 关 | 455 FPS vs 531 FPS（+17%） | TD-06 的关闭路径真的省 GPU |
| compute probe 默认关 vs 开 | 486 FPS vs 455 FPS（-6.5%） | 探针是固定成本，必须默认关 |
| bindless 前 vs 后 | 452 FPS vs 498 FPS（+10%），绑定 104 -> 1 | D-3 有效 |
| IBL 老实现 vs split-sum | 过曝像素 95.9% -> 7.1% | C 阶段质变 |

## 4. 更新基线的规矩

改完影响性能/画面的代码后：

1. 跑 `tools/Baseline.ps1 -Action Diff` 确认无回归；
2. **成对更新**本文件 §2 与根 [`README.md`](../../README.md) §6；
3. 记录测法（帧数、是否剔除预热、构建配置），否则数字无法复现。

## D-8：场景编辑器框架

- **目标**：把引擎层和编辑器层分开，让后续加编辑功能不必再动 RHI/Render。
- **产出**
  - `src/Editor/` 独立层：`EditorContext`（EngineServices 回调）/ `EditorState` /
    `EditorLayer` / `CommandStack` / `Commands` / `SceneSerializer` / 三个面板
  - `scene::SceneDescription` 纯数据结构，作为`registry <-> JSON` 的中间层
  - `NameComponent`、`Model::getSourcePath()`、`Texture::getPath()` 用于序列化
  - `--editor` 开关；关闭时运行时行为与改造前一致
- **实测**
  - `--editor --selftest 70`：`[Editor] selfTest PASS`，`[Validation] errors=0 warnings=0`
  - 运行时 `--selftest 40`：同样 0 error / 0 warning，无回归
- **自检抓到的真 bug**：`DeleteEntityCommand` 的 `rebuild` 闭包捕获了
  `registry.try_get` 返回的**指针**，`apply()` 销毁实体后悬空，撤销删除失效。
  改为 `std::optional<T>` 值拷贝后通过。见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 24。

## D-9：编辑器可用性补完

- **离屏视口（TD-09）**：PresentPass 写视口纹理，UiCompositePass 只画 ImGui 到交换链。
  相机纵横比改用视口尺寸（此前按窗口算会横向拉伸）。
- **尺寸防抖**：连续 2 帧不变才重建纹理；重建走 `resizeOwnedTexture` 原地进行
  （最初每次 `createTexture` 会以约 270MB/s 泄漏显存）。
- **资产面板**：扫描 `Resources/Models`（实测 7 个模型），双击通过
  `CreateEntityCommand` 创建实体。redo 复用模型快照，不重新读盘。
- **纯 3D 截图**：`--screenshot-viewport`，输出视口纹理（1300x861）。
- **实测**
  - `--editor --selftest 40`：`[Editor] scene selfTest PASS | create/undo/redo via command stack`
  - `--editor --screenshot-viewport`：`[Validation] errors=0 warnings=0`
  - `tools/Validate.ps1`：20/20 PASS，stderr 0 字节
