---
id: systems
title: 运行时系统索引
type: system
scope: src/Scene, src/Core, src/Terrain, src/Vegetation
read-when: 修改 ECS/场景、JobSystem、配置、ImGui 面板或程序化地形
size: ~56 行
updated: 2026-11-10
---

# 30-systems 运行时系统

| 文档 | 回答的问题 |
|---|---|
| [scene-ecs](scene-ecs.md) | 场景怎么组织？Transform 层级怎么传播？剔除怎么做？ |
| [jobsystem](jobsystem.md) | 线程池怎么用？有什么限制？ |
| [config](config.md) | 哪些参数可调？配置怎么迁移？ |
| [imgui](imgui.md) | 面板怎么加？为什么文字必须 ASCII？ |
| [terrain](terrain.md) | 程序化地形怎么生成、chunk/LOD 怎么选择与遮缝、存读与限制？ |
| [vegetation](vegetation.md) | 草和树怎么放置、为什么长在水下/陡坡上没有、写进 G-Buffer 的哪一步、怎么调参？ |
| [voxel](voxel.md) | 方块体素怎么存、怎么出网格、怎么挖放、角色怎么站在方块上？ |
| [character](character.md) | 运行模式角色怎么加载、怎么动、骨骼动画怎么切、为什么全黑/悬空？ |
| [water](water.md) | 水面在哪一步画、径向网格怎么跟随相机、波浪/反射/泡沫怎么算、为什么某些场景没有水？ |
| [terrain-materials](terrain-materials.md) | Set2 的 12 个 sampler 怎么排？四套材质集怎么加载/回退？四层 PBR 在 shader 里怎么混？ |
