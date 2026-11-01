---
id: status/milestones-archive
title: 历史里程碑归档（D-8～D-20）
type: status
scope: 空
read-when: 需要回看编辑器与地形阶段 6 的逐轮实现记录
size: ~260 行
updated: 2026-11-01
---

# 历史里程碑归档（D-8～D-20）

> 当前状态、实测基线与近期里程碑见 [milestones](milestones.md)。
> 本页只保留已稳定的历史实现记录，不再追加新里程碑。

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
  - `tools/Validate.ps1`：21/21 PASS，stderr 0 字节

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
- **回归**：`tools/Validate.ps1` 21/21 PASS，stderr 0 字节。

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
- **回归**：`tools/Validate.ps1` 21/21 PASS，stderr 0 字节。

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
- **回归**：`tools/Validate.ps1` 21/21 PASS，stderr 0 字节。

## D-16：占位棋盘格修复

用户反馈两条：没有有效材质的子网格每帧往 stderr 刷警告；棋盘格整片发白，
只剩格子缝是暗的。

1. **警告刷屏**：`GeometrySystem` 优先读 `Material::descriptorSetIndex`，
   而编辑器实体只写了 `MaterialComponent::materialSetIndex`，于是每帧都误判成
   "没有材质"。改为只认实体组件字段；提示改成每个实体最多一行，且走 `stdout`
   （`stderr` 必须保持 0 字节）。
2. **棋盘格发白**：盒式滤波在窗口小于格子时外插到 `[-1.5, 2.5]`，负 albedo
   写进 HDR G-Buffer 后，PresentPass 的 ACES 会把负输入 clamp 成**白色**。
   修法是把 pattern clamp 到 `[0,1]`，并改成局部空间三平面投影
   （球面不再有选轴接缝、不依赖 UV）；fallback 的 PBR 固定为金属度 0、粗糙度 1。
3. **立方体侧面变条纹**：`debug_cube.obj` 没有 `vn`，`Model.cpp` 原来把缺失法线
   填成 `(0,1,0)`，导致所有面都按 +Y 投影。改成零法线，片元着色器在零法线时
   用屏幕导数重建几何法线（同时补上这类模型的 G-Buffer 法线）。
4. **覆盖到 glTF 子网格**：`SubMesh` 增加 `albedoFallback`，`GLTFLoader` 在材质
   没有 `baseColorTexture` 时置位。这样"一切没有纹理的网格"都会走棋盘格，
   而不只是编辑器新建的实体。
5. **验证**：`--empty-scene` + 资产面板创建 `Quad-Sphere.obj` / `plane.obj` /
   立方体，截图都是清晰黑白格（立方体六个面都正确）；`tools/Validate.ps1`
   21/21 PASS，stderr 0 字节。

## D-17：OBB 选中框 + 四元数旋转 + 3D 棋盘

用户反馈三条：旋转时包围盒不跟着转；某些旋转方向相反；球体的黑白格着色有明显
问题。

1. **包围盒必须是 OBB**：`EditorOverlayPass` 原来用 `WorldBoundsComponent`
   的轴对齐 AABB，旋转后框会被包大且不跟着转。改为 `BoundsComponent.local`
   的 8 个角点乘 `WorldTransformComponent.matrix`；Gizmo 手柄锚点同样用
   `matrix * local.center()`。没有局部包围盒的实体才退回世界 AABB。
2. **旋转改用四元数**：`TransformComponent` 新增 `orientation`，`localMatrix()`
   由它生成；欧拉角 `rotation` 只做 Inspector / JSON / 相机显示。Gizmo 拖拽
   从"直接加欧拉分量"改成 `startOrientation * angleAxis(delta, 局部轴)`，
   方向由 `buildPlaneBasis` 的右手系保证，pitch 到 ±90° 不再万向节死锁。
   `SetTransformCommand` 同时保存欧拉角与四元数，撤销/重做写回 `orientation`；
   `CommandStack` 合并时由上一条命令 `absorb()` 新值，重做能回到拖拽最后一帧。
3. **棋盘格**：三平面在球面 45° 过渡带会把黑白混成灰带，UV 图集子区间强行
   拉伸又会接缝/扭曲。改为两条路径：UV 完整时用 8 格 UV 棋盘（导数跨接缝
   折回）；无 UV 或 UV 只占图集子区间时用局部空间 3D 棋盘（周期 1、加非整数
   偏移避免整面落在格子边界、屏幕导数抗锯齿）。`Quad-Sphere.obj` 的 UV 只占
   0.05~0.43，走 3D 路径；`debug_cube` 六个面仍清晰。
4. **验证**：`--empty-scene` 截图确认立方体/球体都是干净黑白格；Inspector
   拖 Rotation X 后白框跟着立方体倾斜；Gizmo 拖 Z 环后框与手柄同步旋转；
   `tools/Validate.ps1` 21/21 PASS，stderr 0 字节；`--selftest` 里新增
   四元数/欧拉矩阵等价、万向节有限性、单轴反解断言。
## D-18：程序化地形 MVP（F2）

- **范围**：单块、CPU 同步生成、顶点色着色；复用现有 `Model` / `GeometrySystem` /
  G-Buffer，不新增 Pass、RenderQueue 或 descriptor binding。
- **CPU 层**：`src/Terrain/` 新增 `TerrainConfig` / `TerrainNoise` /
  `TerrainHeightField` / `TerrainMeshBuilder` / `TerrainSelfTest`。
  默认 129²、上限 257²；Perlin + fBm 无全局状态，同 seed 逐元素确定。
- **Scene/ECS**：`TerrainComponent` 保存参数、generatedConfig、revision、统计和错误；
  `RenderTagComponent::isTerrain`。`Scene::buildTerrainModel()` 先完整构建成功再替换
  旧网格，失败保留旧网格。地形实体没有 MaterialComponent。
