---
id: roadmap/terrain-plan
title: 程序化地形实施方案（F2 / D-18）
type: roadmap
scope: TerrainConfig / Noise / HeightField / MeshBuilder / Scene / Editor / Serializer 的落地方案
read-when: 开始地形实现、评审地形方案、或判断地形功能该改哪些文件
size: ~330 行
updated: 2026-11-02
---

# 程序化地形实施方案（F2 / D-18）

> 实现状态（2026-11-02）：阶段 1~5 MVP + 阶段 6 当前范围（形态参数、SSAO 地形策略；4 层程序化 albedo/normal/height/roughness + Set 2；Surface Style）已落地；系统实现与限制见 [terrain](../30-systems/terrain.md)。外部 16 张 PNG 可按 set 目录按需加载，缺图整套回退程序化，仓库不附带美术资产；阶段 7 的 JobSystem 异步、协作取消、SpatialIndex bucket 增量更新、chunk/LOD 子网格、GPU 滞回选层、抖动 cross-fade、拓扑缝合（可选裙边回退）、稳定帧 indirect 合批、CPU 碰撞查询、GPU compute 高度场、独立 chunk 流送、异步高度场、流送交互硬化、拓扑级 LOD stitching 与 GPU 选层/culling 已落地；GPU 网格组装与 4 槽异步上传已完成单 chunk（多 chunk GPU 合批待做），B-07 kinematic 角色控制器与运行期雕刻已落地（雕刻为运行期状态，不写 JSON），B-08 4 个 Set2 材质集/`materialSetIndex` 序列化/按批次重绑已落地，无限世界未做。当前新建地形默认已提升为 257²；本文第 1/2 节中的 129² 是最初 MVP 的历史取值。

## 1. 结论

这份规范**方向可行**，但不能按它建议的类名/目录照搬：本工程已经有成熟的
`Model`/`GeometrySystem`/`SceneDescription`/`CommandStack`，地形必须接在它们上面，
而不是再长一套。

我的评估结论：

- **MVP 采用单块地形、CPU 生成、同步提交、顶点色着色**。先打通
  "创建 → 参数 → 生成 → 视口显示 → 保存/加载 → 删除/撤销"。
- **不**在 MVP 引入材质分层纹理、异步任务、chunk/LOD。这些放在阶段 6/7，
  因为本工程当前的材质是 bindless 纹理数组，给地形加纹理要改描述符分配/更新路径，
  收益远低于先把端到端跑通。
- **不另建 RenderQueue**。本工程的提交入口是 `Scene::visibleEntities` +
  `GeometrySystem`，地形作为普通 `MeshComponent` 实体进入同一条路径。
- **生成在主线程同步执行，分辨率默认 129²、上限 257²**。生成是显式按钮触发的
  低频操作，不是每帧行为；主线程上传 GPU 也符合本工程 "Vulkan 资源只在主/渲染线程创建"
  的约束。阶段 7 已实现 JobSystem/GPU 异步生成与 4 槽 GPU mesh 上传。
- 文档、阶段、验收标准见第 10~12 节；如果确认，从阶段 1（纯 CPU）开始。

## 2. 现状审计（只列与地形有关的事实）

