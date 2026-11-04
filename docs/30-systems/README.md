---
id: systems
title: 运行时系统索引
type: system
scope: src/Scene, src/Core, src/Terrain
read-when: 修改 ECS/场景、JobSystem、配置、ImGui 面板或程序化地形
size: ~55 行
updated: 2026-11-04
---

# 30-systems 运行时系统

| 文档 | 回答的问题 |
|---|---|
| [scene-ecs](scene-ecs.md) | 场景怎么组织？Transform 层级怎么传播？剔除怎么做？ |
| [jobsystem](jobsystem.md) | 线程池怎么用？有什么限制？ |
| [config](config.md) | 哪些参数可调？配置怎么迁移？ |
| [imgui](imgui.md) | 面板怎么加？为什么文字必须 ASCII？ |
| [terrain](terrain.md) | 程序化地形怎么生成、chunk/LOD 怎么选择与遮缝、存读与限制？ |
| [voxel](voxel.md) | 方块体素怎么存、怎么出网格、怎么挖放、角色怎么站在方块上？ |
| [terrain-materials](terrain-materials.md) | Set2 的 12 个 sampler 怎么排？四套材质集怎么加载/回退？四层 PBR 在 shader 里怎么混？ |
