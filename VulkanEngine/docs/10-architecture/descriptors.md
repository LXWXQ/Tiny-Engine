---
id: architecture/descriptors
title: 描述符布局与 bindless 材质
type: architecture
scope: src/Core/DescriptorManager.h, src/Core/DescriptorManager.cpp, src/Render/GeometrySystem.cpp, shaders/mrt.frag
read-when: 修改材质系统、描述符布局、材质槽位分配或回收
size: ~250 行
updated: 2026-10-30
---

# 描述符布局与 bindless 材质

整个引擎只有**两个主描述符布局**（`DescriptorManager`），其余 Pass 用自己的局部布局。

## 1. Set 0 —— 全局（10 个 binding）

一次性构建（`Application` 构造函数里 `buildGlobalSet`），之后所有 Pass 只读。

| binding | 类型 | 内容 | stage |
|---|---|---|---|
| 0 | `UNIFORM_BUFFER` | `GlobalUbo`（proj/view、方向光、相机位置、点光源、`numLights`、`exposure`） | VERTEX \| FRAGMENT |
| 1 | `COMBINED_IMAGE_SAMPLER` | `environmentMap`（等距柱状 HDR） | FRAGMENT |
| 2 | 同上 | `inPosition`（G-Buffer） | FRAGMENT |
| 3 | 同上 | `inNormal` | FRAGMENT |
| 4 | 同上 | `inAlbedo` | FRAGMENT |
| 5 | 同上 | `inPbr` | FRAGMENT |
| 6 | 同上 | `inSSAO` | FRAGMENT |
| 7 | 同上 | `irradianceMap`（samplerCube） | FRAGMENT |
| 8 | 同上 | `prefilteredMap`（samplerCube，5 级 mip） | FRAGMENT |
| 9 | 同上 | `brdfLut` | FRAGMENT |

> 采样器不由渲染图管理（图只管图像）。`ssaoSampler` 来自 `SSAOPass`，
> IBL 三个来自 `IBLBaker` / `CubemapTexture`，G-Buffer 采样器来自 `GBuffer`。
> 代价：`buildGlobalSet()` 必须等这些 Pass 的 `init()` 之后才调，构造顺序因此有硬依赖。

## 2. Set 1 —— 材质（bindless，4 个纹理数组）

| binding | 内容 | 着色器用法 |
|---|---|---|
| 0 | `sampler2D albedoMaps[]` | `texture(albedoMaps[nonuniformEXT(matId)], uv)` |
| 1 | `sampler2D normalMaps[]` | **当前未采样**（见 §5） |
| 2 | `sampler2D metallicMaps[]` | `.b` 取金属度 |
| 3 | `sampler2D roughnessMaps[]` | `.g` 取粗糙度 |

核心变化（D-3）：不再是"每材质一个 set"，而是**一个 set 里 4 个纹理数组**，
材质下标通过 push constant 传入。整帧只绑一次，切材质只改一个整数。

实测：`desc binds` 104 -> **1**；FPS 452 -> 498（+10%）。
**draw call 数不变（104）** —— bindless 省的是绑定与状态切换，不是 draw call。

## 3. 变长数组的规矩

容量 256 槽（`descriptorManager->init(*vulkanDevice, 256)`），长度在**分配时**用
`VkDescriptorSetVariableDescriptorCountAllocateInfo` 指定，布局不用改。

启用 `descriptorIndexing` 需要 Vulkan 1.2 的 5 个次特性，且有 3 条硬性要求
（都被验证层当场拦过）：

| # | 要求 | 漏了会怎样 |
|---|---|---|
| 1 | `descriptorBindingVariableDescriptorCount` 必须开 | 它是**独立于 `runtimeDescriptorArray` 的开关**，少了布局直接被拒 |
| 2 | `VARIABLE_DESCRIPTOR_COUNT_BIT` **只能加在编号最大的 binding** 上 | `VUID-...-pBindingFlags-03004` |
| 3 | 分配长度必须 **<= 布局声明的 `descriptorCount`** | 布局里那个数是**上限**，不是最终长度 |

