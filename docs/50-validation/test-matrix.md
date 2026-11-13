---
id: validation/test-matrix
title: 回归测试矩阵
type: validation
scope: tools/Validate.ps1
read-when: 加/改回归项，或某项失败需要知道它在测什么
size: ~220 行
updated: 2026-11-13
---

# 回归测试矩阵

`tools/Validate.ps1` 默认跑 **78 项引擎/编辑器用例 + 2 项工具自测 = 80 项**；
加 `-EnableProfiling` 时再多一项 `p1-profiling-enabled`（第 81 项）。

## 1. 每项的判据（统一）

| 判据 | 说明 |
|---|---|
| 退出码 = 0 | 非 0 即失败 |
| 退出码 = `ExpectedExitCode` | 默认仍是 0；错误路径的用例显式声明（例如 `--not-a-flag` 约定返回 2） |
| `stderr` 0 字节 | 默认要求；`AllowStderr = $true` 的用例放宽，并可用 `MustContainStderr` 断言那里的提示 |
| 验证层标记 | 默认要求 `[Validation] errors=0 warnings=0`；`SkipValidationMarker = $true` 的用例不要求（帮助、纯参数校验这类不进设备） |
| **stderr = 0 字节** | 任何 stderr 输出都算失败（验证层已不写 stderr） |
| 关键 stdout 标记存在 | 每项有自己的 `MustContain` |
| `[Validation] errors=0 warnings=0` | 除 4 项不建设备的用例，其余都必须出现 |

## 2. 引擎用例

