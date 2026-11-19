---
id: architecture/descriptors
title: 描述符布局与 bindless 材质
type: architecture
scope: src/Core/DescriptorManager.*, src/Render/TerrainMaterial.*, src/Terrain/TerrainTextureGenerator.*, src/Render/GeometrySystem.cpp, shaders/mrt.frag, shaders/water.frag
read-when: 修改材质/地形纹理系统、描述符布局、材质槽位分配或回收；或判断要不要上 push descriptor / descriptor buffer
size: ~219 行
updated: 2026-11-19
---

# 描述符布局与 bindless 材质

`DescriptorManager` 管理**三个主描述符布局**（Set 0 全局、Set 1 bindless 材质、Set 2 地形纹理），其余 Pass 用自己的局部布局。

## 1. Set 0 —— 全局（15 个 binding）

启动时构建（`Application::buildGlobalSet`），反射/SSR 开关变化时重建；之后所有 Pass 只读。

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
| 10 | `STORAGE_BUFFER` | 材质 factor 表（按材质槽位索引） | FRAGMENT |
| 11 | `COMBINED_IMAGE_SAMPLER` | 3 级联方向光阴影图（`sampler2DArray`） | FRAGMENT |
| 12 | 同上 | 半分辨率平面反射颜色，关闭时回退环境图 | FRAGMENT |
| 13 | 同上 | 半分辨率水面 SSR 结果，关闭时回退环境图 | FRAGMENT |
| 14 | 同上 | 1024x1024 无缝水面法线，世界 XZ 两层平铺 | FRAGMENT |

> 采样器不由渲染图管理（图只管图像）。`ssaoSampler` 来自 `SSAOPass`，
> IBL 三个来自 `IBLBaker` / `CubemapTexture`，G-Buffer 采样器来自 `GBuffer`。
> 代价：`buildGlobalSet()` 必须等这些 Pass 的 `init()` 之后才调，构造顺序因此有硬依赖。

## 2. Set 1 —— 材质（bindless，4 个纹理数组）

| binding | 内容 | 着色器用法 |
|---|---|---|
| 0 | `sampler2D albedoMaps[]` | `texture(albedoMaps[nonuniformEXT(matId)], uv)` |
| 1 | `sampler2D normalMaps[]` | 模型材质仍**未采样**；地形 normal 走 Set 2（见 §3/§7） |
| 2 | `sampler2D metallicMaps[]` | `.b` 取金属度 |
| 3 | `sampler2D roughnessMaps[]` | `.g` 取粗糙度 |

核心变化（D-3）：不再是"每材质一个 set"，而是**一个 set 里 4 个纹理数组**，
材质下标通过 push constant 传入。整帧只绑一次，切材质只改一个整数。

实测：`desc binds` 104 -> **1**；FPS 452 -> 498（+10%）。
**draw call 数不变（104）** —— bindless 省的是绑定与状态切换，不是 draw call。

## 3. Set 2 —— 地形四层纹理（16 个固定 binding）

阶段 6 后续新增。地形每帧一次 draw 需要同时访问四组 albedo/normal，
但现有 `Texture` 是单张 2D 纹理，没有 texture array 接口；因此没有把它们
硬塞进 bindless 材质槽，而是新增一个专用的固定布局：

| binding | 类型 | 内容 |
|---|---|---|
| 0~3 | `COMBINED_IMAGE_SAMPLER` | 沙地 / 草地 / 岩石 / 雪 albedo |
| 4~7 | `COMBINED_IMAGE_SAMPLER` | 沙地 / 草地 / 岩石 / 雪 normal |
| 8~11 | `COMBINED_IMAGE_SAMPLER` | 沙地 / 草地 / 岩石 / 雪 packed height(R)+roughness(G) |

- `TerrainTextureGenerator` 生成 16 张 256² CPU 源纹素；`TerrainMaterial`
  把每层 height/roughness 打包成一张 RGBA，GPU 只上传 12 张纹理；
- `DescriptorManager::createTerrainLayout()` 声明 16 个 binding（0~11 被 `mrt.frag`
  使用，12~15 预留），12 个 fragment sampler 全部带
  `UPDATE_AFTER_BIND | PARTIALLY_BOUND`，布局带 `UPDATE_AFTER_BIND_POOL`（描述符池
  同样开了该标志）——缺一项，运行期重写就会违反
  `VUID-vkUpdateDescriptorSets-None-03047`；
- `updateTerrainTextures()` 在 set 0 初始化时写一次，之后每次
  `TerrainMaterial::uploadMaterialSet()` 切换材质集都会重写同一组 binding，这正是
  必须开 UPDATE_AFTER_BIND 的原因；