| 项 | 当前事实 | 证据 |
|---|---|---|
| 语言/构建 | C++17、MSVC/VS2022、CMake；源码用 `file(GLOB_RECURSE)`，新增 `.cpp` 不需要改 CMake | `CMakeLists.txt` |
| Vulkan | 原生 Vulkan + 自研 RHI 封装；无 Vulkan-Hpp/volk | `src/RHI/` |
| 数学/坐标 | GLM，Y-up、右手；深度 0..1；变换顺序 `T * quat * S` | `Components.h` |
| 内存 | VMA；`Model` 的 VBO/IBO 为 device-local，经 staging 上传 | `Model.cpp` |
| ECS | EnTT registry；`TransformComponent`/`MeshComponent`/`MaterialComponent`/`RenderTagComponent`/`BoundsComponent` | `Scene/Components.h` |
| 网格 | `Vertex{position,color,normal,uv}`；`Model` 支持 Builder、子网格、CPU 拾取几何 | `Scene/Model.h` |
| 上传线程 | `Model` 构造函数内部提交 staging command 并等待；必须主线程/渲染线程调用 | `Model::uploadViaStaging` |
| 材质 | bindless：Set 1 是 4 个纹理数组 + push constant 的材质下标；没有 albedo 就画棋盘格 | `GeometrySystem.cpp`、`mrt.frag` |
| 渲染 | `Scene::visibleEntities` → `GeometrySystem` 写 G-Buffer；无独立 RenderQueue | `GeometrySystem.cpp` |
| 编辑器 | ImGui 面板；`EditorLayer` 菜单；Hierarchy 有 Create Empty；Assets 双击创建网格 | `EditorLayer.cpp` |
| 撤销 | `CommandStack` + `CreateEntityCommand` / `DeleteEntityCommand` | `Editor/Commands.*` |
| 序列化 | `SceneDescription` 纯数据 + JSON v1；网格只存文件路径 | `SceneDescription.h`、`SceneSerializer.cpp` |
| JobSystem | 有 worker pool、submit、parallelFor；无取消/依赖图；**不在 EngineServices 里暴露** | `Core/JobSystem.*`、`EditorContext.h` |
| 空间索引 | Scene 显式 dirty transform/spatial 集合；只对 dirty bounds 重算世界 AABB，bucket 增量 upsert/remove，destroy 同步移除 | `Scene::refreshSpatialIndex`、`SpatialIndex::insert/remove` |
| 测试 | `--selftest`、`EditorLayer::selfTest`、`Validate.ps1`；没有独立单测框架 | `Application.cpp`、`tools/` |

### 与地形直接相关的调用链

```text
EditorLayer::drawMenuBar / Inspector
        |
        v
Scene (registry, device, commandPool, descriptorManager, materialSystem)
        |
        v
地形 CPU 数据 (config -> heightfield -> vertices/indices/colors)
        |
        v
Model::Builder -> Model (VMA buffers, staging upload)
        |
        v
MeshComponent + BoundsComponent + RenderTagComponent
        |
        v
Scene::visibleEntities -> GeometrySystem -> mrt.frag
```

### 地形真正需要新增的东西

1. 纯 CPU 的地形数据/噪声/网格构建（不能碰 Vulkan）。
2. `TerrainComponent`（参数和生成状态）。
3. `Scene` 上的"创建地形实体 / 重新生成"方法（因为只有 `Scene` 持有 device/commandPool）。
4. `GeometrySystem` 的一个 **terrainMode** 分支和对应 push constant。
5. 编辑器入口、Inspector、命令、序列化扩展。
6. `Terrain::selfTest()`，并挂到现有 `--selftest`。

## 3. 规范逐条评估

| 规范要求 | 本项目现状 | 结论 / 适配 |
|---|---|---|
| 不要另建渲染器/场景/资源系统 | 已有完整 Model/Scene/Command/Serializer | 直接复用；地形只是 `MeshComponent` 的一种来源 |
| 第一版确定性噪声 + fBm | 没有噪声库；GLM/STB 不适合 | 自己实现无全局状态的 2D Perlin + fBm，可单测 |
| 高度场转网格 | `Model` 接受任意顶点/索引 | 用 `Model::Builder`（它会保留 CPU 拾取几何） |
| 顶点法线/UV/索引 | `Vertex` 已有 normal/uv/color | 法线用高度场差分；UV 按世界尺寸；32 位索引 |
| 通过现有 Vulkan 路径渲染 | `GeometrySystem` 固定使用 `mrt.vert/frag` | 同一 pipeline，加 `terrainMode` 分支，不新建 Pass |
| 材质分层 | 只有 bindless 纹理材质；无 albedo 自动棋盘格 | **MVP 用顶点色 + 粗糙电介质**；阶段 6 再做纹理混合 |
| 资产/保存 | 没有 Terrain 资产类型 | 保存参数到场景 JSON；加载时重新生成，不缓存顶点 |
| Inspector 参数 | 已有 ImGui 面板和 `DragFloat` 命令化模式 | 加 Terrain 分组；参数走命令，Generate 走单独命令 |
| 生成/随机种子 | 无 | 新增按钮；随机种子只改 seed，Generate 才重建 |
| 异步生成 | JobSystem 存在但无取消/依赖 | 已用 JobSystem/GPU 高度场 + 版本令牌 + 主线程 poll 安装；4 槽异步 GPU mesh 已落地 |
| 分块/LOD | chunk 子网格、3 级 LOD、拓扑缝合、GPU 选层/culling、独立 chunk 流送、异步高度场、单 chunk GPU 网格与 4 槽异步 GPU mesh 已做 | 无限世界与多 chunk GPU 网格合批留到后续 |
| 撤销/重做 | 命令系统成熟 | `CreateTerrainCommand`、`RegenerateTerrainCommand`、`SetTerrainConfigCommand` |
| 序列化版本 | Scene v1；`SceneDescription` 可加可选字段 | 场景版本保持 1（可选字段向后兼容），terrain 子对象自带 version |