| # | 名称 | 命令 | 关键标记 | 测什么 |
|---|---|---|---|---|
| 1 | `jobtest` | `--jobtest` | `[JobSystem] selfTest PASS` | worker pool、parallelFor、嵌套、异常传播 |
| 2 | `td09-config-migration` | `--config-test` | `[Config] selfTest PASS` | v0->v2 迁移、备份、损坏/未来版本保护 |
| 3 | `selftest-default` | `--selftest 120` | `[Scene] ECS/spatial selfTest PASS`、`[Scene] ECS component selfTest PASS`、`[TD07] ComputeProbe disabled...`、`[RenderGraph] setPassEnabled selfTest PASS` | 主路径 + setter 自动 Transform/Bounds dirty 通知 + 增量 AABB + 元数据组件/系统调度 + 原生组件增删/序列化过滤自检 |
| 4 | `selftest-no-bloom` | `--no-bloom` | `[TD06] Bloom disabled: skipping Bright + Down + Up passes` | Bloom 关闭分支真跳过 |
| 5 | `selftest-no-ssao` | `--no-ssao` | `[TD07] ...` | SSAO 关闭分支 |
| 6 | `selftest-no-bloom-no-ssao` | 两者 | 同上两者 | 组合分支 |
| 7 | `td02-cross-queue` | `--cross-queue-test` | `[TD02] compute->graphics verified`、`[TD02] graphics->compute verified` | 跨队列双方向 |
| 8 | `td07-compute-probe` | `--compute-test` | `[D5] async compute probe verified` | GPU 写 + CPU 读回 |
| 9 | `td03-window-resize` | `--resize-test` | `[TD03] Swapchain recreated`、`[TD03] Render targets resized` | 缩放/最小化/恢复 |
| 10 | `td03-render-scale` | `--render-scale 0.5` | `[TD03] Render targets resized` | 动态分辨率 |
| 11 | `td04-material-slot-stress` | `--material-stress` | `[TD04] material slot stress PASS` | 延迟回收 150 轮 |
| 12 | `td05-subresource-probe` | `--subresource-test` | `[TD05] subresource probe active` | mip/layer + buffer 区间正例 |
| 13 | `td05-subresource-error` | `--subresource-error-test` | `[TD05] subresource semantic error PASS` | 反例：必须被拒 |
| 14 | `p0-build-info` | `--build-info` | `[BuildInfo] compiled:`、`[BuildInfo] policy:` | 能力清单自报 |
| 15 | `p0-diagnostics-selftest` | `--diagnostics-test <path>` | `[Diag] diagnostics selfTest PASS` | 合成消息 -> 计数 + JSONL |
| 16 | `p0-gpu-timing` | `--gpu-timing --gpu-timing-csv <path>` | `[Diag] gpu_timing.csv written:`、`[Diag] zero-overhead guard SKIP` | 每 pass 计时 |
| 17 | `p1-screenshot` | `--screenshot <png> --screenshot-frame 30` | `[Diag] screenshot written:` | 无头截图 |
| 18 | `f1-gpu-scene` | `--selftest 40 --gpu-scene` | `[SceneGpu] GPU batch ready`、`[SceneGpu] indirect batch | groups=` | GPU 驱动提交主路径（按组实例化） |
| 19 | `f1b-merged-buffers` | `--selftest 40 --gpu-scene` | `[SceneGpu] merged buffers built` | F1-b 全局合并顶点/索引缓冲 + 一条 indirect |
| 20 | `f1b-merged-off` | `--selftest 40 --gpu-scene --no-merged-buffers` | `[SceneGpu] GPU batch ready`、`[SceneGpu] indirect batch | groups=` | 回退到每模型 indirect |
| 21 | `f1c-hiz-off` | `--selftest 80 --gpu-scene --no-hiz` | `[SceneGpu] cull stats |`、`hizEnabled=0` | Hi-Z 关闭后的对照路径（视锥剔除仍在） |
| 22 | `f1d-stable-path` | `--selftest 120 --gpu-scene` | `[SceneGpu] cull stats |`、`stable=`、`rebuilds=`、`flattenUs=` | 结构签名命中时跳过摊平与 instances/groups 上传，稳定帧只剩 frameData 上传 |
| 23 | `f1d-sync-validation` | `--selftest 60 --gpu-scene --validation-features sync` | `[Diag] validation features enabled: synchronization`、`[Validation] errors=0 warnings=0` | 同步校验（syncval）0 错误，Hi-Z 屏障与跨帧复用无先读后写冲突 |
| 24 | `f1e-screen-space` | `--selftest 40 --gpu-scene --gpu-scene-min-px 8` | `[SceneGpu] cull stats |`、`screenTests=`、`[Validation] errors=0 warnings=0` | 屏幕空间误差剔除：投影过小的组被剔除，`stats[2]` 有测试/剔除计数 |
| 25 | `f1e-external-instances` | `--selftest 40 --gpu-scene --gpu-scene-external 4` | `[SceneGpu] external instances | count=4`、`external=4`、`[Validation] errors=0 warnings=0` | 外部实例（粒子/体素入口）与 ECS 实例共用分组、合并缓冲与同一条 indirect 命令 |
| 26 | `f2-streaming-world` | `--selftest 90 --streaming-world 2` | `[Streaming] world ready | radius=2`、`[Streaming] chunks=25`、`[Validation] errors=0 warnings=0` | 流送世界在构造期建立并填满 5x5 环形（半径 3 时 49 个 chunk 实体，仍低于地形 GPU 选择的 64 slot 上限） |
| 27 | `f2-streaming-fly` | `--selftest 150 --streaming-world 2 --streaming-fly 60` | `[Streaming] chunks=`、`unloaded=`、`[Validation] errors=0 warnings=0` | 相机沿圆周飞行，chunk 持续加载与卸载（实测 `loaded=54 unloaded=19`），滞回环工作正常 |
| 28 | `f2b-terrain-batch` | `--selftest 130 --streaming-world 2` | `[Terrain] mesh arena ready`、`[Terrain] batched draw |`、`heightCaptured=1 heightMin=0 heightRange=12 heightMixed=0`、`[Validation] errors=0 warnings=0` | 多 chunk 一条 indirect：arena 固定槽位 + set 3 实例表 + 2048 条命令区；同时断言所有 chunk 共用全局高度范围 |
| 29 | `f2b-terrain-batch-off` | `--selftest 60 --streaming-world 2 --no-terrain-batch` | `[Terrain] terrain batching disabled |`、`[Terrain] chunk shading | heightCaptured=1 heightMin=0 heightRange=12 heightMixed=0`、`[Validation] errors=0 warnings=0` | `--no-terrain-batch` 回退到逐 chunk 绑定 + 逐 chunk draw；同时断言逐 chunk push 也用全局高度范围 |
| 30 | `f2c-origin-rebase` | `--selftest 240 --streaming-world 2 --streaming-fly 60 --origin-threshold 64` | `[Origin] rebased |`、`[Origin] stats |`、`[Validation] errors=0 warnings=0` | 浮动原点：相机越过阈值时整场景回原点，`worldOrigin` 用 double 累计，chunk 索引与 map key 一起重排（实测 240 帧内 4 次 rebase，chunk 数稳定 35） |
| 31 | `f2c-origin-off` | `--selftest 240 --streaming-world 2 --streaming-fly 60 --origin-threshold 64 --no-origin-rebase` | `[Origin] stats | rebases=0`、`[Validation] errors=0 warnings=0` | 关闭 rebase 的对照：原点保持 (0,0,0)，局部坐标一路变大也不崩 |
| 32 | `f2d-streaming-radius` | `--selftest 150 --streaming-world 8` | `[Streaming] chunks=289`、`[Terrain] mesh arena ready | vertexMB=`、`[Validation] errors=0 warnings=0` | 半径上限 12 后整环 289 块全部落地，GPU 选择槽位/arena 同步扩容 |
| 33 | `f2d-streaming-budget` | `--selftest 120 --streaming-world 8 --streaming-budget 0.2` | `budgetHits=`、`[Validation] errors=0 warnings=0` | 每帧生成预算生效：超预算就停发新 chunk，慢帧不再雪崩 |
| 34 | `f2e-terrain-picking` | 地形拾取走高度场（编辑器自检） | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] streamed terrain PASS`、`picking=OK`、`[Editor] selfTest PASS` |
| 35 | `f2f-arena-slots` | `--selftest 180 --streaming-world 8` | `[Terrain] mesh arena ready | vertexMB=`、`arenaAllocs=`、`[Validation] errors=0 warnings=0` | arena 归 `Scene` 所有，chunk 落地即 `acquire()` + `copyInto()` + `Model::releaseGpuBuffers()`；容量按实际尺寸分配，`arenaAllocs` 随 chunk 生灭回落（飞行 600 帧 305~323） |
| 36 | `f2h-large-radius` | `--selftest 400 --streaming-world 16` | `mesh arena resized | radius=16`、`[Streaming] chunks=1089`、`[Validation] errors=0 warnings=0` | chunk 坐标 int64 + 采样锚点 double，半径上限 24；arena 按整环容量原地重建（`reconfigure()`，渲染侧只持 `this` 指针） |
| 37 | `e1-batch-commands` | 编辑器批量命令（Composite / Duplicate）、预制体与原生组件 | `TinyEditor.exe --selftest 5 --empty-scene` | `batch delete/duplicate`、`[Editor] prefab selfTest PASS`、`components=28`、`[Editor] selfTest PASS` | 多选删除是一条撤销、复制含子树且名字追加 `" Copy"`、`revert()` 清空副本；预制体保存/读取/实例化/undo/redo 全通过；原生 Character 不重复写入 v3 components |
| 38 | `e2-multi-transform` | 编辑器多对象变换（一条命令） | `TinyEditor.exe --selftest 5 --empty-scene` | `multi transform`、`[Editor] selfTest PASS` | 多选拖拽只压一条命令、`mergeWith` 只接受同一选择 |
| 39 | `e3-material-undo` | 材质参数与 Render Tag 的撤销 | `TinyEditor.exe --selftest 5 --empty-scene` | `material/render tag`、`[Editor] selfTest PASS` | 滑杆与复选框各压一条命令；undo 时同时把 GPU factor 写回去 |
| 40 | `e3-play-doc-lock` | 播放期间文档只读 | `TinyEditor.exe --selftest 5 --empty-scene` | `play document lock selfTest PASS` | Playing 下 save/new/open 全被拒绝并留日志 |
| 41 | `e4-unsaved-prompt` | 脏文档的操作确认与标题 | `TinyEditor.exe --selftest 5 --empty-scene` | `unsaved changes prompt selfTest PASS` | 脏状态下 new 只排队不执行；标题带 `*` 且变干净后去掉 |
| 42 | `e4-preferences` | 编辑器偏好持久化（面板可见性 / Gizmo 模式 / 最近场景） | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] preferences selfTest PASS`、`preferences file selfTest PASS` | 内存往返 + 真写 `.tmp/editor-preferences-selftest.json` 再读；坏 JSON 不让启动失败；最近列表去重、置顶、上限 8 |
| 43 | `e4-shortcut-sheet` | 快捷键速查表数据完整性 | `TinyEditor.exe --selftest 5 --empty-scene` | `shortcut sheet selfTest PASS` | 表项非空、键不重复；弹窗与 Help 菜单共用同一张表 |
| 44 | `e4-history-jump` | 撤销历史跳转 | `TinyEditor.exe --selftest 5 --empty-scene` | `history jump selfTest PASS` | `jumpTo` 逐步 undo/redo（不整段搬运），越界 clamp 到总深度 |
| 45 | `e5-toolbar-snap` | 工具栏/状态栏与 Gizmo 吸附 | `TinyEditor.exe --selftest 5 --empty-scene` | `snap selfTest PASS`、`[Editor] selfTest PASS` | `snapValue` 按最近刻度取整（step=0 恒等）；工具栏/状态栏固定矩形，面板整体下移 |
| 46 | `e5-render-settings` | 渲染设置面板（engine.json 默认值与提交路径调试） | `TinyEditor.exe --selftest 5 --empty-scene` | `render settings selfTest PASS`、`[Editor] selfTest PASS` | SSAO 核夹到 4..64 偶数；画质功能在 Inspector 场景组件中管理，Render 面板只保存/重载 engine.json 与 GPU 批调试 |
| 47 | `f2i-update-profile` | `--selftest 240 --streaming-world 8 --update-profile` | `[Update] avg us |`、`[Validation] errors=0 warnings=0` | `Scene::update()` 五段分别计时（genPoll / streaming / world / spatial / visibility），每 60 帧打印一次；关闭时零开销 |
| 48 | `p2-device-fault` | `--device-fault-test` | `[DeviceFault] selfTest PASS` | `VK_EXT_device_fault` 能力就绪 |
| 49 | `terrain-render` | `--selftest 20 --empty-scene --terrain-test` | `[Terrain] selfTest PASS`、`[Terrain] LOD stitching PASS`、`[Terrain] GPU mesh assembly selfTest PASS`、`[Terrain] GPU selection selfTest PASS`、`[Terrain] GPU selection transition PASS`、`[Terrain] demo entity created`、`chunks=4`、`lods=3`、`submeshes=12`、`[Terrain] async generation PASS`、`[Terrain] build task retirement PASS`、`[Terrain] async GPU upload PASS`、`[Terrain] spatial region query PASS | chunks=9 regions=5x5`、`[Terrain] sculpt PASS | chunks=9 rebuilt=1`、`[Terrain] character controller PASS | grounded=1`、`[Terrain] character serialization PASS | captured=1`、`[Terrain] material sets PASS | resident=4 max=4 external=1 fallback=3 serialized=1`、`streaming chunk test PASS | chunks=9 independentModels=9 gpuMesh=9` | CPU 算法/16 张源纹理缓冲 + height/roughness packing + JobSystem 异步构建 + chunk/LOD 子网格/AABB/裙边 + 稳定帧 indirect 合批 + GPU 高度场 + GPU compute 单 chunk 顶点/索引组装 + device-local VBO/IBO Model 接管 + 独立 chunk 流送 smoke + 8 槽异步高度场 + 4 槽异步 GPU mesh submit/poll + 257² 单 chunk 非阻塞重建基线 + CPU 构建中销毁实体的任务退役（retiring 队列） + XZ region 查询/组件私有字段/统一销毁 + 区域限定运行期雕刻 + kinematic 角色控制器/贴地/跳跃/序列化 + 4 个 Set2 材质集/按需外部 16 PNG/程序化回退/`materialSetIndex` 序列化/按材质批次重绑 + chunk→manager picking/删除撤销 + 全 LOD 邻接缝合（skirts=off） + GPU selection parity/过渡（4-int state slot） + CPU 高度/射线碰撞 + 地形 G-Buffer / 12-sampler Set2 四层 PBR 材质 / Surface Style / SSAO 策略 |
| 50 | `b1-help` | `--help` 打印分组参数表 | `TinyEngine.exe --help` | `Tiny Engine -`、`--selftest [N]`、`--validation-features <list>`、`--update-profile` | 退出码 0、不创建窗口/设备（`SkipValidationMarker`）；参数表是 `kUsageLines` 数据，不是注释 |
| 51 | `b1-unknown-arg` | 未知参数不再静默忽略 | `TinyEngine.exe --not-a-flag` | `unknown argument: --not-a-flag` | 退出码 2（`ExpectedExitCode`）、允许 stderr（`AllowStderr`）、提示走 stderr 而 stdout 保持干净；stderr 重定向时是 ANSI 代码页，只断言 ASCII |
| 52 | `b1-window-fit` | 窗口按显示器工作区收敛（1366x768 / 150% 缩放不再把状态栏推出屏幕）+ 启动失败正文 | `TinyEngine.exe --selftest 5 --hidden` | `[Window] fit selfTest PASS`、`[Startup] error log selfTest PASS`、`[Startup] error dialog text selfTest PASS`、`[Window] sizing` | 1920x1080 在 1280x720 工作区收敛到 1177x662；2560x1440 保持 1920x1080；工作区缺失时退回默认；弹窗正文含阶段/原因/日志路径/`--help`，自检不真的弹窗；真实 MessageBox 用无效 `VK_ICD_FILENAMES` 桌面手测 |
| 53 | `b2-empty-state` | 冷启动空场景要有文字出口（视图中央说明卡 + Hierarchy 空树提示 + Assets 目录缺失说明） | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] empty state selfTest PASS` | 天空盒与相机不算内容；有网格才算非空；资产目录缺失时面板显示原因与下一步 |
| 54 | `b3-notices` | 通知状态机：Toast 衰减、错误摘要、未读徽标、打开失败重试状态 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] notices selfTest PASS` | 本帧新错误不算已读；`notifySuccess` 清摘要；`failedOpenPath` 可清空 |
| 55 | `b4-shortcuts` | 快捷键单一数据源：条目数、键位标签、键位冲突检测 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] shortcut sheet selfTest PASS`、`conflicts=0` | `kShortcutEntries` 同时驱动速查表与分发；`Ctrl+Z` 与 `Ctrl+Shift+Z` 是两条精确匹配的条目 |
| 56 | `b5-language-console` | 状态栏数量缩写 + Console 时间戳：`formatCount()` 的 k/M 边界与 `HH:MM:SS` 形状 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] status bar selfTest PASS` | 语言分层（B5/UX-07）、Console 时间列与计数（UX-11）、状态栏三段（UX-15）、诊断入口（UX-18） |
| 57 | `b6-console-search` | Console 关键字搜索（`matchesSearch` 的大小写/空串/中文）+ 地形创建两阶段请求 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] console filter selfTest PASS`、`[Editor] terrain create request selfTest PASS`、`entries=20 conflicts=0` | UX-11 续（Console 搜索）、UX-08（两阶段提示） |
| 58 | `e6-asset-and-textures` | 资产拖拽落点判据（命中地形 / 相机前方 8m）+ 贴图库扩展名过滤 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] asset drop selfTest PASS`、`[Editor] texture library selfTest PASS` | E6（资产拖拽 / 材质贴图选择） |
| 59 | `async-terrain-create` | 异步创建地形的三段状态：当帧实体存在且 generating、当帧无 MeshComponent、wait 后几何就绪（并把同步 105.9 ms / 异步提交 0.28 ms 打进日志） | `TinyEditor.exe --selftest 5 --empty-scene` | `[Scene] async terrain create selfTest PASS` | TD-24（真异步地形生成） |
| 60 | `f6-shadows` | 级联阴影：三级联矩阵自检（split 递增、末级被 90 夹住、8 角点落在光空间裁剪体内、远处球被剔除）+ 首帧确实注册了 Shadow pass | `TinyEngine.exe --selftest 20` | `[Shadow] selfTest PASS`、`[Shadow] first frame \| cascades=3` | 手写正交投影（`glm::ortho` 在 ZERO_TO_ONE+右手空间下深度落在 [0,1] 之外）；AABB 8 角点剔除；默认场景阴影偏淡是光照配比所致（方向光约占 5% 亮度） |
| 61 | `f3-voxel` | 方块体素：面剔除 6/10/6、边界连续、DDA 命中与挖深/放回、地面高度、角色站上方块、面放置；图集布局 512/64/128/32 | `TinyEngine.exe --selftest 20 --voxel-world 1` | `[Voxel] atlas=512 tile=64 pitch=128 gutter=32`、`[Voxel] selfTest PASS`、`[Voxel] scene selfTest PASS` | 顶点材质必须给白贴图否则走棋盘格；旧 Model 换指针前要 `retireModel`；边界格要连带重建邻块；改图集尺寸/间距/gutter 要同步 `mrt.frag` 和 `glass.frag` |
| 62 | `f3-voxel-arena-625` | 半径 12 的 625 块体素世界整批走 arena + 间接提交，验证材质槽/实例表/命令缓冲容量 | `TinyEngine.exe --selftest 200 --voxel-world 12` | `chunks=625`、`arena=625`、`[Voxel] arena batch`、`[Validation] errors=0 warnings=0` | 625 块曾被 256 个材质槽挡住（已扩到 1024）；反射视图不走体素间接批，回退逐块 arena draw |
| 63 | `e8-editor-mode` | 编辑 / 运行模式：编辑模式允许选中、运行模式禁止选中（坐标系/层级/视口三处守卫），快捷键表里有模式切换；帮助 → 示例的体素玻璃与体素世界预设能建世界、等异步网格落地并取景 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] editor mode selfTest PASS`、`entries=20 conflicts=0`、`[Editor] example presets selfTest PASS`、`voxel-world meshes=9 bounds=9 parented=9` | 运行模式进入时清空选中；`Ctrl+左键` 永远是多选（笔刷改到运行模式下的裸左键）；示例自检会清掉当前测试场景；`bounds=9` 守住异步 chunk 的空间索引登记（缺 WorldBounds 会全黑）；`parented=9` 守住 chunk 挂在 `Voxel World` 下的场景树结构 |
| 64 | `e9-runtime-character` | 运行模式接管角色：沿视线 ray-march 找地面出生，挂 `Character.glb`（skinned=1 animations=18），6 个 clip 全部可查且初始为 Idle；相机跟脚底，退出后销毁角色并还原相机；`runtimeYaw/runtimePitch` 原样保存相机弧度，WASD 方向与视线一致 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] runtime character selfTest PASS \| camera follows feet, exit restores pose \| skinned=1 animations=18`、`clips=Idle/Walk/Run/Jump/Jump_Idle/Jump_Land active=Idle` | 出生点不再固定相机前方 2m：视线 2..180m 内命中地面才落地，找不到才走重力兜底；角色无贴图材质必须把 `materialAlbedoFallback` 置 0，否则走棋盘格；蒙皮模型 `isDynamic()`，不能进静态 GPU 合批；yaw 若再按“度→弧度”转一次会缩小约 57 倍，W/S/A/D 全部反向 |
| 65 | `f3-voxel-persist` | 体素世界持久化：挖掉一格后 `captureDescription()` 记录该格 Air、JSON 往返后 seed/半径/baseY/chunk 坐标/edit 四元组逐字段一致 | `TinyEditor.exe --selftest 5 --empty-scene` | `[Editor] voxel edit selfTest PASS`、`capture + json round trip` | 只存 seed + 差分，不存 32768 格；场景 JSON 版本 2，老文件缺 `voxelWorlds` 按空处理 |
| 66 | `f3-voxel-glass` | 体素玻璃透明：玻璃拆出独立网格（每面 4 顶点）走 `GlassPass`，按面质心从远到近排序后一次 draw，透明叠放无黑边 | `TinyEngine.exe --selftest 20 --voxel-world 1 --voxel-glass-demo` | `[Voxel] glass demo placed \| blocks=`、`[Glass] sorted=`、`drawcalls=1` | 玻璃不进 G-Buffer；`GlassPass` 深度 LOAD + depthWrite 关；图集无 alpha，只有 Glass 走混合；CLI 放玻璃前必须 `waitForVoxelBuilds()`，否则初始异步网格落地会覆盖 `glassFaceCount`，玻璃 pass 一场都不画 |
| 67 | `f3v-taa` | TAA 时域抗锯齿：Halton(2,3) 抖动、历史重投影与 AABB clamp 生效，渲染图双附件正常 | `TinyEngine.exe --selftest 40 --voxel-world 1` | `[TAA] initialized \|`、`[Validation] errors=0 warnings=0` | 历史图读写不能与颜色附件同一 layout；`updateDescriptors` 的 currentColor 必须绑 `hdrScene` |
| 68 | `f3v-taa-off` | TAA 关闭路径：权重与抖动归零，Pass 仍执行以写 `taaOutput` | `TinyEngine.exe --selftest 20 --no-taa` | `[TAA] disabled`、`[Validation] errors=0 warnings=0` | 下游 Bloom/Present 固定绑 `taaOutput`，不能真跳过 |
| 69 | `f3v-water` | 水面在主路径注册：径向网格、4 个大波、1024x1024 无缝法线贴图细节、平面反射与厚度吸收；只绑 Set0，混合进 HDR | `TinyEngine.exe --selftest 20 --voxel-world 1` | `[Tiny] Water normal map loaded \|`、`[Water] initialized \|`、`[Water] first draw \|`、`[Validation] errors=0 warnings=0` | 没有地形/体素块时不注册；`--no-water` 时完全不注册；push constant 48 字节，shader 只消费 position 与 params3 |
| 70 | `f3v-water-reflection` | 半分辨率平面反射：镜像相机重画 960x540 G-Buffer 与延迟光照，水面从 Set0 binding12 采样 | `TinyEngine.exe --selftest 20 --voxel-world 1` | `[Reflection] planar reflection initialized \|`、`[Reflection] first draw \|`、`planarReflection=1`、`[Validation] errors=0 warnings=0` | 开关 `water.planarReflection` 默认开；关闭时 binding12 必须回退环境图，否则指向未转换布局的纹理会报 40 个验证层错误；反射只含不透明几何 |
| 71 | `f3v-water-off` | 水面关闭路径 | `TinyEngine.exe --selftest 20 --voxel-world 1 --no-water` | `[Water] disabled`、`[Validation] errors=0 warnings=0` | 与 TAA 不同：Water 是"真跳过"，因为下游没有任何 pass 固定读它的输出 |
| 72 | `f3v-ssr` | 水面近场 SSR：半分辨率 48 步屏幕空间追踪，结果经 Set0 binding13 混入水面反射；主路径注册在 Water 之前 | `TinyEngine.exe --selftest 20 --voxel-world 1` | `[SSR] initialized \|`、`[SSR] first draw \|`、`[Water] first draw \|`、`ssr=1`、`[Validation] errors=0 warnings=0` | 只在水面启用、场景有地形/体素块时注册；屏幕外/超过 `ssrMaxDistance` 回退平面反射/IBL；关闭时 binding13 必须回退环境图 |
| 73 | `f3v-ssr-off` | SSR 关闭路径：Pass 不注册，水面仍能画，日志 `ssr=0` | `TinyEngine.exe --selftest 20 --voxel-world 1 --no-ssr` | `[SSR] disabled`、`[Water] first draw \|`、`ssr=0`、`[Validation] errors=0 warnings=0` | 与 `--no-water` 不同：只关 SSR，水面/平面反射继续；Water 的 `params3.y` 分支必须跳过采样 |
| 74 | `f6-particles` | 粒子发射器 + Kenney sprite：空场景创建 `effects.particles` 喷泉，启动预加载 `Texture/Particles`，CPU 发射/模拟后由 `ParticlePass` 前向透明绘制 | `TinyEngine.exe --selftest 60 --empty-scene --particle-demo` | `[Particle] textures=`、`status=ready`、`[Particle] demo emitter created \|`、`texture=Particles/Kenney/fire_01.png`、`[Particle] emitters=1 alive=`、`drawcalls=1`、`[Validation] errors=0 warnings=0` | 粒子不进 G-Buffer；Pass 在 Water 后、TAA 前，LOAD HDR + 深度、`depthWrite = FALSE`、按相机距离排序；set1 bindless 材质数组，空/失效路径回退圆形；新增贴图需重启；`enabled=false` 停发但存量走完寿命 |
| 75 | `f6-clouds` | 体积云：空场景创建 `scene.clouds`，启动生成 64³ RGBA8 可平铺噪声，`CloudPass` 在 Lighting 后全屏 raymarch，读 G-Buffer 世界位置截断，预乘混合进 HDR | `TinyEngine.exe --selftest 30 --empty-scene --cloud-demo` | `[Cloud] initialized`、`[Cloud] demo scene ready`、`[Cloud] first draw`、`[Validation] errors=0 warnings=0` | 云默认关闭（`cloudEnabled=false`），只有 `scene.clouds` 存在且启用时才注册；无深度附件、不写 G-Buffer；4 步太阳自阴影、`maxDistance=6500`、步骤上限 128；`--cloud-demo` 设 coverage=.55、base=200、thickness=120、steps=56；云不进平面反射/阴影，也没有时域重投影 |
| 76 | `f6-sun` | 太阳盘 + 镜头光晕：空场景创建 `scene.sun` 与 `scene.environment`，`SunFlarePass` 在 TAA 后、Bloom 前读深度、纯加色写 `taaOutput`；日盘按世界空间射线夹角绘制，鬼影沿太阳-屏幕中心镜像 | `TinyEngine.exe --selftest 30 --empty-scene --sun-demo` | `[Sun] initialized \|`、`[Sun] demo scene ready \|`、`[Sun] first draw \|`、`[Validation] errors=0 warnings=0` | 太阳默认关闭（`sunEnabled=false`），只有 `scene.sun` 存在且启用时才注册；方向/光色跟随 `scene.environment`，不单独存太阳方向；5 点深度只决定日盘可见度，鬼影不逐像素遮挡；放在 TAA 后避免拖影、Bloom 前让日盘参与辉光；云不写深度所以日盘会叠在云上；太阳不进平面反射/SSR |
| 77 | `f6-vegetation` | 地形植被：空场景 + 流送地形生成程序化草/树，`VegetationPass` 在 Geometry 后、SSAO 前写 G-Buffer；草/树各一次实例化 draw | `TinyEngine.exe --selftest 90 --empty-scene --streaming-world 3 --vegetation-demo` | `[Vegetation] meshes \|`、`[Vegetation] demo scene ready \|`、`[Vegetation] placed \| grass=`、`[Vegetation] first draw \|`、`[Validation] errors=0 warnings=0` | 默认关闭（`vegetationEnabled=false`），只有 `scene.vegetation` 存在且启用时才放置/绘制；只采样地形、跳过水面与陡坡；放置去抖：相机>6m、参数变化或地形签名稳定 0.5s；不投自身阴影、不进平面反射；无 LOD/外部资产，实例上限草 49152 / 树 4096 |
| 78 | `f6-fog-volume` | 局部体积雾：空场景 + 流送地形创建一个 `effects.fogVolume` 雾盒，组件的 center/size/rotation 定义局部盒体再乘实体 WorldTransform；`FogVolumePass` 在 Cloud 后、反射/透明前逐盒 raymarch，读 G-Buffer 世界位置截断，预乘混合进 HDR | `TinyEngine.exe --selftest 70 --empty-scene --streaming-world 3 --fog-demo` | `[FogVolume] initialized \| noise=32^3`、`[FogVolume] demo volume created \| density=0.045 center=-6,10,-10 size=30x10x36 steps=24`、`[FogVolume] first draw \|`、`[Validation] errors=0 warnings=0` | 默认不添加，只有存在且启用的 `effects.fogVolume` 实体才注册 Pass；size 为全尺寸、最多 8 个；32³ 静态噪声、单盒 steps 1..32；无深度附件、不写 G-Buffer，雾不进平面反射/CSM 阴影；地形/体素对象禁止挂通用组件；`--fog-demo` 用固定 `groundY=10`，流送 chunk 创建时尚未落地 |

