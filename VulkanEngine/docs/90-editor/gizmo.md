---
id: editor/gizmo
title: 视口交互：点选与三轴手柄
type: system
scope: 射线拾取、Gizmo 的坐标系约定、拖拽如何换算成变换
read-when: 改视口交互 / 点不中物体 / 拖拽方向不对 / 加新的手柄
size: medium
updated: 2026-10-30
---

# 视口交互

两件事：**点选**（射线拾取）和**三轴手柄**（平移/旋转/缩放）。

## 点选

鼠标位置 → NDC → 逆 `projection*view` → 世界射线 → **三角形级求交**，取最近命中。

### 为什么不能用 AABB

一开始用的是 ray-AABB，结果"点任何东西都选中 Sponza"。

原因：Sponza 整栋是**一个实体**，它的世界 AABB 把相机包在里面 ——
射线与 AABB 的入射距离恒为 0，于是它永远是最先命中的那个，
连点在它前面的球也一样。

这不是阈值没调好，是 **AABB 在"相机位于物体内部"时完全失效**。

所以现在是两级：

```
AABB 粗筛（绝大多数实体一次比较就排除）
    ↓ 通过
Möller-Trumbore 三角形求交（精确）
```

`Model` 保留了一份 CPU 侧几何（只存位置 + 索引，Sponza 约 10MB），
求交时把**射线**变换到模型局部空间（只乘一次逆矩阵），而不是把几十万顶点
变换到世界空间。

点空白处取消选中。天空盒必须跳过 —— 它是相机跟随的巨球，永远包住射线。

## 选中包围盒：必须走 3D，不能用 ImGui 画

选中实体的**世界 AABB** 画成白色 12 边线框。

### 为什么不能用 ImGui 的 draw list

ImGui 的 draw list 是**纯 2D 叠加，没有深度概念** —— 框会穿透墙体永远画在最前面。
物体被挡住时框还看得见，完全误导人（用户第一反应就是"这个不对"）。

所以包围盒改由 `EditorOverlayPass` 当**真正的 3D 线段**画：

| 项 | 做法 |
|---|---|
| 拓扑 | `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`，12 条边 = 24 顶点 |
| 顶点 | 世界空间坐标（AABB 本来就在世界空间算好，不需要 model 矩阵） |
| 深度附件 | **复用 G-Buffer 的深度图**，`LOAD_OP_LOAD` + 管线里 `depthWriteEnable = VK_FALSE` |
| 颜色目标 | `editorViewport`，`LOAD_OP_LOAD`（保住 PresentPass 的画面） |
| 顺序 | PresentPass 之后、UiCompositePass 之前 |

「只测不写」是关键：深度图后面还要被其它东西用，不能被线框污染。

### 手柄仍然画在最前面（刻意）

Gizmo 手柄还是 ImGui 叠加。这是有意的 —— 即使物体被挡住，你也得能抓住手柄。
主流编辑器（Unity / Unreal）也是这个行为。

### 为什么用世界 AABB 而不是模型 OBB

它每帧已经在 `Scene::rebuildSpatialIndex()` 里算好（零额外开销），
而且它同时是拾取用的粗筛体 —— 看到的框和"能点到的范围"是同一个东西。

## 为什么拖拽时包围盒会和物体分开（已修）

UI 是在 `render()` 里构建的，而 `Scene::update()`（负责
`updateWorldTransforms` + `rebuildSpatialIndex`）在 `update()` 里**已经跑完了**。

所以 gizmo 拖拽改完 `TransformComponent` 之后，`WorldTransformComponent` 与
`WorldBoundsComponent` 还停在上一帧 —— 白框和手柄会落后物体一帧。

**这里有两个坑，而且是叠加的**，只修一个都还是会分离。

### 坑一：刷新时机

改完 transform 后必须**当场**刷新，不能推迟到帧末 —— 推迟的话本帧绘制
仍然用旧包围盒。

```cpp
state.services.scene->refreshTransformsAndVisibility();
origin = refreshedBounds->world.center();       // 手柄锚点
localAxes[0..2] = 归一化的世界矩阵列向量;        // 旋转后轴要跟着转
```