### 需要明确的三个取舍

**取舍 A：MVP 不着色纹理，用顶点色。**

`GeometrySystem` 现在只要实体没有 albedo 贴图就强制画棋盘格，地形如果直接挂普通
`MaterialComponent` 会变成一格一格的黑白地形。要显示草/岩/雪有三种选择：

1. 生成一张 albedo 纹理并更新 bindless 材质槽位 —— 要处理纹理所有权、槽位更新、
   延迟释放，MVP 风险最大；
2. 新增地形专用材质/管线 —— 违背"不另建渲染路径"；
3. **顶点色 + `terrainMode` 分支** —— 复用同一 G-Buffer/光照，零新描述符，
   地形颜色按高度/坡度在 CPU 顶点上混合。

选 3。它不是最终形态，但能立刻得到有光照、有层次的地形，且阶段 6 可以无缝换成
纹理分层（顶点色可保留为 fallback）。

**取舍 B：MVP 同步生成，不引入异步任务状态机。**

`JobSystem` 的 `submit` 没有取消；把生成放到 worker 后，UI 必须每帧轮询结果，
还要处理"旧任务晚到覆盖新地形"、对象销毁、GPU 上传主线程等问题。MVP 先用
显式 Generate + 默认 129²（上限 257²），生成耗时可控；阶段 7 再按真实 profile 决定。

**取舍 C：单块地形，不做 chunk/LOD。**

本工程 `SpatialIndex` 每帧重建、`GeometrySystem` 逐实体提交；单块地形只有 1 个
draw call，正好适合先验证。chunk/LOD 需要全局坐标采样、邻接缝合、增量索引，
是 F2 的规模化阶段，不与 MVP 混做。

## 4. 目标架构

```text
Editor
 ├─ Create 菜单 / Hierarchy 按钮：Create Terrain
 ├─ Inspector：TerrainConfig 分组 + Generate / Randomize / Reset
 └─ Commands：CreateTerrain / SetTerrainConfig / RegenerateTerrain
          |
          v
Scene
 ├─ TerrainComponent { config, generatedConfig, stats, error }
 ├─ createTerrainEntity() / regenerateTerrain() / buildTerrainModel()
 └─ MeshComponent (shared_ptr<Model>) + RenderTag{isTerrain}
          |
          v
Terrain CPU（src/Terrain/，不依赖 Vulkan/Editor）
 ├─ TerrainConfig      纯数据 + 校验
 ├─ TerrainNoise       2D Perlin + fBm（seed 局部化）
 ├─ TerrainHeightField 规则网格采样
 └─ TerrainMeshBuilder 顶点/索引/法线/UV/顶点色
          |
          v
Existing Engine
 ├─ Model::Builder -> VMA buffers + staging
 ├─ GeometrySystem -> terrainMode push constant
 ├─ mrt.vert/frag  -> vertex color + rough dielectric
 └─ SceneSerializer -> terrain 参数（不存 GPU 句柄）
```

### 新增文件（预计）

```text
src/Terrain/TerrainConfig.h
src/Terrain/TerrainNoise.h
src/Terrain/TerrainNoise.cpp
src/Terrain/TerrainHeightField.h
src/Terrain/TerrainHeightField.cpp
src/Terrain/TerrainMeshBuilder.h
src/Terrain/TerrainMeshBuilder.cpp
src/Terrain/TerrainSelfTest.h        (可选：把自检声明单独放，Scene 只调一次)
docs/80-roadmap/terrain-plan.md      (本文)
```