### 迁移改造自检（M1 系列，均不创建 Vulkan 设备）

| 名称 | 参数 | 断言标记 | 覆盖内容 |
|---|---|---|---|
| `m1-handle-contract` | `--handle-test` | `[Handle] selfTest PASS` | 句柄失效、索引复用升版本、拷贝与往返 |
| `m1-trace-contract` | `--trace-test` | `[Trace] selfTest PASS` | 命中 epsilon、request 语义、surface 位运算、hitPoint |
| `m1-b1-gltf-nodes` | `--gltf-node-test` | `[Model] nodeSelfTest PASS` | 父子链合成、TRS 顺序、越界返回单位矩阵 |
| `m1-b2-tpack` | `--pack-test` | `[AssetPack] selfTest PASS` | 包格式、16B 对齐、后缀查找、挂载后回读 |
| `m1-b3-material-json` | `--material-json-test` | `[Material] jsonSelfTest PASS` | 只写非默认字段、往返、25 材质 -88% |
| `m1-b4-terrain-sculpt` | `--terrain-sculpt-test` | `[Terrain] sculptDeltaSelfTest PASS`、`[Terrain] sculptJsonSelfTest PASS` | 差分只收改动格、存档往返、空差分不写盘 |
| `m1-b5-texture-budget` | `--texture-budget-test` | `[Texture] budgetSelfTest PASS` | mip 链字节、25% / 75% 两档降级、记账与峰值 |

