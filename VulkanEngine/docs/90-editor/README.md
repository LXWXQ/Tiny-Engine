---
id: editor
title: 场景编辑器
type: system
scope: 编辑器的整体结构、分层边界与面板清单
read-when: 要动编辑器任何部分之前，先读这一页决定该看哪份子文档
size: small
updated: 2026-10-30
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
| [panels](panels.md) | 写新面板 / 改布局 |
| [viewport](viewport.md) | 改视口尺寸/格式 / 画面拉伸或发暗 |

## 当前能力（D-8 / TD-09）

- 层级树：选中、拖拽重挂、右键删除
- 属性面板：变换 / 网格信息 / 材质 / 渲染标签，变换走命令栈
- 控制台：分级过滤、自动滚动
- 菜单栏：File / Edit / Play / View，状态条显示脏标记与撤销深度
- 播放状态机：Editing / Playing / Paused，Stop 时恢复快照
- 场景存读：`*.scene.json`
- 离屏视口：3D 画面渲染到纹理，作为 ImGui 图片嵌在面板里（面板环绕而非覆盖）
- 资产面板：扫描 `Resources/Models`，双击即通过命令栈创建实体（可撤销）
- 纯 3D 截图：`--screenshot-viewport` 读视口纹理，不含 UI

## 启动

```
VulkanEngine.exe --editor
VulkanEngine.exe --editor --selftest 70     # 无头验证编辑器 UI
```