### 修改文件（预计）

```text
src/Scene/Components.h               TerrainComponent / RenderTag::isTerrain
src/Scene/Scene.h / Scene.cpp        create/regenerate/build、生命周期
src/Scene/SceneDescription.h         terrain 可选字段
src/Editor/SceneSerializer.cpp       terrain JSON 读写
src/Editor/Commands.h / Commands.cpp CreateTerrain / SetTerrainConfig / RegenerateTerrain
src/Editor/EditorLayer.h / .cpp      Create 菜单、命令入口
src/Editor/panels/InspectorPanel.*   Terrain 分组
src/Editor/panels/HierarchyPanel.cpp 可选：Create Terrain 按钮
src/Core/FrameInfo.h                 push constant 增加 terrainMode
src/Render/GeometrySystem.cpp       识别 isTerrain；push.terrainMode
shaders/mrt.vert / mrt.frag          terrainMode 分支 + 顶点色
docs/00-meta/doc-map.md             回写映射
docs/30-systems/terrain.md          实现后的系统文档（阶段 3 之后）
docs/90-editor/README.md            能力清单
docs/70-status/milestones.md         D-18
```

## 5. 数据模型

### 5.1 TerrainConfig（纯数据，可序列化）

| 字段 | 默认 | 范围/校验 |
|---|---:|---|
| `seed` | 12345 | 任意 uint32 |
| `resolutionX/Z` | 129 | 2..257；`size_t` 计算顶点/索引 |
| `sizeX/Z` | 100 | >0，≤10000，isfinite |
| `baseHeight` | 0 | isfinite，±10000 |
| `heightScale` | 25 | 0..1000，isfinite |
| `noiseFrequency` | 0.01 | 0.0001..10，isfinite |
| `octaveCount` | 6 | 1..8 |
| `persistence` | 0.5 | 0.05..0.95 |
| `lacunarity` | 2.0 | 1.1..4.0 |
| `ridgeWeight` | 0 | 0..1 |
| `detailWeight` | 0.25 | 0..1 |
| `edgeFalloff` | 0 | 0..1 |
| `uvTiling` | 1 | 0.01..100 |

`TerrainConfig::validate()` 返回可读错误；非法值不允许进入生成器。
所有浮点先 `std::isfinite`；分辨率/顶点/索引用 `size_t` 乘法并检查上限。

### 5.2 TerrainComponent（ECS）

```cpp
struct TerrainComponent
{
    terrain::TerrainConfig config;          // Inspector 编辑的参数
    terrain::TerrainConfig generatedConfig; // 当前网格实际对应的参数
    uint64_t revision = 0;                  // 每次成功生成 +1
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    double lastGenerationMs = 0.0;
    std::string lastError;                  // 最近一次失败原因
    bool generated = false;
};
```

`MeshComponent` 仍然持有 `std::shared_ptr<Model>`；`BoundsComponent.local` 由
`Model::getBoundsMin/Max` 填。**不把 VkBuffer / VmaAllocation 放进 TerrainComponent。**

### 5.3 渲染标签

```cpp
struct RenderTagComponent
{
    bool isSkybox = false;
    bool isGrid = false;
    bool isTerrain = false;   // 新增
};
```

`GeometrySystem` 对 `isTerrain` 的实体：

- 不要求 `MaterialComponent`，不分配材质槽位，不触发"没有材质槽位"警告；
- 设置 `push.terrainMode = 1`；
- 仍然走 `geometryPipeline`、写 4 个 G-Buffer、参与深度/剔除。

## 6. 生成算法

### 6.1 噪声

实现无全局状态的 2D Perlin：

```text
hash(x,z,seed) -> uint32
gradient = 8 个方向之一（由 hash 低位选择）
u = quintic(fade(x))，v = quintic(fade(z))
bilinear(grad·offset) -> [-1,1]
```

`seed` 只在 `hash` 里参与，绝不调用 `rand()`；同 seed/坐标必得同值。
fBm：

