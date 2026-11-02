---
id: architecture/weak-points
title: 架构薄弱点
type: architecture
scope: 空（跨代码库）
read-when: 评估改动风险、或决定"下一步该修什么"
size: ~90 行
updated: 2026-11-01
---

# 架构薄弱点（按影响排序）

只列**当前仍然成立**的。已修的在 [milestones](../70-status/milestones.md)，
需要排期修的在 [tech-debt](../70-status/tech-debt.md)。

| # | 薄弱点 | 影响 | 什么时候必须处理 |
|---|---|---|---|
| 1 | 跨队列资源走 `CONCURRENT` sharing，未实现 EXCLUSIVE ownership transfer | 无法做细粒度的队列所有权转交，带宽上不是最优 | 接入 GPU culling / 体素间接缓冲时 |
| 2 | glTF 几何烘焙到世界空间，丢失节点层级 | Sponza 内部节点不能单独驱动；阻塞局部动态物体与实例化 | 资产管线升级 / 地形体素前 |
| 3 | ECS 销毁契约仍靠约定：`registry.destroy()` 可绕过 Scene 清理；区域索引只覆盖 streaming terrain chunk，其他实体仍用统一网格 | 外部扩展误用会留下 Model/空间索引残留；非 chunk 查询仍是全局网格 | 编辑器插件 / 外部扩展时 |
| 4 | `LightingPass` 兼任"导入交换链" | Pass 职责不对称，init 顺序有隐式依赖 | 清理窗口期 |
| 5 | 自检开关只覆盖 bloom / ssao | 新增可关闭 Pass 时那条分支是验证盲区 | 加新 Pass 时照 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 14 做 |
| 6 | `Application` 构造函数约 120 行、`Application.h` 包含过多 | 不可单独测试，改动易踩顺序依赖 | 重构窗口期 |
| 7 | `FrameInfo` 成员混用引用与裸指针，无成文规则 | 隐式耦合，易误用 | 同上 |
| 8 | `Swapchain::imageCount` 与 ImGui `MinImageCount=2` 不一致 | 目前不触发（ImGui 只在小于时报错） | 改交换链创建参数时 |
| 9 | `Pass::writeDepth` 在 `depth==0 && stencil==0` 时静默改成 `{1.0, 0}` | 真想清成 0 的调用方会被坑 | 引入阴影贴图时 |
| 10 | JobSystem 只有全局队列，无 work stealing / 依赖图 / 核心取消令牌 | 大量短任务时负载不均；地形目前是 Scene 级协作取消 | 地形/体素接入后按 profile 升级 |
| 11 | bindless `normalMaps` 绑了但没采样 | 每材质白占 1/4 描述符 | 启用切线空间法线贴图时 |

## 未被验证覆盖的路径

这类问题比"已知 bug"更危险，因为**没有任何自动化会报**：

1. **正常关窗退出路径**：`tools/Validate.ps1` 用的是 `--selftest` 正常退出，覆盖了析构；
   但交互式关窗（用户点 X）没有被单独回归。历史 bug（VMA 断言）就藏在这条路径上。
2. **窗口必须在前台**：截图类测试若窗口没到前台会截到桌面，数字"看着正常"但毫无意义。
   `tools/Screenshot.ps1` 做了显式检查并在失败时丢弃结果，但这是脚本级的，不是引擎级的。
3. **长时间运行**：所有回归都是 120 帧以内，没有跑过小时级稳定性。