## 3. 工具自测

| # | 名称 | 内容 |
|---|---|---|
| 79 | `p3-baseline-diff` | 先 `Baseline.ps1 -Action Save` 再 `-Action Diff`，三维度（GPU 时间 / 验证层 / 视觉）自动判回归 |
| 80 | `p3-debug-bundle` | `DebugBundle.ps1` 轻量版（跳过深度校验与基线 diff）产出证据包 |
| 81 | `p1-profiling-enabled` | 仅 `-EnableProfiling` 时追加：`--capture-frame 10` 抓帧路径（默认 80 项不含它） |

## 4. 文档一致性检查

`Validate.ps1` 还会调用 `tools/DocCheck.ps1`：

| 检查 | 失败条件 |
|---|---|
| 根目录只有 `README.md` | 出现其它 `.md` |
| front-matter 完整 | 缺字段 / 类型非法 / `id` 与路径不匹配 |
| 父索引覆盖 | 新增文档未在父 `README.md` 登记 |
| 相对链接可达 | 链接目标不存在 |
| 单文档行数 | 不检查（2026-11-04 取消 600 行上限） |

## 5. 加一项回归

1. 确认它能输出一个**唯一、稳定**的 stdout 标记（不要用会变的内容如帧率）；
2. 在 `tools/Validate.ps1` 的 `$tests` 数组里加一项：
   `@{ Name = "..."; Args = @(...); MustContain = @("可选的唯一标记") }`；
3. 若它不创建 Vulkan 设备，要加进"跳过零验证层检查"的白名单；
4. 在本文件 §2/§3 补一行；
5. 跑一次 `tools/Validate.ps1` 确认全绿。