```text
sum = 0; ampSum = 0; amp = 1; freq = noiseFrequency;
for i in 0..octaveCount-1:
    n = perlin(x*freq, z*freq, hash(seed, i))
    if ridgeWeight > 0: n = mix(n, 1 - abs(n), ridgeWeight)
    if detailWeight > 0 && i > 0: n *= (1 + detailWeight * ...)
    sum += n * amp; ampSum += amp
    amp *= persistence; freq *= lacunarity
h = 0.5 + 0.5 * sum / max(ampSum, 1e-8)
```

- `ampSum` 不可能为 0（octave>=1, amp=1），仍显式保护。
- 边缘衰减只在 `edgeFalloff > 0` 时生效，且是**可选**的，不偷偷归零。
- 山脊/细节是可选层，不把表达式写成不可调试的一坨；每个参数独立。

### 6.2 高度场

`HeightField`：

```cpp
struct HeightField {
    uint32_t width, height;               // 顶点数
    std::vector<float> heights;           // 0..1
    float at(...), sample(...), min/max;
};
```

世界坐标：

```text
x = -sizeX/2 + i * sizeX/(resX-1)
z = -sizeZ/2 + j * sizeZ/(resZ-1)
y = baseHeight + heightScale * h(i,j)
```

### 6.3 网格

- 顶点数 `resX * resZ`；索引数 `(resX-1)*(resZ-1)*6`。
- 绕序：`(a,b,c)` / `(b,d,c)`，其中 `a=(i,j)`、`b=(i,j+1)`、`c=(i+1,j)`、
  `d=(i+1,j+1)`；当前 pipeline `cullMode=NONE`，但绕序仍按"从 +Y 看逆时针"固定，
  方便以后启用背面剔除。
- 法线（世界间距，不是样本差）：

```text
dhdx = (h[i+1,j]-h[i-1,j]) / (2*dx)
dhdz = (h[i,j+1]-h[i,j-1]) / (2*dz)
normal = normalize(vec3(-heightScale*dhdx, 1, -heightScale*dhdz))
```

边界用单边差分。法线长度必须接近 1，且无 NaN/Inf。

- UV：`u=(x+sizeX/2)/sizeX*uvTiling`，`v=(z+sizeZ/2)/sizeZ*uvTiling`；
  不把 UV 绑定到分辨率。
- 顶点色（MVP）：按归一化高度 + 坡度混合
  沙地 `#c2b280`、草地 `#4f7a3a`、岩石 `#6b6560`、雪 `#e8edf2`；
  高度/坡度阈值固定为常量，阶段 6 再参数化。颜色写线性值，交给 G-Buffer。
- 使用 `Model::Builder`（不要用裸 vector 构造函数）以保留 CPU 拾取几何。

## 7. 渲染与材质

`mrt.frag` 在算出 `outNormal` 之后、材质分支之前：

```glsl
if (push.terrainMode != 0u) {
    outAlbedo = vec4(fragColor, 1.0);
    outPBR = vec4(0.0, 0.95, 0.0, 1.0); // metallic=0, roughness=0.95
    return;
}
```

为什么在 `outNormal` 之后：G-Buffer 法线仍然来自真实地形法线，光照/SSAO 正常。
为什么用 `fragColor`：`mrt.vert` 已把 `Vertex::color` 传到片元，零新描述符。
为什么 roughness 0.95：地形是粗糙电介质，避免默认 metallic=1 把颜色吃掉。

`SimplePushConstantData` 末尾增加：

```cpp
uint32_t terrainMode = 0;
static_assert(offsetof(..., terrainMode) == kMaterialOffset + 48, ...);
static_assert(sizeof(...) == kMaterialOffset + 52, ...);
```

`mrt.vert/frag` 的 push block 同步增加 `uint terrainMode;`，并保留现有
`checkerUvTransform` 的 16 字节对齐。**不要**给地形新增 descriptor binding。

## 8. 编辑器与命令

### 8.1 创建入口

- `EditorLayer` 新增顶层 `Create` 菜单：`Terrain`。
- `HierarchyPanel` 的 `Create Empty` 旁加 `Create Terrain`（可选，二者共用入口）。
- 落点：相机前方 `sizeZ * 0.5 + 10` 处，避免地形中心压在相机上；
  地形高度从 `baseHeight` 开始，默认 0。