- `GeometrySystem` 的 pipeline layout 现在包含 `{Set0, Set1, Set2}`，
  每帧一次 `vkCmdBindDescriptorSets(..., 3, sets, ...)`，`desc binds` 仍是 1；
- 地形实体仍然没有 `MaterialComponent`，不消耗 bindless 槽位；
- Surface Style 只改 shader 权重/色调，不重建 12 张 GPU 纹理。

与 Set 1 分离的收益是：不会破坏材质数组的变长描述符规则，也不会让地形
纹理更新和 bindless 槽位回收互相干扰。代价是 shader 必须固定知道有 4 层。

## 4. 变长数组的规矩

容量 1024 槽（`descriptorManager->init(*vulkanDevice, 1024)`），长度在**分配时**用
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

## 5. 槽位管理

```
allocateMaterialSet(material) -> slot       // 从 freeSlots 栈弹一个
releaseMaterialSet(slot)                    // 进延迟回收队列，不立即归还
kInvalidMaterialSlot = UINT32_MAX           // 哨兵
```

### 5.1 为什么哨兵不是 0

0 是**合法槽位号**。原来默认 0 导致"没有材质的物体"（天空盒、程序化网格）
悄悄绑到第 0 号材质上——只是恰好那槽已分配才没崩。这是典型的"靠巧合工作"。
所以统一用 `UINT32_MAX`（`src/Core/MaterialSlot.h`）。

同理 `GeometrySystem::render` 的 `lastBoundSlot` 初值也是 `kInvalidMaterialSlot`，
保证第一个子网格一定会真正 bind 一次。

### 5.2 延迟回收（TD-04）

bindless 之后"回收再复用"意味着**改写 GPU 可能正在读的数组项**。当前做法：

```
releaseMaterialSet(slot)
  -> 记录"归还时的最后一个已提交 graphics frame"
  -> 每帧查询 timeline 完成值
  -> safeAfterFrame <= completedFrame 时 push 回 freeSlots
```

配合 `descriptorBindingSampledImageUpdateAfterBind` + `UPDATE_AFTER_BIND_POOL`，
允许向已绑定的数组写新槽位。空闲槽不足时只等待最老的可能引用者，不 `vkDeviceWaitIdle`。

## 6. `materialValid` 为什么走 push constant

天空盒 / 程序化网格不采材质数组，它们的槽位可能压根没写过内容。
`descriptorBindingPartiallyBound` 允许数组尾部未绑定，但**采它就是未定义行为**。

所以 push constant 里带一个有效性标记，着色器先看 `push.material.y`，
为 0 就直接输出顶点色 + 默认 PBR。

## 7. 模型材质 `normalMaps` 仍未采样

切线空间法线贴图仍是"有意关闭"状态（`mrt.frag::getNormalFromMap()` 保留待用，
因为模型还不能提供可靠切线）。所以每个材质白占一个描述符 + 一次 `vkUpdateDescriptorSets`，
256 材质 = 256 个无用描述符（占总量 1/4）。

**重新权衡的时机**：启用切线空间法线贴图时（要么模型提供可靠切线，
要么用偏导数构造 TBN 并处理 UV 接缝——那个函数已经写好了）。

## 8. 其他局部 set

| 使用者 | layout 来源 | 内容 | 数量 |
|---|---|---|---|
| `SSAOPass` | `descriptorManager->createSetLayout()` | binding 0 = `SSAOUbo`，binding 1 = 4×4 噪声图 | 1 |
| `BloomPass` | 自己建 | binding 0 = 一张 sampler2D | 7（1 bright + 3 down + 3 up） |
| `PresentPass` | 自己建 | Bloom 开：`hdrScene` + `bloomResult`；关：只有 `hdrScene` | 1（两套布局） |

池容量按这个账算：`maxSets = maxMaterials + 72`，
`COMBINED_IMAGE_SAMPLER = maxMaterials*4 + 144`。地形占用其中 1 个 set 和
12 个固定 sampler；余量留给 SSAO/Bloom/诊断等局部 set。

> 这两个魔法数（+72 / +144）**与 Bloom 级数、地形 set 耦合**：新增固定
> descriptor set 或把 `BloomPass::kLevelCount` 调大很多时要回来同步加余量。

## 9. 检查清单

- [ ] 新 binding 的 stage 和类型与 shader 一致？
- [ ] 变长数组标志只在最大 binding 上？
- [ ] 新槽位是否可能被 GPU 正在读？（需要延迟回收）
- [ ] 描述符集是否按帧槽开了两份？（Set 2 terrain 是场景级单份、只读）
- [ ] 使用新纹理前是否已经调用 `updateTerrainTextures()`？
- [ ] 跑 `--material-stress` 与 `--validation-features gpuav`（后者专抓 bindless 越界）