### 坑二：绘制顺序（更隐蔽）

刷新对了还不够。原来的绘制顺序是：

```
画包围盒  ← 用旧包围盒
gizmo.manipulate()  ← 改 transform 并刷新
```

框在**交互之前**就画完了，所以照样落后一帧。

修法是把 gizmo 拆成两步，让调用方按正确顺序走：

```
gizmo.interact()   ← 命中测试 + 拖拽 + 立刻刷新包围盒
画包围盒            ← 这时包围盒已经是最新的
gizmo.draw()       ← 手柄最后画，压在框上面
```

`Gizmo` 因此提供 `interact()` / `draw()` 两个方法，`interact()` 把锚点、轴、
手柄长度缓存到成员里供 `draw()` 使用。

回归守卫：`selfTestWithScene()` 里有一条断言"沿 +Y 平移 3 个单位后，
包围盒中心也必须移动 3 个单位"。

## 手柄中心对齐包围盒中心

手柄中心取 `WorldBoundsComponent.world.center()`，没有包围盒时退回物体原点。

为什么不用原点：很多模型的几何原点在脚底或角上（Sponza 的原点在地面中心），
手柄画在原点会离物体很远，看起来像"没对齐"。用户期望的是手柄落在
物体"看起来的中心"。

**已知偏差**：旋转手柄视觉上以包围盒中心为圆心，但实际旋转仍绕物体原点
（因为 `TransformComponent` 的 rotation 是绕自身原点的欧拉角）。
父节点带非均匀缩放时更明显。

## 坐标系约定（最容易搞错的地方）

| 模式 | 手柄画在 | 为什么 |
|---|---|---|
| 平移 | **世界轴** | "往上拖就是世界往上"，符合直觉 |
| 旋转 | 实体**局部轴** | `rotation` 是欧拉角，用局部轴才不会"拖 X 结果斜着转" |
| 缩放 | 实体**局部轴** | `scale` 本身就是局部的 |

平移时世界位移要折算回**父节点局部空间**：

```cpp
const glm::mat4 invParent = glm::inverse(parentWorld->matrix);
localDelta = glm::vec3(invParent * glm::vec4(worldDelta, 0.0f));
```

不做这一步，父节点带旋转时"往世界 X 拖"会变成"往父节点的 X 拖"。

## 拖拽换算

**平移 / 缩放**：用"射线与轴线的最近点"求沿轴参数（`closestPointOnAxis`），
比"投影到屏幕再算像素差"更稳 —— 后者在轴几乎正对相机时精度崩掉。

**旋转**：射线与"垂直于轴的平面"求交，取交点相对圆心的极角差。

```cpp
const float angle = std::atan2(glm::dot(v, dragPlaneV), glm::dot(v, dragPlaneU));
float delta = angle - startAngle;
// 折到 [-PI, PI]，否则绕一圈会突然跳 2PI
```

## 屏幕坐标 ↔ NDC：Y 方向只能翻一次

引擎的投影矩阵里有 `projectionMatrix[1][1] *= -1`（Vulkan 的 NDC Y 向下），
所以 **NDC +Y 指向屏幕下方**。

投影与反投影都必须直接用 `ndc*0.5+0.5`，**不能**再套一层 `1.0f - ...`：

```cpp
// 投影
outScreen.y = rectMin.y + (ndc.y * 0.5f + 0.5f) * height;
// 反投影
ndcY = ((screen.y - rectMin.y) / height) * 2.0f - 1.0f;
```

写错的后果：包围盒与手柄相对真实渲染**垂直镜像** —— 物体往上移，白框往下移，
看起来就是"两者方向相反"；拾取射线也会镜像，点 A 选中 B。

**自检必须断言方向**（往返测试抓不到这个，因为两次翻转会互相抵消）：

```cpp
if (!(higher.y < lower.y)) fail;   // 世界 +Y 必须投影到更小的屏幕 y
if (!(right.x > left.x)) fail;     // 世界 +X 必须投影到更大的屏幕 x
```