- 创建后自动选中，`state.markDirty()`。

### 8.2 Inspector

Terrain 分组（放在 Transform 后）：

```text
Terrain
  Seed [int] [Randomize]
  Resolution X/Z [int 2..257]
  Size X/Z [float]
  Base Height / Height Scale
  Noise: Frequency / Octaves / Persistence / Lacunarity
  Shape: Ridge Weight / Detail Weight / Edge Falloff
  [Generate] [Reset]
  Status: Ready / Generated in 42 ms / <error>
  Vertices / Indices
```

- 控件每次变化都提交 `SetTerrainConfigCommand`（合并窗口），参数可撤销。
- **Generate** 才真正重建网格；不因拖滑块每帧重建。
- **Randomize Seed** 只改 `seed`（并走命令），不自动 Generate；按钮旁边提示
  "Seed changed, press Generate"。
- 生成失败：显示 `lastError`，保留上一次可用网格（`generatedConfig` 不变）。

### 8.3 命令

```text
CreateTerrainCommand
  apply: scene.createTerrainEntity(config, name, spawn)；快照 Mesh/Terrain/RenderTag/Bounds
  revert: 销毁实体，快照保留 Model
  redo: 复用快照，不重新生成

SetTerrainConfigCommand
  old/new TerrainConfig；apply/revert 只改 component.config
  合并窗口按实体合并；absorb 更新目标值

RegenerateTerrainCommand
  old: generatedConfig + Mesh/Bounds/Terrain
  new: config + 新生成的 Mesh/Bounds/Terrain
  apply: 换新网格；revert: 换回旧网格
```

**关键**：`DeleteEntityCommand` 的 snapshot 目前只收 Transform/Name/Mesh/Material/Tag/Bounds，
必须加 `std::optional<TerrainComponent>`，否则删除地形后撤销会丢地形参数。
`RegenerateTerrainCommand` 同时持有新旧 `MeshComponent`（shared_ptr），
这样旧 Model 不会在 GPU 仍可能使用时立刻析构。

## 9. 保存与加载

`EntityDescription` 增加：

```cpp
bool isTerrain = false;
terrain::TerrainConfig terrainConfig;
```

JSON（场景 version 仍为 1；`terrain` 是可选字段）：

```json
{
  "name": "Terrain",
  "translation": [0, 0, 0],
  "rotation": [0, 0, 0],
  "scale": [1, 1, 1],
  "terrain": {
    "version": 1,
    "seed": 12345,
    "resolutionX": 129,
    "resolutionZ": 129,
    "sizeX": 100,
    "sizeZ": 100,
    "baseHeight": 0,
    "heightScale": 25,
    "noiseFrequency": 0.01,
    "octaveCount": 6,
    "persistence": 0.5,
    "lacunarity": 2.0,
    "ridgeWeight": 0.0,
    "detailWeight": 0.25,
    "edgeFalloff": 0.0,
    "uvTiling": 1.0
  }
}
```

原则：

- 不保存顶点/索引/GPU 句柄；只保存参数，加载时重新生成。
- 不保存材质槽位；地形没有 MaterialComponent。
- `Scene::captureDescription` 只写 `terrain.config`；`buildFromDescription` 调
  `Scene::createTerrainEntity` 或 `buildTerrainModel` 重新生成。
- 旧场景（没有 terrain 字段）照常加载。
- 加载失败返回 `errorMessage`，不留下半个场景。
- 保存后重开，参数与地形外观一致（相同 seed/算法/版本）。

## 10. 分阶段实施

### 阶段 0：审计与方案（本文）

交付：现状表、取舍、文件清单、风险。完成条件：你确认第 11 节的取舍。

### 阶段 1：纯 CPU 算法

实现 `TerrainConfig`、`TerrainNoise`、`TerrainHeightField`、`Terrain::selfTest()`。

验收：

- 同 seed/参数两次结果逐元素一致；
- 不同 seed 结果不同；
- 所有高度 finite，范围在约定内；
- 非法参数被拒绝；分辨率上下限；
- `--selftest` 输出 `[Terrain] selfTest PASS`，stderr 0。

