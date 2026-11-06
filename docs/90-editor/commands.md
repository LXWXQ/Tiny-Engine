---
id: editor/commands
title: 撤销 / 重做
type: architecture
scope: CommandStack 的语义、合并窗口、以及"必须走命令"的硬规则
read-when: 加任何会修改场景的交互；或撤销结果不对、撤销后数据损坏
size: ~160 行
updated: 2026-11-06
---

# 撤销 / 重做

## 硬规则

**任何修改场景的代码都必须经过 `EditorState::commands.execute()`。**

```cpp
// 禁止
registry.get<TransformComponent>(e).translation = x;

// 正确
commands.execute(std::make_unique<SetTransformCommand>(registry, e, oldPos, x, ...));
```

如果先做功能再补撤销，就得把每个功能重写一遍 —— 因为"可撤销"要求操作被建模成对象，
而不是一串直接改数据的语句。这是第一版就要有的原因。

## 命令的写法

统一模式：**保存旧值 + 保存新值，`apply()` 写新值，`revert()` 写旧值。**

不要写增量（`translation += delta`）：撤销时浮点误差会累积，
十几次撤销后物体回不到原位。自检里断言的是**精确相等**，就是为了钉死这条。

## 合并窗口

拖 `DragFloat` 时每帧都会产生命令。如果每次都压栈，一次拖拽要按几百次 Ctrl+Z。

`CommandStack` 用时间窗口（0.35s）+ `mergeWith()` 合并连续同类命令：

```cpp
bool SetTransformCommand::mergeWith(const Command& previous) const
{
    const auto* other = dynamic_cast<const SetTransformCommand*>(&previous);
    return other && other->entity == entity;
}
```

合并时**不再压新命令**，只让上一条重新 `apply()` 一次（它存的是最新目标值）。

## 批量操作：CompositeCommand 与 DuplicateEntityCommand

多选之后，一次用户操作必须是撤销栈里的一步。`CompositeCommand` 把多条子命令打包：
`apply()` 顺序执行，`revert()` **反序**执行（先建的后撤，父子/依赖关系才不会断）。

```cpp
std::vector<std::unique_ptr<Command>> children;
children.push_back(std::make_unique<DeleteEntityCommand>(registry, a, scene));
children.push_back(std::make_unique<DeleteEntityCommand>(registry, b, scene));
stack.execute(std::make_unique<CompositeCommand>("Delete Entities", std::move(children)));
```

`DuplicateEntityCommand` 复制一个实体及其子树：只拷"数据型"组件（`NameComponent` 追加
`" Copy"`、`Transform`、`Mesh`、`Material`、`RenderTag`、`Bounds`、`CharacterController`），
并额外把 `Scene::captureComponentRecords()` 得到的元数据注册组件逐条 apply 到副本；
地形 / 流送 chunk / 相机拒绝复制（`canDuplicate()`）。撤销 = 销毁这次新建的副本，重做会重建一遍
（新 ID，和 `DeleteEntityCommand` 的恢复语义一致）。

`InstantiatePrefabCommand` 走同一个模式，但首次实例化发生在构造函数里（成功才入栈，
因为 `execute()` 没有失败返回值）；undo 逆序销毁创建的实体，redo 重新实例化并产生新 ID。
详见 [prefabs](prefabs.md)。

批量删除前先 `selectionRoots()` 过滤：父节点被同时选中时跳过子节点，否则同一棵子树会被删两次。

## SetTransformsCommand：多对象一条命令

多选拖拽不能用 N 条 `SetTransformCommand` —— 撤销要按 N 次。`SetTransformsCommand`
持有一个 `std::vector<TransformPose>`（每项含实体 + 旧/新的平移、欧拉角、缩放、四元数），
`apply()` / `revert()` 逐项写回并 `markTransformDirty`。

`mergeWith()` 要求**实体列表顺序完全一致**（数量相同、逐个实体相同），这样拖拽过程中的
每帧命令会并进第一条，`absorb()` 只覆盖各项的 `new*` 值，旧值保留为拖拽起点。

## 材质与渲染标记：SetMaterialCommand / SetRenderTagCommand

材质滑杆、Alpha Mode、Skybox / Grid 复选框以前直接写组件 + `markDirty()`，不进命令栈。

- 载荷：`MaterialSnapshot { vec4 baseColor; float metallic; float roughness; float alphaCutoff; int alphaMode; }`；
- `SetMaterialCommand` 的 apply / revert 都走 `writeState()`：写 `Material` 字段，并调 `DescriptorManager::updateMaterialFactors()` 把 factor 推给 GPU —— 撤销也要推，否则 UI 回退了画面没回退；
- `mergeWith` 只比较实体，连续拖滑杆在 0.35 秒窗口内合并成一条；
- `SetRenderTagCommand` 同理，管 Skybox / Grid 两个开关。

## 通用组件：SetComponentRecordCommand

元数据注册组件（`effects.spin` / `effects.orbit` 等）没有专用命令类。Inspector 统一用
`SetComponentRecordCommand(Scene&, entity, type, before, after, action)`：

| before | after | 语义 |
|---|---|---|
| 有 | 无 | 移除组件 |
| 无 | 有 | 添加组件（after 来自 `defaultComponentRecord()`） |
| 有 | 有 | 修改属性 |

- `apply()`：`after` 存在时 `scene.applyComponentRecord()`，否则 `scene.removeComponent()`；
  `revert()` 对称地用 `before`。
- `mergeWith(previous)`：必须是同一实体 + 同一组件 type，且双方 `before`/`after` 都存在；
  拖 `DragFloat` 产生的连续命令因此合并成一条，`absorb()` 只覆盖 `after`，`before` 保留拖拽起点。
