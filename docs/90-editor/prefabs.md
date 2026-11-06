---
id: editor/prefabs
title: 预制体资产
type: system
scope: 保存选中子树为 .prefab.json、实例化到当前场景、撤销/重做语义
read-when: 做预制体功能、改菜单/序列化、或实例化后层级/材质不对
size: ~110 行
updated: 2026-11-06
---

# 预制体资产

预制体是“一棵可复用的实体子树”。它不引入新的数据模型：

```
选中实体子树 --Scene::captureDescription(root)--> SceneDescription --SceneSerializer--> .prefab.json
.prefab.json --prefab::load--> SceneDescription --Scene::instantiatePrefab--> 当前场景
```

文件格式仍是 v3 场景 JSON，只是约束为**单根子树**。所以预制体文件可以直接当普通场景打开查看，
也可以被场景加载器读取；区别只在编辑器入口是否会清空当前场景。

## 保存

菜单：**文件 → 保存选中为预制体...**（播放中不可用）。

1. 取 Hierarchy 的当前主选中实体作为根。
2. `Scene::captureDescription(root)` 按 `HierarchyComponent::children` DFS 收子树，父一定排在子前。
3. `editor::prefab::validate()` 检查单根、父下标、环与运行期实体。
4. 写盘为 `.prefab.json`。

保存的是**局部变换**（相对父节点）。根自己的局部变换就是预制体的默认摆放。

拒绝保存的内容：

- 相机、地形、流送地形、天空盒、Grid；
- 体素世界（`VoxelWorldComponent`）及其子树；
- 体素 chunk / 地形 chunk（运行期派生实体，本来就不会进描述）。

被拒绝时会弹错误提示，不会写出半个文件。

## 实例化

菜单：**创建 → 实例化预制体...**（播放中不可用）。

1. 打开 `.prefab.json`，`editor::prefab::load()` 做和保存时相同的校验。
2. 先构造 `InstantiatePrefabCommand`，构造函数里真正调用 `Scene::instantiatePrefab()`；
   只有成功才入命令栈并选中新根。
3. 实例化**追加**到当前场景，根挂在场景根下（`parent = entt::null`）；
   需要挂到别的实体下时，实例化后在 Hierarchy 里拖拽即可。

实例化会重新加载网格、重新分配材质槽位、重建 `MaterialComponent`；不会复用源实体的 GPU 句柄。
角色、Bounds、渲染标签与已注册的元数据组件都会恢复。

失败时按已创建实体**逆序销毁**，不触碰场景里原有的实体。

## 撤销 / 重做

- **Undo**：逆序销毁这次实例化创建的全部实体，场景回到实例化前。
- **Redo**：按描述重新实例化，得到**新的实体 ID**；不要缓存上一次的根 ID。
- 首次执行时命令已经由构造函数完成实例化（`adopted` 状态），`apply()` 只负责接管，
  不会重复创建一份；原因是 `CommandStack::execute()` 没有失败返回值，失败命令不能入栈。

## 已知边界

- 只支持单根子树；没有嵌套预制体、预制体变体/覆盖、实例与源资产的联动。
- 不保存体素世界差分（`voxelWorlds`）、地形参数、相机与流送状态。
- 每次实例化都重新分配材质槽位；反复实例化/销毁会消耗槽位（与场景加载一致，见 tech-debt #3）。
- 实例化根固定挂在场景根下，暂不支持“实例化到选中实体下”的菜单选项。
- 材质贴图路径为空时不主动清空已有共享材质的贴图；这是与场景加载一致的既有语义。

## 关键文件

| 文件 | 职责 |
|---|---|
| `src/Editor/Prefab.h/.cpp` | 校验、保存、读取 |
| `src/Scene/Scene.cpp` `captureDescription(root)` | 子树捕获（DFS、跳过运行期 chunk、不写 voxelWorlds） |
| `src/Scene/Scene.cpp` `instantiatePrefab()` | 追加实例化与失败回滚 |
| `src/Editor/Commands.h/.cpp` `InstantiatePrefabCommand` | 撤销/重做 |
| `src/Editor/EditorLayer.cpp` | 两个菜单项与文件对话框 |
| `src/Editor/SceneSerializer.cpp` | v3 JSON 读写 |

## 测试

- 编辑器自检：`[Editor] prefab selfTest PASS | save/load/instantiate/undo/redo`。
- 用例覆盖 cube 父子树 + `effects.spin`：保存、读取、实例化、父子关系、组件值、mesh 计数、
  undo 消失、redo 换新 ID 恢复。
- `tools/Validate.ps1` 的 `e1-batch-commands` 断言该行日志。
