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

## D-10：视口交互（点选 + 三轴手柄）

- **点选**：屏幕 → 世界射线，与 `WorldBoundsComponent` 做 ray-AABB，取最近命中。
  天空盒跳过（相机跟随巨球会吞掉所有射线）。点空白处取消选中。
- **三轴手柄**：`W`/`E`/`R` 切换平移/旋转/缩放；平移用世界轴，旋转/缩放用局部轴。
  手柄屏幕尺寸恒定 ~90px；拖拽一次经命令栈合并成**一条**可撤销命令。
- **实测**：三种模式截图确认手柄正确绘制；`[Editor] selfTest PASS` 含
  Gizmo 数学往返（投影→反投影、射线-轴最近点、射线-平面求交）。
- **抓到的真 bug**：Vulkan 投影矩阵 `m[1][1]` 为负，导致 `tanHalfFov` 为负、
  手柄长度被钳到 1e-4，三个箭头叠成一点。见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 26。

## D-11：视口交互修正

用户反馈四个问题，逐条修：

1. **"为什么我可以拖动视角"** —— `CameraController` 直接读 GLFW，不知道 ImGui
   在捕获键鼠。在资产过滤框里敲 "wasd" 相机会跑。已加 `allowCameraInput` 门控
   （鼠标在视口内 / 正在按住右键看 / 未在输入文字 / 未在拖 gizmo）。
2. **"双击球没生成在视角正前方"** —— 新建实体落点从世界原点改为
   `相机位置 + 前向 * 4`。
3. **"点球却选中 Sponza"** —— 实体级 AABB 被相机包住，入射距离恒为 0。
   改为 AABB 粗筛 + Möller-Trumbore 三角形精确求交。
   见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 27。
4. **"应该有白色包围盒线框 / 手柄要对齐包围盒中心"** —— 新增选中实体世界 AABB
   白色 12 边线框；gizmo 中心改用 `WorldBoundsComponent.world.center()`。

- **自检**：`selfTestWithScene()` 新增"射线必须命中近侧物体"断言
  （距离 6 处的立方体，命中应 < 6）。这条断言正是靠它发现了
  `sphere.obj` 其实是半径 1000 的天空盒球。

## D-12：视口交互修复（第二轮反馈）

1. **"拖动时把视口拖动了"** —— `Viewport` 窗口缺 `NoMove`/`NoResize`，
   在视口里按住左键拖拽会把整块面板拖走，位置还会被 `imgui.ini` 记住。
   已补三个标志，布局改用 `ImGuiCond_Always`。
2. **"拖动时包围盒和物体不在一起"** —— UI 在 `render()` 构建，而
   `Scene::update()` 早已跑完，`WorldBoundsComponent` 落后一帧。
   改为拖拽提交后**立刻**刷新世界变换并当场重算手柄锚点与局部轴。
   回归守卫：`selfTestWithScene()` 断言"平移 3 单位后包围盒中心也移动 3 单位"。

## D-13：包围盒跟随（第三轮反馈）

用户反馈"拖动时球体和包围盒还是分离"。上一轮只修了一半：

1. **刷新时机**（D-12 已修）：改完 transform 立刻 `refreshTransformsAndVisibility()`。
2. **绘制顺序**（本轮修，更隐蔽）：包围盒原本在 `gizmo.manipulate()` **之前**
   就画完了，而 gizmo 是在同一帧稍后才改 transform 的 —— 所以框永远用旧数据。
   把 gizmo 拆成 `interact()` / `draw()`，调用方按
   **交互 → 画框 → 画手柄** 的顺序走。

同时按用户建议新增 `--empty-scene`：不加载 Sponza / 天空盒，只留相机，
方便单独观察交互（Sponza 的巨大包围盒会干扰判断）。

- **验证**：`--empty-scene` + 程序化放置一个球，截图确认球 / 白框 / 手柄三者同心。
- **回归**：`tools/Validate.ps1` 20/20 PASS，stderr 0 字节。

## D-14：屏幕坐标 Y 方向修正（第四轮反馈）

用户反馈"拖动时物体和包围盒的移动方向像相反"。

**根因**：`VulkanCamera::setPerspectiveProjection` 里有 `projectionMatrix[1][1] *= -1`
（Vulkan 的 NDC Y 向下）。而 `projectToScreen` / `screenToRay` 都按"NDC Y 向上"
写的，于是投影与反投影一起多翻了一次 —— 两者互相抵消，所以：

- 包围盒/手柄相对真实渲染**垂直镜像**（物体上移、框下移）；
- 拾取射线也镜像（点 A 选中 B）；
- **往返自检照样通过**（两次翻转抵消），只有"世界 +Y 必须投影到更小屏幕 y"
  这类**方向不变量**才能抓到。

修了四处：`Gizmo::projectToScreen`、`Gizmo::screenToRay`、
`ViewportPanel::projectPoint`、`ViewportPanel::screenToRay`。

自检也补强了：投影矩阵改成和引擎一致（含 `[1][1] *= -1`），
并新增上下/左右方向断言。

- **验证**：`--empty-scene` 放一个偏离中心的球，截图确认球与白框同侧、
  绿色 Y 轴指向上方。
- **回归**：`tools/Validate.ps1` 20/20 PASS，stderr 0 字节。

## D-15：包围盒深度测试 + 占位材质棋盘格

用户反馈两条：

1. **"包围盒没被深度测试，始终画在最前面"** —— ImGui 的 draw list 是纯 2D 叠加，
   没有深度概念。新增 `EditorOverlayPass`：`LINE_LIST` 拓扑、世界空间顶点、
   深度附件复用 G-Buffer 深度图（`LOAD_OP_LOAD` + `depthWriteEnable = FALSE`，
   只测不写），颜色目标 `editorViewport`（`LOAD_OP_LOAD` 保住画面）。
   顺序在 Present 之后、UiComposite 之前。手柄仍用 ImGui 画在最前面（刻意，
   否则物体被挡住时抓不到手柄）。
2. **"没有有效材质时应该是黑白棋盘格"** —— `mrt.frag` 加程序化棋盘格，
   两种情况启用：完全没材质（`material.y == 0`）、有材质但没 albedo 贴图
   （`push.albedoFallback`）。后者由 `GeometrySystem` 判断，且**只认实体自己的
   材质** —— glTF 的材质在子网格槽位里，误判会让 Sponza 整片变格子。

附带修了一个 UV 退化问题：`debug_cube.obj` 没有有效 UV，棋盘格会退化成纯色。
用 `dFdx/dFdy` 判断 UV 是否退化，退化时改用世界坐标按法线主轴投影。

- **验证**：空场景放"遮挡方块(3m) + 被选中的球(8m)"，截图确认包围盒被完全挡住、
  手柄仍可见；遮挡方块显示棋盘格。
- **回归**：`tools/Validate.ps1` 20/20 PASS，stderr 0 字节。