详见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 28。

## 手柄恒定屏幕尺寸

手柄长度按相机距离换算，保证屏幕上恒定 ~90px：

```cpp
const float worldPerPixel = 2.0f * tanHalfFov * distance / viewportHeight;
const float handleLength = kHandlePixels * worldPerPixel;
```

**坑**：Vulkan 的投影矩阵把 Y 轴翻转（NDC Y 向下），所以 `projection[1][1]`
是**负数**。直接取倒数会得到负的 `tanHalfFov`，手柄长度算成负值被 `std::max`
钳到 1e-4 —— 表现是"三个轴箭头全叠在中心一点"。

```cpp
const float tanHalfFov = std::abs(1.0f / view.projection[1][1]);   // 必须取绝对值
```

这个 bug 只能靠**看截图**发现（没有任何运行时错误），见
[pitfalls](../60-debug-profiling/pitfalls.md) 坑 26。

## 撤销

每一帧拖拽都提交一条 `SetTransformCommand`，靠 `CommandStack` 的合并窗口
（0.35s，按实体合并）把整个拖拽压成**一条**命令 —— 撤销一次回到拖拽前，
而不是退回一帧。

## 输入优先级

```
gizmo.manipulate()  -> 返回 true 表示手柄吃掉了这次点击
    ↓ false
点选（ray pick）
```

顺序不能反：否则拖手柄的同时会顺手改掉选中。

## 相机输入门控

`CameraController` 直接用 `glfwGetKey` / `glfwGetMouseButton` 读输入，
**完全不知道 ImGui 正在捕获键鼠**。不加门控的话：

- 在资产面板的过滤框里敲 "wasd"，相机会跟着跑；
- 拖 gizmo 时相机会同时转。

所以 `Scene::update` 多了一个 `allowCameraInput`：

```cpp
const bool looking = RMB 或 MMB 按下;
allowCameraInput = (viewportHovered || looking) && !ImGui::GetIO().WantTextInput;
if (gizmo.isDragging()) allowCameraInput = false;
```

`looking` 这一项是必要的：拖拽视角时鼠标可能移出视口矩形，
只按 hover 判断会让视角"拖一半就停"。

禁用时控制器**仍然把内部状态写回 transform**（而不是直接 return），
否则相机会停在上一帧的平滑中间值上。

## 新建物体生成在相机前方

资产面板双击创建实体时，落点是 `相机位置 + 前向 * 4`，不是世界原点。

相机离原点可能很远，落在原点会让用户以为"没生成"。

前向公式必须和 `CameraController::cameraForward` 一致（这套相机的投影把
view-space +Z 当作前方，所以世界前向是 +w 而不是 -w）。

**注意**：`Models/sphere.obj` 是**半径 1000 的天空盒球**，不是普通小球。
想放小球用 `Quad-Sphere.obj` 或 `debug_cube.obj`。

## 调试单个物体：`--empty-scene`

```
build\Debug\SentinelEditor.exe --empty-scene
```

不加载 Sponza / 天空盒，只留相机，然后从 Assets 面板双击加物体。

为什么需要它：Sponza 整栋是一个实体、包围盒包住整个场景，会干扰
"点选 / 包围盒 / 手柄"这类交互的观察 —— 画面里同时有一个巨大的白框
和一堆几何体，很难判断某个框到底跟没跟上物体。

## 快捷键

| 键 | 模式 |
|---|---|
| `W` | 平移 |
| `E` | 旋转 |
| `R` | 缩放 |

也可以在 `Edit` 菜单里切换。当前模式显示在菜单栏右侧状态条与视口左上角。

## 已知限制

- 只有单轴约束。没有平面手柄（XY/YZ/XZ）和屏幕空间手柄。
- 旋转直接改对应欧拉分量，父节点带非均匀缩放时会与视觉不完全一致。
- 拾取用 AABB，细长/镂空物体的选中范围会比视觉略大（例如 Sponza 整栋是一个实体）。