## 10. 什么时候该往上走一档（TM-11）

这一节记的是**升级的触发条件**，不是计划：当前实现够用，出现下面表里的信号时再动。

### 10.1 现在的量（2026-11-19 实测）

| 项 | 数量 |
|---|---|
| `vkCreateDescriptorSetLayout` | 16 |
| `vkCreateDescriptorPool` | 10 |
| `vkAllocateDescriptorSets` | 11 |
| `vkUpdateDescriptorSets` | 31 |
| `VK_DESCRIPTOR_TYPE_UPDATE_AFTER_BIND` | 0 |
| `vkCreateDescriptorUpdateTemplate` | 0 |
| `vkCmdPushDescriptorSet` | 0 |
| `VK_EXT_descriptor_buffer` | 0 |
| `vkCreateSampler` | 20 |

`vkUpdateDescriptorSets` 的分布是：DescriptorManager 6、SceneGpuBatch 5、SSAOPass 3、GeometrySystem 3、PresentPass 2、BloomPass 2，其余 10 个文件各 1。**但要按调用频率看**：`SceneGpuBatch::writeDescriptors` 只在容量翻倍或 G-Buffer 重建时跑；真正每帧都写的只有几处 —— BloomPass 每帧 3 次（约 7~9 个 binding）、Cloud / FogVolume / SSR / SunFlare 各 1 次、SSAOPass 的模糊通道 1 次、PresentPass 按 bloom 开关 1 次、TAA 1 次。合计每帧 12~15 次 `vkUpdateDescriptorSets`、每次 1~2 个 binding。

### 10.2 三档阶梯与各自的触发条件

| 档 | 做法 | 值得做的信号 | 成本 |
|---|---|---|---|
| 池化 set + 环形分配 | 每线程每帧一个 pool，set 从环里取（参考 `VULKAN_DESCRIPTOR_RING_SIZE = 16`、`VULKAN_NUM_SETS_PER_POOL = 64`） | 每帧**新建** set 的组合数 > 16，或 `vkAllocateDescriptorSets` 每帧 > 32 次 | 1~2 天 |
| push descriptor | `vkCmdPushDescriptorSet` 把 1~2 个 binding 直接推进命令缓冲，不碰 pool | 每帧重写 > 200 次，且每次只改 1~2 个 binding | 2 天 |
| descriptor buffer | `VK_EXT_descriptor_buffer`，描述符当内存写，set 退化成地址 | 同一批视图要按物体反复打包，或要让 GPU 自己写描述符（bindless 动态增长） | 4~5 天 |

三档都只在解决"每帧重写描述符的 CPU 开销"。当前这个量级是每帧十几微秒，占一帧（约 10 ms）的千分之一量级 —— 所以真正该等的不是"什么时候顺手做"，而是表里那些**数量级信号**。

### 10.3 参考实现的位置

Granite 是三条路径并存的写法，真要上某一档时看它对应部分：

- per-type 位掩码 + `ArraySizeAccessMeta meta[VULKAN_NUM_BINDINGS]` + `UNSIZED_ARRAY = 0x7f`（`vulkan/descriptor_set.hpp`）；
- `bindless = layout.meta[0].array_size == UNSIZED_ARRAY`（`vulkan/descriptor_set.cpp:39`）—— 同一个 allocator 自动切 bindless，不需要两个类；
- `descriptorPoolOverallocation` 可用时加 `ALLOW_OVERALLOCATION_POOLS / SETS_BIT_NV` 复用 pool（`vulkan/descriptor_set.cpp:388-395`）；
- `vkGetDescriptorSetLayoutSizeEXT` / `vkGetDescriptorSetLayoutBindingOffsetEXT` 记 desc buffer 偏移（`vulkan/descriptor_set.cpp:215-230`）。

### 10.4 升级前先做的事

1. **set 0 的单一事实来源**：15 个全局 binding 现在由 `DescriptorManager` 定义、各 Pass 靠约定引用。上任何一档之前，先把它收敛成一处导出（binding 号 + 类型 + stage），否则池化只会把"写错 binding"变成"写错 set"。
2. **shader 侧已经不用手抄了**：binding 号与结构体布局由构建期反射给出（[shaders](../20-pipeline/shaders.md) 第 6 节），C++ 侧照抄即可被编译器拦住。
3. **先量化再动**：加计数（每帧 set 新建数 / 重写数）比换机制便宜，与 `--stats-pipelines`、`--stats-scratch` 同一套做法。