- 组件 record 是纯数据值拷贝，不持有组件指针，撤销时不会悬空。
- `DeleteEntityCommand` 快照也带 `components`，覆盖删除后撤销；`DuplicateEntityCommand`
  在 `copyDataComponents()` 之后逐条 apply record，覆盖复制。

Inspector 的 `+ Add Component` 菜单只列 `descriptor.exposed` 且当前未挂载的类型；
移除走组件标题右键菜单。两条路径都 `state.markDirty()` 并经过命令栈。

## 删除实体的撤销（最容易写错的地方）

EnTT 销毁后重建会拿到**新 ID**，所以撤销不能指望 ID 不变。构造时必须存下：

1. 每个实体的组件**值拷贝**
2. 子树的父子关系（用"在快照里的下标"表示，不用实体 ID）

### 坑：捕获 try_get 返回的指针

`registry.try_get<T>()` 返回指向组件存储的**指针**。如果闭包捕获这个指针：

```cpp
// 错的
auto transform = registry.try_get<TransformComponent>(e);
rebuild = [transform](entt::entity created) { registry.emplace<TransformComponent>(created, *transform); };
```

`apply()` 一销毁实体，指针就悬空，`revert()` 解引用就是 UB。
表现是"撤销删除后实体没回来"或读到垃圾数值。

正确做法是用 `std::optional<T>` 做真正的值拷贝（见 `Commands.cpp` 里的 `Snapshot`）。
本项目自检就是靠这条断言抓到这个 bug 的。

## 创建实体：撤销后重做不能重新加载模型

`CreateEntityCommand` 首次 `apply()` 会调 `Scene::createMeshEntity()`，
那是一次真的读盘 + 顶点/索引缓冲上传（Sponza 那种规模要几百毫秒）。

如果每次 redo 都重载，撤销/重做会卡到不可用。所以首次 apply 之后把
`MeshComponent`（内部是 `shared_ptr<Model>`）**快照**下来：

```cpp
if (const auto* mesh = registry->try_get<MeshComponent>(entity))
    meshSnapshot = *mesh;          // shared_ptr 拷贝，GPU 缓冲因此活到命令被丢弃
```

`revert()` 只销毁实体（模型仍被快照持有），`apply()` 再走快照路径重建组件，跳过加载。

## 地形三命令

| 命令 | 首次 apply | revert / redo |
|---|---|---|
| `CreateTerrainCommand` | 调 `Scene::createTerrainEntity`，快照 Mesh/Terrain/RenderTag/Bounds | revert 销毁实体；redo 复用快照，不重新生成噪声/上传 |
| `SetTerrainConfigCommand` | 只写 `TerrainComponent::config` | 精确写回 old/new config；同实体命令在合并窗口内合并 |
| `RegenerateTerrainCommand` | 直接恢复已有 new 快照 | 在 old/new `MeshComponent`（含 `shared_ptr<Model>`）之间切换 |

`RegenerateTerrainCommand` 的 new 状态必须由 Inspector 在
`Scene::requestRegenerateTerrain()` 的成功回调里取得：CPU 由 JobSystem 完成，
主线程上传 Model 后再构造命令。这样生成失败/任务被更新请求 supersede 时不会
入栈，旧网格也不会被释放。场景加载与 `selfTestWithScene()` 仍可使用同步
`Scene::regenerateTerrain()`。

阶段 7 起 `SetTransformCommand` / `DeleteEntityCommand` 接受可选 `Scene*`：
编辑器路径传入 Scene；Transform/Bounds setter 现在通过 owner callback 自动
通知 Scene，命令仍显式 mark 作为兼容路径；`destroyEntity()` 同步移除
SpatialIndex，快照恢复后重新绑定回调。纯 registry 自检可不传 Scene。命令同时持有新旧 Model，撤销历史存在期间旧显存仍存活；Scene 另外对已被替换的 Model 做 4 帧延迟回收，覆盖 GPU 在途帧。`TerrainComponent::generating` 是瞬态标记，所有命令恢复时统一清成 false，避免撤销出一个没有任务却永久 generating 的组件。

`DeleteEntityCommand` 的 snapshot 已包含 `std::optional<TerrainComponent>` 和
`std::optional<TerrainStreamingComponent>`；否则删除地形再撤销会只剩 Mesh，
参数和 generatedConfig 全丢。runtime chunk 被选中时会先解析到 manager。
删除 manager 会级联销毁 chunk；undo 重建 manager 后调用
`Scene::adoptStreamingTerrainManager()` 恢复流送，redo 则销毁 undo 生成的新 ID，
而不是已经失效的原始 ID。

## 空 tag 组件

`CameraComponent` 是空结构体。查存在性必须用 `registry.any_of<T>(e)`，
`try_get<T>()` 对空类型取指针不成立。

## 历史长度

上限 256 条，超出丢弃最老的。每条命令可能持有实体快照，不能无限增长。

## 历史跳转：`CommandStack::jumpTo`

`void jumpTo(size_t targetUndoCount)`：把撤销栈走到指定深度（0 = 全部撤销）。

- 校验：`target > undoCount + redoCount` 时 clamp 到总深度；
- 实现：`while (undoStack.size() > target) undo();` / `while (undoStack.size() < target) redo();`，
  **不整段搬运** —— 命令的 apply/revert 可能有 GPU 副作用（`SetMaterialCommand` 会写材质 factor SSBO），
  跳过中间状态会让缓冲停在半途；
- 调用方：`HistoryPanel` 点击某一行；面板在跳转后自己 `markDirty()` + `transformRefreshPending = true`。