- **渲染**：`SimplePushConstantData` / `mrt.vert` / `mrt.frag` 同步增加
  `terrainMode`；片元在写真实 G-Buffer 法线后走顶点色 albedo + roughness 0.95。
  C++ push block 因尾部 16 字节对齐显式补 12 字节 padding，总大小 192。
- **编辑器**：Create 菜单与 Hierarchy 按钮创建地形；Inspector 参数分组、
  Generate / Randomize / Reset / 状态；`CreateTerrainCommand` /
  `SetTerrainConfigCommand` / `RegenerateTerrainCommand` 全部可撤销。
  `DeleteEntityCommand` 快照已包含 TerrainComponent。
- **序列化**：场景 JSON 增加可选 `terrain` 子对象（自身 version 1），只存参数；
  加载时重新生成，旧 v1 场景不回归。
- **验证**：
  - `[Terrain] selfTest PASS | grid=65x65 vertices=221 indices=1152`
  - `SentinelEditor.exe --selftest 5 --empty-scene`：
    `[Editor] scene selfTest PASS | create/undo/redo + terrain create/generate`
  - `VulkanEngine.exe --selftest 25 --empty-scene --terrain-test --screenshot ...`：
    129² 地形可见，`[Validation] errors=0 warnings=0`，stderr 0 字节
  - `tools/Validate.ps1` 新增 `terrain-render` 用例，默认 21 项。
## D-19：地形阶段 6（形态与程序化材质）

- **形态**：新增 `heightCurve` / `islandWeight` / `islandRadius` /
  `ridgeSharpness` / `detailFrequency`；高度曲线与径向岛屿遮罩在
  `TerrainHeightField` 内完成，ridge 指数和 detail 频率进入 fBm。
  新地形默认分辨率从 129² 提到 257²；Inspector 提供 `High Detail 257`
  预设（仍需显式 Generate）。
- **材质**：`mrt.frag` 地形分支按局部高度和坡度重建沙地/草地/岩石/雪权重，
  用旋转 quintic value-noise 做分层 albedo 细节和有限差分微法线；
  屏幕空间 footprint 控制高频细节淡出。`materialDetailStrength` 是
  shader-only 参数，修改后立即生效，不要求重建网格。
- **SSAO 坑**：大面积平滑地形上的 16-tap SSAO 产生明显低频条纹，提高网格
  分辨率只会让条纹变细。修法是把 G-Buffer Normal `.w` 定义为 SSAO policy
  mask：地形写 0，`ssao.frag` 对地形输出无遮蔽；普通几何仍写 1。
  细节见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 32。
- **Serialization**：`terrain.version = 2`，新增形态/材质字段；旧 v1 地形
  缺省字段由 `TerrainConfig` 默认值补齐。
- **验证**：
  - `[Terrain] selfTest PASS` 新增 height curve / island mask 有界性断言；
  - `SentinelEditor.exe --selftest 5 --empty-scene`：编辑器地形命令/生成 PASS，
    `[Validation] errors=0 warnings=0`，stderr 0 字节；
  - `--empty-scene --terrain-test --no-bloom` 截图确认 SSAO 条纹消失；
  - `tools/Validate.ps1` 的 `terrain-render` 继续覆盖 G-Buffer 路径。

## D-20：地形阶段 6 后续（纹理层 + Set 2）

- **纹理资源**：新增 `TerrainTextureGenerator`，启动时确定性生成 4 层
  256² 可平铺 albedo 与 4 层 256² tangent-space normal；沙地/草地/岩石/雪
  各自有独立颜色、颗粒和法线强度。`TerrainMaterial` 用现有 `Texture` 上传
  路径创建 mip/anisotropy 采样器。
- **描述符**：新增专用 Set 2，固定 8 个 fragment sampler（0~3 albedo，
  4~7 normal），不占用 Set 1 bindless 材质槽。`DescriptorManager` 负责 layout、
  set 分配与 `updateTerrainTextures()`；`GeometrySystem` 的 pipeline layout
  绑定 Set0/1/2，`desc binds` 仍为 1。池容量同步为
  `maxSets = maxMaterials + 72`、`COMBINED_IMAGE_SAMPLER = maxMaterials*4 + 128`。
- **Shader**：地形分支按高度/坡度权重采样四层 albedo/normal；albedo 用双尺度
  样本 + patch mask 抑制平铺，normal 混合后投影到世界 XZ 切线基；低频 fBm
  做宏观明暗，roughness 按层加权 0.72~0.96。`outNormal.w=0` 的 SSAO policy
  保持。
- **参数**：新增 `terrainUvTiling` push 字段，push constant 从 192 字节增到
  208 字节；`uvTiling` 与 `materialDetailStrength` 都是 shader-only，
  `TerrainConfig::geometryEquals()` 排除二者，修改后无需 Generate。
- **测试**：`TerrainSelfTest` 增加纹理缓冲尺寸、确定性和非恒定断言；
  `terrain-render` 继续覆盖 257² 上传、Set 2 绑定、G-Buffer 与 SSAO 路径。
- **验证（2026-11-01）**：
  - `[TerrainMaterial] 4 procedural layers ready | albedo+normal 256x256`
  - `[Terrain] selfTest PASS`
  - `[Editor] scene selfTest PASS | create/undo/redo + terrain create/generate`
  - `tools/Validate.ps1 -Incremental -SkipSetup`：21/21 PASS，stderr 全 0 字节，
    `[Validation] errors=0 warnings=0`
- **仍限制**：纹理是程序化生成而非外部美术资产；暂无独立 roughness/height
  map 或材质变体替换 UI。