### 阶段 2：网格构建

实现 `TerrainMeshBuilder`，接入 `Terrain::selfTest` 的网格断言。

验收：

- 顶点数 = `resX*resZ`；
- 索引数 = `(resX-1)*(resZ-1)*6`；
- 所有索引 < 顶点数；
- 法线长度 ≈1，无 NaN；
- 平坦高度场的法线为 +Y；
- 非正方形尺寸的差分除以真实间距。

### 阶段 3：渲染接入

`Scene::createTerrainEntity`（`Model::Builder`）、`RenderTag::isTerrain`、
`GeometrySystem` 的 `terrainMode`、`mrt.*` 分支、`TerrainComponent`。

验收：

- `--empty-scene` 创建地形后视口可见；
- 光照/深度/SSAO 正常；无验证层错误；
- 地形包围盒正确；
- 删除/重建不崩溃；旧 Model 不在 GPU 使用期被释放。

### 阶段 4：编辑器

Create 菜单、Inspector、三个命令、Randomize/Reset、状态显示。

验收：

- 创建/选择/删除/撤销/重做正常；
- 参数变更不每帧重建；Generate 只点一次；
- 非法参数显示错误且保留旧网格；
- 现有网格创建/材质/场景功能不回归。

### 阶段 5：序列化

`SceneDescription`/JSON/`capture`/`build`/`DeleteEntityCommand` 扩展。

验收：

- 保存→关闭→重开，地形参数与外观一致；
- 旧 v1 场景加载不回归；
- 地形不写顶点/GPU 句柄；
- `EditorLayer::selfTest` 的 roundTrip 覆盖 terrain。

### 阶段 6：材质与形态增强（当前范围已完成）

- 高度/坡度纹理分层：使用专用 Set 2，4 层 albedo + normal + height +
  roughness，不占用 Set 1 bindless 材质槽；
- 高度曲线、岛屿遮罩、更细的 ridge、detail frequency 已落地；
- set 0 由 `TerrainTextureGenerator` 启动时生成，set 1~3 按需从 `setN/` 加载外部 PNG；均可平铺且确定性；
- Inspector 提供 Natural / Arid / Alpine / Lush Surface Style；
- 剩余：可选的 bundled 美术资产；性能/规模化工作进入阶段 7。

### 阶段 7：性能与规模化（进行中）

- [x] JobSystem 异步：不可变 config 快照 + request serial + 主线程上传 +
  旧任务丢弃；Inspector Generate 不再阻塞 CPU 主线程；
- [x] chunk 子网格 + 局部 AABB 逐块视锥剔除；
- [x] 每 chunk 最多 3 级 LOD + CPU 距离选层 + 15% 滞回 + 0.24s 抖动 cross-fade + 拓扑缝合（可选裙边回退）；
- [x] 稳定 LOD 帧的 indirect 合批；
- [x] 保留生成时 HeightField，提供局部/世界高度采样与地形三角形 raycast；
- [x] 独立 chunk 实体 + 相机中心有限半径流送 + manager 序列化模板；
- [x] 流送 GPU 高度场 8 槽异步化 + 描述重建恢复 chunk 集；
- [x] 流送 chunk→manager picking、删除撤销/重做、JSON 文件加载 smoke；
- [x] B-02 拓扑级 LOD stitching：全分辨率 chunk 边界 + 粗内部 + 过渡环带；
- [x] B-03 GPU LOD 选择 / GPU-driven culling：compute 写 indirect command/count；
- [x] B-04 单 chunk GPU 顶点/索引组装 + device-local Model 接管 + GPU/CPU 逐项 parity；
- [ ] 无限世界 / 多 chunk GPU 合批；
- [x] `SpatialIndex` bucket 增量 upsert/remove + Setter 自动 Transform/Bounds dirty 通知 + 增量 AABB + destroy 同步移除；
- [x] 地形任务协作取消：新请求置位 atomic，CPU builder 每行检查；
- [x] 被替换地形 Model 的 4 帧延迟回收队列；
- [x] height/roughness 打包，Set2 从 16 降到 12 个 sampler；GPU compute 高度场生成（CPU/GPU 参考 maxDiff < 5e-4）。
- [x] GPU compute 顶点/索引组装与 GPU 上传（单 chunk；多 chunk CPU fallback）。
- [x] B-05 异步 GPU mesh 提交/回收与帧时间尖峰基线（4 槽 submit/poll + worker picking mirror；257² 主线程 poll 约 1ms；`20261101-222753`）。
- [x] B-07 kinematic 角色控制器（高度场贴地/重力/跳跃/坡度/step/snap）+ 区域限定运行期雕刻 + 编辑器笔刷/Play 驱动 + 角色序列化。
- [x] B-08 地形材质集：4 个 Set2 描述符集 + 按需 `setN` 外部 16 PNG/程序化回退 + `materialSetIndex` 序列化 + 按相邻材质批次重绑。

