---
id: architecture
title: 架构层索引
type: architecture
scope: src/Core, src/RHI, src/Render, src/Scene
read-when: 需要理解"为什么这样设计"或改动会跨多个子系统时
size: ~70 行
updated: 2026-10-30
---

# 10-architecture 架构层

这一层回答"**为什么**"，不回答"**怎么调**"（后者在 pipeline / systems / build 层）。

| 文档 | 回答的问题 | read-when |
|---|---|---|
| [layering](layering.md) | 四个目录各是什么职责？依赖方向是什么？ | 不确定改动该放哪层时 |
| [rendergraph](rendergraph.md) | 屏障怎么推出来的？子资源/buffer 怎么跟踪？语义校验查什么？ | 改任何 Pass 声明、资源使用 |
| [resource-lifetime](resource-lifetime.md) | 谁创建谁销毁？帧槽位怎么隔离？销毁顺序为什么必须显式？ | 新增任何 GPU 资源 |
| [synchronization](synchronization.md) | 2 帧在飞怎么工作？双队列怎么调度？resize 怎么不崩？ | 碰帧循环、队列、交换链 |
| [gpu-driven-submission](gpu-driven-submission.md) | 几何提交怎么从 CPU 逐实体变成 GPU 剔除 + 间接绘制？实例缓冲里有什么？set 3 干什么？ | 改几何提交路径、接大批量实例、调间接绘制 |
| [descriptors](descriptors.md) | Set0/Set1/Set2 各是什么？bindless 槽位和地形纹理怎么管理？ | 改材质或描述符布局 |
| [world-origin](world-origin.md) | 场景坐标怎么不抖？rebase 时哪些东西必须跟着挪？地形噪声锚点为什么保持绝对？ | 接大世界、排查远距离抖动或地形接缝 |
| [decisions](decisions.md) | 21 条关键取舍，各自放弃了什么 | **质疑现有设计之前必读** |
| [weak-points](weak-points.md) | 现在哪里脆？按影响排序 | 评估改动风险、决定下一步 |

## 层内阅读顺序

如果是第一次接触这个项目：

```
layering  ->  rendergraph  ->  resource-lifetime  ->  synchronization
```

`descriptors` 与 `decisions` / `weak-points` 按需。

## 与其它层的关系

- 具体的 Pass 输入输出 -> [20-pipeline](../20-pipeline/README.md)
- 场景/任务/配置 -> [30-systems](../30-systems/README.md)
- "怎么证明没改坏" -> [50-validation](../50-validation/README.md)
- "为什么这么多坑" -> [60-debug-profiling/pitfalls](../60-debug-profiling/pitfalls.md)

## 不变量（跨文档，改动前必须记住）

这四条被多处依赖，任何一条被破坏都会以"偶发花屏/崩溃"的形式出现：

1. **Pass 只声明访问，不写屏障。** 所有 `VkImageMemoryBarrier2` / `VkBufferMemoryBarrier2`
   由 `RenderGraph` 推导。（见 rendergraph）
2. **下游读的瞬时资源，本帧必须有人写。** 由 `validateWriteCoverage()` 在 `endFrame` 检查。
3. **跨队列资源必须 `crossQueue=true`**，且同一帧只允许单向依赖。
4. **销毁顺序必须显式写在 `~FirstApp`**，不能依赖成员声明顺序。
