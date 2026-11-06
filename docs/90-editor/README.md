---
id: editor
title: 场景编辑器
type: system
scope: 编辑器的整体结构、分层边界与面板清单
read-when: 要动编辑器任何部分之前，先读这一页决定该看哪份子文档
size: ~90 行
updated: 2026-11-04
---

# 场景编辑器

编辑器是**独立于引擎的一层**：引擎（`src/RHI`、`src/Render`、`src/Core`）不认识编辑器，
编辑器通过 `EngineServices` 反向调用引擎能力。`--editor` 关闭时这条路径完全不执行。

## 它和引擎的边界

| | 引擎层 | 编辑器层 |
|---|---|---|
| 拥有 | 设备、交换链、渲染图、场景、材质系统 | 选中态、命令栈、日志、文档路径、播放状态 |
| 允许依赖 | 任意 | 只能经 `EditorServices` 拿引擎能力 |
| 碰 GPU | 是 | **否**（要渲染就通过回调请求引擎） |

违反这条边界的典型症状：编辑器里 `#include "RHI/Device.h"`。一旦出现，
以后想把编辑器拆成独立可执行文件就得重写。

## 子文档

| 文档 | 什么时候读 |
|---|---|
| [architecture](architecture.md) | 加新面板 / 改编辑器与引擎的接缝 |
| [commands](commands.md) | 加可撤销操作 / 调试撤销不对 |
| [serialization](serialization.md) | 改场景文件格式 / 加组件要存盘 |
| [prefabs](prefabs.md) | 改预制体保存/实例化 / 子树层级或材质不对 |
| [panels](panels.md) | 写新面板 / 改布局 |
| [viewport](viewport.md) | 改视口尺寸/格式 / 画面拉伸或发暗 |
| [gizmo](gizmo.md) | 点不中物体 / 拖拽方向不对 / 加新手柄 |
| [materials](materials.md) | 新建物体显示成纯白 / 改 G-Buffer 着色 |
| [ux-rules](ux-rules.md) | **改任何用户可见的按钮、提示、错误反馈之前**（判据 U1~U9） |
| [ux-audit](ux-audit.md) | 计划一轮友好性改造前，确认哪些问题还在、优先级如何 |
| [ux-playbook](ux-playbook.md) | 让 AI 执行改造时，作为任务描述的第一份输入 |
| [milestones](milestones.md) | 编辑器做过哪些轮次与每轮基线 |

## 当前能力（D-8 / TD-09）

- 层级树：选中、拖拽重挂、右键删除
- 属性面板：变换 / 网格信息 / 材质 / 渲染标签，变换走命令栈
- 控制台：分级过滤、自动滚动
- 菜单栏：File / Edit / Play / View / Help，状态条显示脏标记与撤销深度
- 播放状态机：Editing / Playing / Paused，Stop 时恢复快照
- 场景存读：`*.scene.json`
- 预制体：选中子树保存为 `*.prefab.json`、实例化到当前场景、撤销/重做（[prefabs](prefabs.md)）
- 离屏视口：3D 画面渲染到纹理，作为 ImGui 图片嵌在面板里（面板环绕而非覆盖）
- 视口点选：AABB 粗筛 + 三角形精确求交，点空白处取消选中
- 选中包围盒：局部 AABB × 世界矩阵的 **OBB**，3D 线段绘制，**带深度测试**（被墙挡住就看不见）
- 占位材质：没有有效材质槽位或没有 albedo 贴图时画程序化黑白棋盘格（完整 UV 走 2D 棋盘，否则局部 3D 棋盘）
- 三轴手柄：平移 / 旋转 / 缩放（`W`/`E`/`R`），旋转用四元数（无万向节死锁），拖拽一次 = 一条可撤销命令
- 资产面板：扫描 `Resources/Models`，双击即通过命令栈创建实体（可撤销）
- 程序化地形：`Create -> Terrain` / Hierarchy 按钮创建；Inspector 改参数、Generate/Reset/Randomize、High Detail 257；创建与重建都走命令栈；地形没有 MaterialComponent，顶点色 + 片元程序化材质层走既有 G-Buffer 路径
- 帮助 → 示例：一键加载预设场景（体素玻璃 / 体素世界 / 程序化地形 / 流送地形），自动替换当前文档并取景；有未保存修改时先走确认框，示例本身不置脏
- 纯 3D 截图：`--screenshot-viewport` 读视口纹理，不含 UI

## 启动

构建会产出**两个可执行文件**，都在 `build/<Config>/` 下：

| 可执行文件 | 模式 | 说明 |
|---|---|---|
| `TinyEditor.exe` | 编辑器 | 双击即进编辑器，等价于 `TinyEngine.exe --editor` |
| `TinyEngine.exe` | 运行时 | 与引入编辑器之前行为完全一致 |

两者共享同一份源码（OBJECT 库），只有 `main.cpp` 各编译一次，
差异只在一个编译期宏 `TINY_DEFAULT_EDITOR`。

```
build\Debug\TinyEditor.exe                    # 直接进编辑器
build\Debug\TinyEditor.exe --selftest 70      # 无头验证编辑器 UI
build\Debug\TinyEngine.exe                      # 运行时模式
```

编辑器版仍接受全部命令行参数（`--screenshot-viewport` 等）。