## 11. 需要你确认的取舍

1. **着色**：MVP 用顶点色（零新描述符，最快打通），阶段 6 再上纹理分层。
   如果第一版就必须是纹理分层，请明确，我会把阶段 6 前移并增加描述符/纹理更新设计。
2. **生成时机**：MVP 同步生成，默认 129²、上限 257²；Explicit Generate。
   如果第一版就必须异步，我会先做 JobSystem 任务状态机，但周期和风险更高。
3. **范围**：第一版单块地形，不做 chunk/LOD；符合 roadmap F2 的"先打通"。

## 12. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| `Model` 析构立即 `vmaDestroyBuffer` | 重新生成可能释放 GPU 在用的旧缓冲 | 命令持有新旧 Model；Scene 另有 4 帧延迟回收队列 |
| 材质没有 albedo 会走棋盘格 | 地形显示成黑白格 | `terrainMode` 分支 + 顶点色；不依赖 MaterialComponent |
| `DeleteEntityCommand` 未快照 Terrain | 删除后撤销丢参数 | snapshot 加 `TerrainComponent` |
| 生成在主线程 | 高分辨率时 UI 卡顿 | 257² 异步重建已落地；同步加载仍阻塞 |
| push constant 布局错位 | 材质/地形颜色全乱 | `offsetof`/`sizeof` 静态断言 + 与 GLSL 同步 |
| registry.destroy 可绕过 Scene 清理；非 chunk 实体仍用统一网格 | 外部扩展误用会产生 Model/空间索引残留 | 生产路径已统一；外部扩展必须走 Scene::destroyEntity，chunk 查询用 region 索引 |
| 场景版本/旧文件 | 旧场景加载失败 | `terrain` 可选字段，缺省不生成；scene version 保持 1 |
| 撤销命令长期持有旧 Model | 显存增长 | 命令历史上限 256；Scene 4 帧延迟回收只覆盖在途 GPU 使用 |

## 13. 最小验收清单（对应规范第 11 节）

- [x] 相同 seed/参数，高度场一致；不同 seed 通常不同。
- [x] 高度/法线 finite；法线长度 ≈1。
- [x] 顶点数、索引数、索引范围、绕序正确。
- [x] 地形通过现有 G-Buffer/光照渲染。
- [x] Create Terrain、Inspector 参数、Generate、Randomize 可用。
- [x] 创建/删除/撤销/重做正常。
- [x] 保存/加载后参数与外观一致。
- [x] 旧场景/普通网格/材质渲染无回归。
- [x] `--selftest`、`tools/Validate.ps1` 全 PASS，stderr 0。
- [x] 文档说明参数、使用方式、已知限制。

## 14. 立即执行的第一步

如果确认第 11 节，下一步只做**阶段 1 + 阶段 2**（不碰 Vulkan/UI）：

- 新增 `src/Terrain/TerrainConfig.h`、`TerrainNoise.*`、`TerrainHeightField.*`、
  `TerrainMeshBuilder.*`；
- 在 `Scene::selfTest()` 里调用 `terrain::selfTest()`；
- 构建、跑 `--selftest`、输出统计；
- 不修改渲染、编辑器、序列化。

这样即使后续渲染/UI 方案要调整，纯 CPU 算法和网格已经可独立验证，不会白做。