另一个易错点：**用 `VkPhysicalDeviceFeatures2` 链传特性时，
`VkDeviceCreateInfo::pEnabledFeatures` 必须为 `nullptr`**（`VUID-VkDeviceCreateInfo-pNext-04748`）。
特性链是 `1.3 -> 1.2`。

## 4. 槽位管理

```
allocateMaterialSet(material) -> slot       // 从 freeSlots 栈弹一个
releaseMaterialSet(slot)                    // 进延迟回收队列，不立即归还
kInvalidMaterialSlot = UINT32_MAX           // 哨兵
```

### 4.1 为什么哨兵不是 0

0 是**合法槽位号**。原来默认 0 导致"没有材质的物体"（天空盒、程序化网格）
悄悄绑到第 0 号材质上——只是恰好那槽已分配才没崩。这是典型的"靠巧合工作"。
所以统一用 `UINT32_MAX`（`src/Core/MaterialSlot.h`）。

同理 `GeometrySystem::render` 的 `lastBoundSlot` 初值也是 `kInvalidMaterialSlot`，
保证第一个子网格一定会真正 bind 一次。

### 4.2 延迟回收（TD-04）

bindless 之后"回收再复用"意味着**改写 GPU 可能正在读的数组项**。当前做法：

```
releaseMaterialSet(slot)
  -> 记录"归还时的最后一个已提交 graphics frame"
  -> 每帧查询 timeline 完成值
  -> safeAfterFrame <= completedFrame 时 push 回 freeSlots
```

配合 `descriptorBindingSampledImageUpdateAfterBind` + `UPDATE_AFTER_BIND_POOL`，
允许向已绑定的数组写新槽位。空闲槽不足时只等待最老的可能引用者，不 `vkDeviceWaitIdle`。

## 5. `materialValid` 为什么走 push constant

天空盒 / 程序化网格不采材质数组，它们的槽位可能压根没写过内容。
`descriptorBindingPartiallyBound` 允许数组尾部未绑定，但**采它就是未定义行为**。

所以 push constant 里带一个有效性标记，着色器先看 `push.material.y`，
为 0 就直接输出顶点色 + 默认 PBR。

## 6. 已知浪费：`normalMaps` 未被采样

切线空间法线贴图仍是"有意关闭"状态（`mrt.frag::getNormalFromMap()` 保留待用，
因为模型还不能提供可靠切线）。所以每个材质白占一个描述符 + 一次 `vkUpdateDescriptorSets`，
256 材质 = 256 个无用描述符（占总量 1/4）。

**重新权衡的时机**：启用切线空间法线贴图时（要么模型提供可靠切线，
要么用偏导数构造 TBN 并处理 UV 接缝——那个函数已经写好了）。

## 7. 其他局部 set

| 使用者 | layout 来源 | 内容 | 数量 |
|---|---|---|---|
| `SSAOPass` | `descriptorManager->createSetLayout()` | binding 0 = `SSAOUbo`，binding 1 = 4×4 噪声图 | 1 |
| `BloomPass` | 自己建 | binding 0 = 一张 sampler2D | 7（1 bright + 3 down + 3 up） |
| `PresentPass` | 自己建 | Bloom 开：`hdrScene` + `bloomResult`；关：只有 `hdrScene` | 1（两套布局） |

池容量按这个账算：`maxSets = maxMaterials + 64`，`COMBINED_IMAGE_SAMPLER = maxMaterials*4 + 96`。

> 这两个魔法数（+64 / +96）**与 Bloom 级数耦合**：把 `BloomPass::kLevelCount`
> 从 4 调大很多时要回来同步加余量。

## 8. 检查清单

- [ ] 新 binding 的 stage 和类型与 shader 一致？
- [ ] 变长数组标志只在最大 binding 上？
- [ ] 新槽位是否可能被 GPU 正在读？（需要延迟回收）
- [ ] 描述符集是否按帧槽开了两份？
- [ ] 跑 `--material-stress` 与 `--validation-features gpuav`（后者专抓 bindless 越界）
