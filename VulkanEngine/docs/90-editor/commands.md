---
id: editor/commands
title: 撤销 / 重做
type: architecture
scope: CommandStack 的语义、合并窗口、以及"必须走命令"的硬规则
read-when: 加任何会修改场景的交互；或撤销结果不对、撤销后数据损坏
size: medium
updated: 2026-10-30
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

## 空 tag 组件

`CameraComponent` 是空结构体。查存在性必须用 `registry.any_of<T>(e)`，
`try_get<T>()` 对空类型取指针不成立。

## 历史长度

上限 256 条，超出丢弃最老的。每条命令可能持有实体快照，不能无限增长。
