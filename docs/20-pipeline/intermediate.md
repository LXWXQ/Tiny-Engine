---
id: pipeline/intermediate
title: 中间目标、TAA 历史与 Bloom 金字塔
type: pipeline
scope: src/Render/TAAPass.cpp, src/Render/BloomPass.cpp, src/Render/PresentPass.cpp, src/Render/RenderResources.h
read-when: 修改中间目标尺寸/格式、TAA 历史、Bloom 级数、Present 合成方式
size: ~170 行
updated: 2026-11-05
---

# 中间目标、TAA 历史与 Bloom 金字塔

## 1. 中间目标清单

| 目标 | 格式 | 尺寸 | 持有者 | 写 / 读 |
|---|---|---|---|---|
| `ssao` | `R8_UNORM` | 交换链尺寸 x renderScale | 渲染图（`createTexture`） | SSAOPass 写 / SSAOBlur 读写 / Lighting 读 |
| `hdrScene` | `R16G16B16A16_SFLOAT` | 同上 | 渲染图 | Lighting 写 / Glass 读写 / TAA 读 |
| `taaOutput` | 同 `hdrScene` | 同上 | 渲染图 | TAA 写 / Bloom 与 Present 读 |
| `taaHistory[0..1]` | 同 `hdrScene` | 同上 | 渲染图（persistent） | TAA 读上一帧 / 写当前帧 |
| `bloomLevels[0..3]` | 同 `hdrScene` | 1/2, 1/4, 1/8, 1/16 | 渲染图 | BloomPass 内部读写 |
| `bloomResult` | = `bloomLevels[0]` | 1/2 | 渲染图 | Present 采样 |

除 `taaHistory` 外，每张图按 `kFramesInFlight = 2` 各开两份（见
[resource-lifetime](../10-architecture/resource-lifetime.md) §2）；`taaHistory` 本身只有 2 张，
同时充当帧槽与 ping-pong 历史。

## 2. Bloom 金字塔

```
taaOutput (全分辨率)
   |
   v  Bright 阈值提取
level0 (1/2)
   |
   v  Down0
level1 (1/4)
   |
   v  Down1
level2 (1/8)
   |
   v  Down2
level3 (1/16)
   |
   v  Up2  (加法混合)
level2
   |
   v  Up1
level1
   |
   v  Up0
level0  -> bloomResult
```

- 7 个 pass：1 bright + 3 down + 3 up。
- 上采样用 `LOAD_OP_LOAD` + `ONE/ONE` 混合**直接累加回上一级**，不另开累加缓冲
  （见 [decisions](../10-architecture/decisions.md) 决策 9）。
- 因此同一个 Pass 里"既读又写"同一张图——渲染图靠"同一 Pass 内重复访问只保留最后一次声明"处理。

### 2.1 关闭 Bloom 时

**真的不注册这 7 个 pass**（TD-06）。PresentPass 有第二套描述符布局与
`present_no_bloom.frag` 管线，只在 `taaOutput` 上做 tonemap。

验收标记：`[TD06] Bloom disabled: skipping Bright + Down + Up passes`。

## 3. TAA 历史

TAA 有 **2 张历史图**，按帧号奇偶 ping-pong：本帧写 `taaHistory[frame & 1]`，
同时读上一帧那份。历史图是 persistent 的，窗口 resize 时与 `taaOutput`
一起原位重建，重建后 `historyValid = false`，首帧只输出当前颜色。

- 抖动：Halton(2,3) 像素偏移，只加到场景投影矩阵的 `[2][0]/[2][1]`；
  阴影、FrameInfo、ImGui 仍用未抖动投影。
- 反投影：`shaders/taa.frag` 用未抖动 `previousProjectionView` 把
  `G-Buffer.Position` 的世界坐标投到上一帧 UV，不需要运动向量（当前场景只有相机运动）。
- 拒绝：3x3 当前帧颜色 AABB clamp；历史 alpha 存世界半径，半径差超过
  `max(0.05, worldRadius * 0.0025)` 时丢弃历史。
- 关闭：`--no-taa` 时权重 0、抖动 0，Pass 仍要执行（下游 Bloom/Present 绑的是
  `taaOutput`）；重新开启时重置历史。顺序与原因见 [pass-order](pass-order.md) §2–§3。

## 4. tonemap 链（PresentPass）

```
taaOutput (+ bloomResult * intensity)
   -> exposure（来自 GlobalUbo / settings）
   -> ACES Filmic
   -> gamma 1/2.2
   -> 交换链
   -> ImGui 叠加（同一个 vkCmdBeginRendering 作用域内）
```

顺序理由见 [pass-order](pass-order.md) §2。

## 5. 动态分辨率

`renderScale`（0.25~1.0）只影响上面这些中间目标，**交换链尺寸不变**。
管线不重建（动态 viewport/scissor），但渲染图的 owned texture 要原位 resize，
相关描述符视图要重写。

## 6. 改动注意

- **改 Bloom 级数**：必须同步 `DescriptorManager` 池余量（`+64` / `+96`），见
  [descriptors](../10-architecture/descriptors.md) §7。
- **改中间目标格式**：检查 `PresentPass` / `LightingPass` 的 shader 采样类型与
  `Pipeline` 的 `colorAttachmentFormats` 是否一致。
- **改 TAA**：第二个颜色附件必须显式设置 blend state；`currentColor` 必须绑
  `hdrScene`，不能绑 `taaOutput`（会形成 feedback loop）。
  resize 时由 `TAAPass::resize()` 重建历史并置 `historyValid = false`。
- **改尺寸逻辑**：跑 `--selftest 60 --resize-test` 与 `--selftest 20 --render-scale 0.5`。
