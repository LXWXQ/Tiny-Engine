---
id: pipeline
title: 渲染管线索引
type: pipeline
scope: src/Render, shaders
read-when: 修改 Pass 顺序、着色器、G-Buffer 或中间目标
size: ~50 行
updated: 2026-10-30
---

# 20-pipeline 渲染管线

| 文档 | 回答的问题 |
|---|---|
| [pass-order](pass-order.md) | Pass 顺序是什么？为什么是这个顺序？ |
| [gbuffer](gbuffer.md) | G-Buffer 5 个附件的格式与内容？天空盒怎么"借道"？ |
| [intermediate](intermediate.md) | 中间目标有哪些？Bloom 金字塔怎么组织？ |
| [ibl](ibl.md) | IBL 三步烘焙怎么实现？ |
| [shaders](shaders.md) | 全部 shader 清单与编译管线 |

屏障与资源归属不在这一层——见 [architecture/rendergraph](../10-architecture/rendergraph.md)
与 [architecture/resource-lifetime](../10-architecture/resource-lifetime.md)。
