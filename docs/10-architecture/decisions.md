---
id: architecture/decisions
title: 关键设计决策与取舍
type: architecture
scope: 空（跨代码库的设计理由）
read-when: 想改变某个既有设计之前；或评审"这个方案为什么不用更好的做法"
size: ~1813 行
updated: 2026-11-26
---

# 关键设计决策

每条都写清**放弃了什么**。没有"只列结论"的条目——只写结论的决策文档等于没有文档。

> 顺序大致按引入时间。标"已修"的是被后续阶段推翻或改进过的。

## 决策 1：Vulkan 1.3 动态渲染，彻底不要 `VkRenderPass`

- **放弃**：Vulkan 1.0~1.2 设备与移动端。
- **换来**：没有 `VkRenderPass`/`VkFramebuffer` 要建、要缓存、要随图像数复制；
  不用处理 `initialLayout`/`finalLayout`/subpass dependency 这套**与渲染图显式屏障语义重叠**的隐式同步。
  一个 Pass 就是"一组 imageView + loadOp/clearValue"，正好是 `VkRenderingInfo` 的形状。
- **代价**：`vkCmdBeginRendering` 之后必须显式设 viewport/scissor（已做成所有图形管线的动态状态）；
  不能用 subpass（TBDR 的 on-chip 复用没了，桌面无影响）。

## 决策 2：线性渲染图，而不是完整 DAG

- **放弃**：自动重排、内存别名化、死代码剔除、跨帧资源池化。
- **换来**：约 1200 行能读懂能调试的同步系统。
- **为什么"线性"够用**：手写屏障 90% 的痛点是"这次该是什么布局、上次是什么"，顺序执行 +
  记住每张图上次状态就完全解决了。
- **为什么不上 DAG**：DAG 需要拓扑排序、生命周期分析、别名内存布局、跨帧池化——每样都是独立子系统，
  且错了极难调试（"这个别名是不是踩了那个的生命周期"）。收益要等场景规模真的上来才体现。
- **D-2 的压力测试**：新增 9 个 Pass（1 亮部 + 3 down + 3 up + Present + PresentTransition）
  **没写一行屏障代码**，全部只声明 `read`/`writeColor`。它扛住了，但也暴露了"漏声明不会报错"的风险，
  所以同一提交里补了"同一 Pass 内重复访问只保留最后一次声明"和"声明合法性校验"。

## 决策 3：访问声明推导屏障，而不是 Pass 自己写屏障

- **放弃**：少写几行声明。
- **换来**："漏声明 = 漏同步"这个**响亮**的失败模式（验证层直接报 layout mismatch / sync hazard）。
- **反过来的设计**（Pass 自写屏障，图只做批处理）的失败模式是"屏障太少 -> 偶发花屏"
  或"屏障太多 -> 性能悄悄下降"，两者都难定位。**错误可见比错误少更重要。**
- **代价**：声明与 shader 之间有一份必须手工同步的清单（例如 `LightingPass::addToGraph` 里 9 条 `read`）。
  改 shader 忘了改它就会炸——但会**立刻**炸。

## 决策 4：渲染图不管采样器，只管图像

- **放弃**：接口更省几行。
- **理由**：采样器是"怎么读"（过滤/寻址），与"谁写了这张图"（图要管的同步）正交。
  塞进图里会导致同一张图被不同过滤方式读时要做冲突解决，接口语义变模糊。
- **代价**：描述符集必须等"图已创建 image view"之后才能写，`Application` 构造顺序因此有硬依赖。

## 决策 5：`presentTransition` 由应用显式声明

- **放弃**：渲染图对"帧"的完整知识。
- **换来**：离屏渲染 / 多窗口 / 截图不用改渲染图。

## 决策 6：光照输出线性 HDR，tonemap 独立成 PresentPass（D-2）

- **放弃**：一张全分辨率 16F 中间目标（1920x1080x8B 约 16MB 显存 + 相应带宽）。
- **换来**：Bloom 作用在线性量上，后效链可组合。
- **不这么做会怎样**：Bloom 作用在已 clamp 的 LDR 上只能"把白涂更白"，整体发灰。
- **为什么曝光也一起搬**：Bloom 阈值必须作用在与显示无关的线性量上。曝光留在光照里，
  阈值就随曝光漂移（拖滑块时辉光强度乱跳）。

## 决策 7：G-Buffer 的 Albedo 用 `R16G16B16A16_SFLOAT`（D-2 从 8bit SRGB 改）

- **放弃**：该通道带宽与显存 x2。
- **换来**：天空盒 HDR 高光能活着进 Bloom。
- **为什么需要**：设计上让天空盒"借道 albedo 通道"把线性 HDR 辐照度交给 Lighting 透传。
  8bit 会在写入时截断到 1.0，而被截断的 1.0 **恰好超过 Bloom 阈值** -> 整块天空起辉光 -> 一层假雾。
- **未来可考虑**：给天空盒单独一个 HDR 附件，或把天空从 G-Buffer 拿出去单独画。

## 决策 8：Bloom 用 4 级金字塔，而不是单级大核模糊

- **放弃**：7 个额外 pass + 4 张额外中间目标。
- **换来**：O(N) 采样复杂度 + 多尺度光晕。
- 单级大核是 O(r^2)，且只有一个尺度（近处远处辉光形态一样，看起来"糊"而不是"发光"）。
- 4 级是实测选定：3 级最大辉光半径偏小；5 级最粗一级只有 60x34，贡献看不见但多付两个 pass。
- **代价**：`kLevelCount` 与 `DescriptorManager` 池余量（+64/+96）有隐式耦合。

## 决策 9：上采样用加法混合叠加，不额外开累加缓冲

- **放弃**：数据流更单纯的 ping-pong 缓冲（显存 x2）。
- **换来**：省一张全分辨率缓冲，代价是"同一张贴图在一个 Pass 里既读又写"
  （`LOAD_OP_LOAD` + `ONE/ONE` 混合）。渲染图保证进入 Pass 时布局正确，这在 Vulkan 里完全合法。

## 决策 10：IBL 一次性 GPU 烘焙 + split-sum

- **放弃**：约 2MB 常驻显存 + 初始化几十毫秒。
- **换来**：运行时零成本的物理正确环境光照。
- 老实现 `envSpecular *= (1.0 - roughness)` 能量完全不守恒（粗粗糙度时高光消失）。
  split-sum 把镜面积分拆成两个可预计算项，运行时只有 2 次采样。**实测过曝像素 95.9% -> 7.1%。**

## 决策 11：IBLBaker 使用动态渲染

`renderFaces` / `renderBrdfLut` 直接用 `vkCmdBeginRendering`，不建 `VkRenderPass`/`VkFramebuffer`。
原则仍然保留：命令缓冲引用的 image/view/buffer 必须活到提交完成之后。

## 决策 12：材质槽位用「一次性预分配 + 空闲栈」

- **放弃**：256 槽 + 1024 个 sampler 描述符在启动时一次占掉（即使场景只有几十个材质）。
- **换来**：① 运行期无 `vkAllocateDescriptorSets`（不会池耗尽）；② 分配/归还 O(1)；
  ③ 槽位号稳定，可直接进 push constant（这是 bindless 的地基）。
- **配套**：`kInvalidMaterialSlot = UINT32_MAX` 而非 0（见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 4）。

## 决策 13：glTF 几何在加载时烘焙到世界空间

- **放弃**：节点层级的动态性（想让 Sponza 里某扇门动起来做不到）。
- **换来**：① 渲染路径不需要"每子网格一个 model matrix"（Sponza 上百节点，逐节点 push constant 是开销）；
  ② 顶点缓冲合并成一个（减少绑定切换）；③ 与 OBJ 路径产出的 `Model` 同构。
- **对开放世界目标不行**：见 [known-limits](../70-status/known-limits.md) 与
  [roadmap](../80-roadmap/roadmap.md) F4。

## 决策 14：用 `RenderTagComponent` 标签而不是继承类型

`GeometrySystem::render` 里按 `isSkybox`/`isGrid` 选管线。极简，不需要多态/组件注册。
标签仍是互斥语义（类型上没强制），但现在是可选组件，渲染系统通过 view 查询。

## 决策 15：`FrameInfo` 作为每帧数据总线，引用传递可变状态

- **放弃**：编译期约束（谁都不能改什么）。
- **换来**：Pass 不需要持有 `Scene*`，换场景只需换 `FrameInfo`；ImGui 改了 `settings` 当帧生效。
- **代价**：`FrameInfo` 是"什么都往里塞"的袋子。成员混用引用和裸指针（可空的用指针），
  但**没有文档规定这条规则**——属于一致性瑕疵。

## 决策 16：`Application` 构造函数承担全部创建顺序

- **放弃**：可测试性、短构造函数。
- **换来**：所有顺序约束集中在一处，**顺序错了立刻在这里暴露**。
- 顺序细节见 [layering](layering.md) §3。

## 决策 17：窗口不可缩放 -> 已由 TD-03 推翻

原决策是"窗口不可缩放"，因为它阻塞动态分辨率/LOD/超采样，但工作量与多帧并行耦合。
TD-03 已完成：可缩放窗口、帧边界重建、动态分辨率接口。保留此条是为了记录**当时为什么先不做**。

## 决策 18：`std::cout` / `std::cerr` 而不是日志库

spdlog 已在 `3rdParty` 且配了 include，但**一行都没用**。
`[Tiny]` / `[GLTF]` 这些前缀是手拼的，没有级别区分。

关键后果：验证层消息原本走 stderr，"stderr 0 字节"这条验收标准因此顺带要求了
"没有 `std::cerr` 输出"。P0 已把验证层回调改成结构化计数 + 可选 JSONL，**不再写 stderr**。

## 决策 19：配置用 nlohmann/json 存进工程根 `config/`（W1）

- **为什么需要**：D-2 调 Bloom 扫了四组参数，每组都要"改源码 -> 重编译 -> 启动 -> 截屏"。
  这不是"加个好用的功能"，是被开发节奏逼出来的。
- **为什么用 nlohmann/json**：它已经在树里（`3rdParty/tinygltf/json.hpp`），零额外依赖。
- **为什么放工程根**：与资源路径同源——不能依赖工作目录；且能让 Debug/Release 共享同一份设置、
  让这份文件进版本库当默认预设。
- **容错取向**：文件不存在 / JSON 语法错 / 单字段类型不对，**都不抛异常**，
  缺失字段保持编译期默认值并记警告。手改坏一个字段不该让引擎起不来。
- **原子写**：先写 `.tmp` 再 rename。直接覆盖写到一半崩溃会留下语法坏掉的配置。
- **只持久化"设置"，不持久化"场景"**：前者是设置，后者是资源（涉及路径、加载顺序、失败回退）。
- **UI 文字必须 ASCII**：ImGui 内置字体只有 ASCII 字形，中文会渲染成 `???`。
  所以 `EngineConfig::getLastStatus()` 返回英文，这是刻意的。

## 决策 20：把"瞬时资源必须有人写"做成渲染图硬校验（W2）

- **为什么**：渲染图保证**同步**正确性，但**语义**正确性完全没检查。
  验证层对这类问题一言不发（从 `UNDEFINED` 转出来是合法的）。
- **为什么只需顺序遍历**：这是线性渲染图，Pass 严格按声明顺序执行，顺序扫描的结论就准确。
- **为什么报错要指名"本该由谁写"**：只说"没人写过"读者还得自己翻是谁该写。
- **为什么给常驻资源豁免**：IBL 结果这类整帧只读是正常状态。豁免依据 `TextureDesc::persistent`，
  由 `importTexture` 把"初始布局 != UNDEFINED ⇒ 常驻"回写进去——同一事实只能有一个来源。
- **代价**：它逼着两个 Pass 改了行为（Bloom 关闭从"跳过"改成"强度置零"；`importTexture` 回写）。
  **这笔交易我当时并不满意，已登记为 TD-06 并已修复**：现在 Bloom 关闭时真的跳过 7 个 pass，
  PresentPass 使用只读 HDR 的独立管线。

## 决策 21：材质描述符 bindless（D-3）

- **为什么**：Sponza 103 子网格，每次切材质都要 `vkCmdBindDescriptorSets`。
- **为什么是"一个 set 里 4 个纹理数组"**：这就是 bindless 的定义，是后续实例化 / GPU-driven 的前提。
- **为什么 4 个独立数组**：GLSL 不允许把 `sampler2D` 放进结构体数组再索引。
- **为什么变长数组**：想让容量可调而不维护多套布局。代价是三个必填开关（见 [descriptors](descriptors.md) §3）。
- **为什么 `materialValid` 走 push constant**：见 [descriptors](descriptors.md) §5。
- **收益/代价**：FPS 452 -> 498，绑定 104 -> 1，**draw call 不变**。
- **新增约束**：bindless 后"回收再复用"会改写 GPU 可能正在读的数组项。
  D-4 已改为 timeline 延迟回收（见 [resource-lifetime](resource-lifetime.md) §4.3）。

## 决策 22：旋转以四元数为真源，欧拉角只做边界表示

- **为什么**：Gizmo 连续拖拽不能直接改欧拉分量。`T*Ry*Rx*Rz*S` 下
  `rotation.x += delta` 不等于绕局部 X 转；pitch 到 ±90° 还会万向节死锁。
- **做法**：`TransformComponent` 增加 `glm::quat orientation`，`localMatrix()`
  用 `mat4_cast`；`rotation` 是显示/序列化缓存，通过 `setEuler()` /
  `setOrientation()` 双向同步。
- **为什么保留欧拉角**：相机、`config/engine.json`、`*.scene.json` v1、
  Inspector 都以欧拉角为接口。改格式会破坏现有资产与配置迁移。
- **为什么局部右乘**：`orientation` 是相对父节点的局部旋转，绕自身轴转就是
  `orientation * angleAxis(delta, localAxis)`；世界空间方向由父矩阵共轭得到。
- **代价**：两套表示必须同步。**禁止直接写 `.rotation`**，只能走
  `setEuler()`；拖拽命令同时存欧拉角与四元数，撤销/重做写回 `orientation`。
- **已知代价**：万向节附近 Inspector 的欧拉数值可能跳变；极端姿态存盘后
  可能得到等价但不同的欧拉表示。普通角度矩阵精确一致（`--selftest` 有断言）。

## 决策 23：选中框显示 OBB，拾取粗筛仍用世界 AABB

- **为什么**：世界 AABB 旋转后会被包大，框与物体分离；三维编辑器的选中框
  必须跟物体一起转。
- **做法**：`EditorOverlayPass` 用 `BoundsComponent.local` 的 8 个角点乘
  `WorldTransformComponent.matrix`；Gizmo 锚点用局部中心乘世界矩阵。
- **为什么拾取仍用 AABB**：均匀网格 broad phase 需要轴对齐体；精确求交仍在
  三角形上。看到的是 OBB，能点到的范围是稍大的 AABB —— 这是性能与精度的
  折中，不是 bug。
- **代价**：没有 `BoundsComponent` 的实体退回 AABB 显示；细长/旋转物体的
  点选范围会比 OBB 略大。

## 决策 24：管线缓存落盘，且必须过三道校验

- **要解决的问题**：`vkCreatePipelineCache` 传空 cache 也能跑，代价是每次启动都要
  重新编译全部着色器流水线。桌面 NVIDIA 驱动自己就有磁盘级缓存，所以本机测不出
  差距（`--selftest 60` 冷 102.8 / 热 98.9 FPS，就是噪声）；真正疼的是
  Android / Steam Deck 这类"驱动不提供磁盘缓存"的场景，以及将来上
  `VK_KHR_pipeline_binary` 做预烘焙。
- **为什么不能"试试看"**：换驱动/换卡之后把旧 blob 喂给驱动是未定义行为，轻则
  `VK_ERROR_INVALID_PIPELINE_CACHE_DATA`，重则驱动内部崩。所以文件头里记
  `magic / version / vendorId / deviceId / driverVersion / apiVersion /
  payloadBytes / payloadHash(FNV-1a) / pipelineCacheUUID`，加载时按
  UUID → 长度 → 哈希 三道校验，任一不符**整份丢弃**并打一行`丢弃旧缓存：原因`。
- **落盘必须原子**：先写 `pipeline_cache.bin.tmp`，再 remove + rename。直接覆盖
  唯一一份可用缓存，等于"崩一次就把优化成果清零"。载荷 <= 8 字节（空驱动头）时
  直接不写，避免用空文件盖掉上一份有用的。
- **可写性要探测**：`%LOCALAPPDATA%` 建得出来不等于写得进去（沙盒/受限账户会
  `EACCES`）。探测失败就退回工作目录 —— 缓存放哪儿不重要，有没有才重要。
- **失败不是错误**：`vkCreatePipelineCache` 失败就退回 `VK_NULL_HANDLE` 并打印
  "禁用管线缓存"。缓存是纯优化，任何情况下都不许让启动失败。

## 决策 25：容器用"池 + 环"，且命中必须提升（TM-03 / TM-04 / TM-05）

- **问题**：每帧都要建/拆一批"按 key 查、只活一两帧"的表（`GlassPass` 的 chunk 状态、
  将来的材质解析与视锥缓存）。写成 `std::unordered_map` 每帧 clear + 重填，代价不在查找，
  而在每帧几百次节点 new/delete —— 时间花在堆上，不是花在渲染上。
- **做法**：`src/Core/ObjectPool.h`（分块对齐分配 + vacants 栈 + placement new）+
  `src/Core/IntrusiveList.h`（节点挂在对象自己身上）+ `src/Core/TemporaryHashmap.h`
  （RingSize 个环，每环一张固定桶侵入式哈希表；`beginFrame()` 轮转到下一个环并回收它）。
  环回收把对象还回池，下一帧复用同一批指针，稳态下零分配。
- **必须有命中提升**：`request()` 命中老环时把条目重新链进当前环。没有这一步，
  条目会跟着"创建它的那一帧"一起过期 —— 一个每帧都在用的 key 每 `RingSize` 帧被销毁重建一次。
  实测代价：`GlassPass` 接成 RingSize=2 而没做提升时，30 帧里重建了 15 次几何（等于缓存
  每两帧失效一次，比不用缓存更糟）；补上提升后 30 帧只重建 1 次，池存活数恒定。
- **所有权**：条目归表所有，`~TemporaryHashmap()` 自己 `clear()`；`ObjectPool` 只管内存，
  对象仍须显式 `destroy()`，Debug 下析构时若还有存活对象直接断言 —— 宁可当场停，
  也不要在发布版里悄悄泄漏。
- **驱动怪癖单独一层**：`src/RHI/DeviceQuirks.h` 把"探测到的事实"（厂商、设备类型、
  subgroup 支持的阶段与操作、是否有独立 compute 队列）和"由事实推出的策略位"
  （`asyncComputePost`、`subgroupOpsUsable`）集中成一个纯数据结构，启动打印一行。
  判断点只读字段，不再各处散落 `vendorId == 0x10DE`。核显上异步 compute 默认关：
  与图形共用同一批执行单元，抢占只会让总时间变长。
- **不引入的**：参考实现的 subpass 合并 / transient 相关怪癖位（本项目用 dynamic rendering，
  没有 subpass 可合并），以及线程安全版本的对象池（当前所有使用点都在主线程）。


## 决策 26：每帧暂存用一大块 + bump，按帧槽轮转（TM-07）

- **问题**：每帧都要往 GPU 送一批「只活这一帧」的数据（相机与 Pass 参数、实例列表、每帧重建的小段顶点）。原来的做法是每个 Pass 自己建一份 `HOST_VISIBLE | MAPPED` 的 buffer、自己 memcpy、自己 flush，尺寸写死 —— `src/Render/EditorOverlayPass.cpp:103` 的 `bufferSize = sizeof(glm::vec3) * 48` 只够两个盒子，多画一个就越界；而这类分配一共有十几处，串行启动时各建一份，写起来谁也不欠谁。
- **做法**：`src/RHI/ScratchArena.h/.cpp` 按 `kFramesInFlight` 开槽位，每槽一整块映射 buffer（usage 一次给足 UNIFORM / STORAGE / VERTEX / INDEX / INDIRECT / TRANSFER_DST）。`RenderGraph::beginFrame` 调 `beginFrame(slot)` 把 offset 归零，各 Pass 在 `addToGraph` 里 `allocate(bytes, alignment)`，`RenderGraph::endFrame` 调一次 `flushIfNeeded()`。
- **uniform 必须 256 对齐**：规范只保证 `minUniformBufferOffsetAlignment` 不超过 256，按 16 对齐会在部分驱动上直接撞 VUID。顶点 / 索引 / 实例用 16 就够。
- **扩容时旧块不能立刻销毁**：本帧早先拿走的 `Block` 还指着它。旧块挪进 retired，等下一次轮到这个槽位再回收 —— 依据是槽位被复用的前提就是它上一次提交已完成（`Renderer::waitForFrameSlot`）。这条前提变了，回收时机也要跟着变。
- **描述符必须带偏移**：`Block.offset` 每帧都可能不同，用它绑 uniform / storage 的描述符要每帧重写。写死 `offset = 0` 会读到别人的数据，而且只在扩容或分配顺序变化时才出错。
- **COHERENT 就别刷**：内存类型是 `HOST_COHERENT` 时跳过 `vmaFlushAllocation`（本机 30 帧全部跳过）。实测收益不在每帧几微秒，而在三件事：帧内分配可见（`--stats-scratch`）、写死尺寸的自建 buffer 不再散落、新消费者不必再造一份 VMA 分配。
- **不做一次性大改**：先只迁 `SSAOPass`（每帧一段 UBO）。其余消费者（`FogVolumePass` / `ReflectionPass` / `ParticlePass` / `VegetationPass` / `EditorOverlayPass` / 全局 UBO）按「下次改到它时顺手迁」推进 —— 每个都牵描述符写入时机，一次全迁会让一次像素回归极难归因。

## 决策 27：资源别名化先量后做，本结构下不做（TM-08）

- **问题**：显存里同时活着的中间目标越多，峰值越高。参考实现把「生命周期不重叠的资源放进同一块显存」列为显存优化的重要手段（`renderer/render_graph.cpp:628` 的 `// Find resources which can alias safely`）。
- **做法**：先只出报告，不改任何分配 —— `src/RHI/RenderGraphAlias.h/.cpp` 在 `RenderGraph::endFrame` 里取每个纹理的 `firstWritePass` / `lastUsePass`（Pass 序号用 `lastFrameExecutedPasses.size() - 1`，跳过的 Pass 不占号），`pack()` 按 `firstWrite` 升序做线性扫描 + best-fit 装箱，`--alias-report` 打印候选与预估节省。
- **为什么先只报告**：别名写错的症状是「特定分辨率或特定 Pass 开关组合下偶尔花屏」，不是崩溃；而它要动的是分配器与 image view 的兼容性约束。先让数字可见、逐对核对，再决定要不要复用。
- **实测结论（1920x1080，`--selftest 30 --alias-report`）**：渲染图自己持有的每帧目标 8 个、40.8 MiB，生命周期全部重叠，**可省 0**；真正占显存的是 13 个每帧重建但导入的目标（181.9 MiB，GBuffer 5 张附件 + 深度），显存不归渲染图管；另有 4 个常驻导入目标（229.4 MiB）本就不该参与。
- **不做第二步**：要真省必须先做三件结构改造 —— 让渲染图持有 GBuffer 与深度的显存（现在是 `src/Render/GeometryPass.cpp:33` 建好后 `importTexture`）、让 bloom 链与 TAA 的生命周期错开（现在是嵌套的 up / down sample 链）、放宽并验证格式兼容规则（现在要求同格式同尺寸）。三条都不便宜，而当前 8 个目标合计才 40.8 MiB。
- **判据与兼容规则写在代码里**：合并前提是「格式 / 尺寸 / mip / 层数全同」，比 Vulkan 允许的更保守 —— 松一档就要顺带验证 image view 的兼容性，那是第二步要付的成本，不是第一步的。
- **保留的东西**：`--alias-report` 与 `[Alias] selfTest`（生命周期装箱 + 形状兼容 + 字节估算）常驻。哪天渲染图开始持有 GBuffer，这个开关会立刻给出新的数字。
- **不引入的**：`VK_IMAGE_CREATE_ALIAS_BIT` 的实际绑定（未启用）、显式子分配策略（`VmaAllocation` 仍在渲染图手里按资源分配）。
## 决策 28：跨队列所有权转移（EXCLUSIVE）本结构下不做（TM-09）

- **问题**：跨队列资源现在一律 `VK_SHARING_MODE_CONCURRENT`（建资源时 `queueFamilyIndexCount = 2` 加 graphics / compute 两个族），靠 timeline semaphore 串依赖。CONCURRENT 的代价是驱动不能假设单一队列独占，可能在压缩与布局上保守；EXCLUSIVE 加 release / acquire 是另一条路。
- **做法（已实现并回退）**：`crossQueue` 资源在开关打开时按 EXCLUSIVE 建（`queueFamilyIndexCount = 1`），`SubresourceState` / `BufferEntry` 记 `ownerQueue` / `releasedFrom` / `transferCount`，在 `emitBarrierIfNeeded` / `emitBufferBarrierIfNeeded` 里发 release（写进上一个队列的命令流）加 acquire（写进当前 pass 的命令流），帧末再把仍持有的资源 release 回另一条队列。探针数据两个方向都正确（`[TD02] graphics->compute verified` 与 `compute->graphics verified`，checksum 与 count 一致），但验证层始终有 error。
- **验证层真实报文（逐版记录）**：
  - 创建用 `queueFamilyIndexCount = 0`、首次用 IGNORED 到 family 的 acquire：6 个 error，`acquiring ownership of VkImage ... from srcQueueFamilyIndex 4294967295 to dstQueueFamilyIndex 0 has no matching release barrier queued for execution.` —— 无所有者这条路验证层不认。
  - 创建即归 graphics、用时成对 release 加 acquire：3 个 error，`from srcQueueFamilyIndex 0 to dstQueueFamilyIndex 2 has no matching release barrier queued for execution.`
  - 把全零的 `srcStageMask` 换成 `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`（全零掩码的屏障不被当成 release）：仍是 3 个 error。
  - 加帧末 release 加用时懒 acquire：降到 2 error 加 2 warning，`releasing queue ownership ... from srcQueueFamilyIndex 0 to dstQueueFamilyIndex 2 duplicates existing barrier recorded in this command buffer.` 与 `acquiring ownership ... from srcQueueFamilyIndex 0 to dstQueueFamilyIndex 2 has no matching release barrier queued for execution.`
- **根因（结构性）**：每条队列一帧只有一条命令流、一次提交，提交顺序由数据依赖方向决定（`src/RHI/Renderer.cpp:317-328`，生产者在先）。release 必须出现在源队列已提交的命令流里，所以一次转移只能沿提交方向发生。`--cross-queue-test` 的探针在同一帧里两个方向都要（偶数帧 compute 产、graphics 消费；奇数帧 graphics 产、compute 消费），必然有一半转移逆着提交顺序 —— 这不是实现细节，是单提交结构表达不了跨队列乒乓所有权。
- **结论**：不实现。单向跨队列（一帧内只沿一个方向转交）在原理上可行，但当前唯一的跨队列消费者就是那个双向探针，没有可验证的落点；按 [borrowed-techniques](../80-roadmap/borrowed-techniques.md) §2 的闸门（必须有可量化验收），不满足条件。
- **保留的东西**：`weak-points` 第 1 条与 `known-limits` 第 33 行的问题描述仍成立，但补上了「已评估，见决策 28」；`--cross-queue-test` 与 TD-02 探针继续作为 CONCURRENT 路径的正反例跑在验证矩阵里。
- **不引入的**：`VK_QUEUE_FAMILY_EXTERNAL` / foreign 队列族的间接所有权（会把外部同步责任引进来）、把渲染图拆成每队列一帧多提交（那是另一套架构，代价远大于这点带宽收益）。
## 决策 29：Pass 的准备与录制分开，但准备阶段不并行跑（TM-10）

- **问题**：`RenderGraph::endFrame` 的 Pass 循环里混了三类工作 —— 访问去重与声明校验（纯 CPU 数据）、屏障推导（依赖上一个 Pass 留下的布局）、命令录制（碰 `VkCommandBuffer`）。第一类看起来可以并行，21 个 Pass 各算各的。
- **做法**：把循环拆成三段 —— 串行挑出本帧要跑的 Pass 并分配计时序号；`preparePass()` 做去重 / 校验 / 组装 `VkRenderingInfo`（纯数据，错误先记进 `PreparedPass::error`，回串行阶段按 Pass 顺序抛，否则报错顺序会随线程调度变）；然后串行录制。准备阶段可以选择走 `JobSystem::parallelFor`，由 `--parallel-prepare` 控制，默认关闭。
- **为什么错误不能从 worker 里抛**：并行时异常会随调度落到不同线程，第一个报出的错不一定是第一个错。所以 `PreparedPass` 带一个 `error` 字段，回到串行阶段按 Pass 序号依次检查 —— 报错文案与顺序和串行路径逐字一致。
- **实测（RTX 2070，1920x1080，`--selftest 60 --stats-rendergraph`，各 3 次）**：串行准备 35.5 / 35.9 / 36.9 us/帧，并行准备 176.9 / 203.2 / 207.7 us/帧，录制都是 1435~1532 us/帧。并行慢 4.9~5.8 倍。
- **结论**：不并行。21 个 Pass 的准备合计只有 36 us，占一帧（约 10 ms）的 0.3%；而把 21 个任务丢进工作窃取池、再等它们做完的开销是 160 us 量级 —— 线程唤醒延迟远大于被并行的工作量本身。要让并行赢，得让每个 Pass 的准备重到几十微秒（例如真的在准备阶段做 SPIR-V 反射或管线状态推导），当前不是。
- **保留的东西**：准备 / 录制的拆分本身留下（`preparePass()` 与 `PreparedPass`），因为屏障推导与数据准备的边界现在写在类型上，读代码不用再区分哪几句碰了命令缓冲；`--stats-rendergraph` 打印两段耗时；`--parallel-prepare` 保留为可复现的对照开关，默认关闭。
- **不引入的**：跨帧缓存 PreparedPass（Pass 的声明基本不变，缓存能省掉 36 us 里的一部分，但每帧都要校验句柄与 usage，缓存失效条件比省下的时间更难维护）、把录制也并行化（`vkCmd*` 只能在各自队列的命令缓冲上串行录制）。

## 决策 30：把反射结果变成构建产物，而不是运行期反射（TM-12）

- **问题**：binding 号、set 号、UBO / push constant 的成员偏移全靠手抄。写错一个数字，验证层要等到那一刻才报（descriptor 类型不匹配 / 越界），而"块大小改了 C++ 结构体没改"这类错误往往表现为画面错乱，不是崩溃。
- **做法**：构建期为每个 shader 生成 `build/shaders/<name>.reflect.h`（`tools/SpirvReflect.cpp`，自己解析 SPIR-V）。C++ 侧 `#include` 之后用 `static_assert` 对齐 `sizeof` 与 `offsetof`，binding / set 号直接用生成常量。
- **为什么自己写解析器**：`spirv-reflect` / `spirv-cross` 的块大小口径与我们不同（它们会把 push constant 补齐到 16、对 SSBO 报 0），而 `static_assert` 比的是 C++ 的 `sizeof`。用外部工具就要在文档里记住"它们的数字要换算"，那等于把坑换了个位置。
- **为什么不是运行期反射**：运行期反射要求 shader 以 SPIR-V 形式随包发布并调用 `vkCreateShaderModule` + 自己解析，既增加启动时间，又让"binding 变了"变成运行期才能发现的事；构建期生成还能进 diff。
- **实测（52 个 shader）**：全部生成成功；与 `spirv-reflect` 逐块对比，15 处差异经复核全是口径差异，我们的数值与 C++ 结构体一致（`ssao.frag` 的块 1168 字节、偏移 0/64/128/1152/1156/1160/1164、`samples` 长度 64）。
- **验收证据**：在 `shaders/ssao.frag` 里临时插 `vec4 dbgMargin;`，构建立刻被 6 条 `static_assert` 拦住（块大小 + samples / kernelSize / radius / bias / enabled 偏移）；改回后一次通过。
- **已知边界**：std140 尾部填充（末尾是 `vec3` 时块大小会进位）、变长数组块只给偏移不给大小、全局 set 0 仍由 `DescriptorManager` 单点定义。
- **不引入的**：把反射结果做进运行期缓存、按 shader 生成描述符 set 布局（布局顺序在管线布局里是有序的，生成反而更难读）。

## 决策 31：描述符保持现状，先写触发条件再谈升级（TM-11）

- **问题**：参考实现在同一套代码里并存"池化 set / push descriptor / descriptor buffer"三条路径。要不要跟？
- **实测（2026-11-19）**：全仓库 `vkUpdateDescriptorSets` 31 处调用点，但按频率看每帧只有 12~15 次、每次 1~2 个 binding（BloomPass 3 次、Cloud / FogVolume / SSR / SunFlare 各 1、SSAOPass 模糊 1、PresentPass 1、TAA 1）；`UPDATE_AFTER_BIND`、`vkCreateDescriptorUpdateTemplate`、`vkCmdPushDescriptorSet`、`VK_EXT_descriptor_buffer` 全部 0 命中。CPU 侧开销在每帧十几微秒量级，占一帧的千分之一量级。
- **结论**：不升。三档阶梯的**触发条件**写进 [descriptors](descriptors.md) 第 10 节（每帧新建 set 组合 > 16 / 每帧重写 > 200 次 / 需要 GPU 写描述符），出现时照那节走。
- **为什么现在就写触发条件**：这类"基础设施升级"最容易在没有信号时凭感觉做，做完还要长期维护三条路径。把判据写成数字，下一次就不用重新讨论。
- **升级前必须先做的**：set 0 的 15 个 binding 收敛到单一事实来源（现在散在 `DescriptorManager` 与各 Pass 的约定里），否则池化只会把"写错 binding"变成"写错 set"。

## 决策 32：点光源走两条路径，聚簇默认关闭（TM-13）

- **问题**：引擎此前根本没有点光源 —— `ubo.numLights` 恒为 0，`lighting.frag` 里 `for (int i = 0; i < ubo.numLights; i++)` 是死代码。要做聚簇，先得有灯。
- **做法**：`LightSources` 存灯（`GpuPointLight{ positionRadius, colorIntensity }`），`ClusteredLighting` 建 16x9x24 的视锥分块表（计数排序，`kMaxLightsPerCluster = 256`），参数块 / ranges / indices / 灯数组走 Set 0 新增的 binding 15~18，每帧从 `ScratchArena` 切四段写。着色器按 `ubo.clusterParams.x` 在「聚簇查表」与「逐灯遍历」之间二选一。
- **为什么先 CPU 建簇**：Release 实测 8 灯 49.7 us、128 灯 250.7 us、512 灯 1004.9 us。建簇改成「每盏灯先算候选簇范围 + 计数排序」后比最初的全量扫描快约 5 倍（Debug 512 灯 90.3 ms → 18.3 ms），暂时不需要 compute 版本。
- **实测（`--selftest 60 --voxel-world 1`，RTX 2070，Debug）**：8 灯 —— 逐灯 65.8 / 71.0 FPS，聚簇 61.4 / 58.6 FPS；512 灯 —— 逐灯 69.4 / 64.5 FPS（每片元最多 `MAX_POINT_LIGHTS = 100` 盏），聚簇 28.3 / 28.8 FPS（最挤的簇 202 盏）。
- **结论**：默认关闭，`--clustered-lighting` 才开。不是「聚簇没用」，而是这批灯太密：512 盏铺在相机前方 40 米见方的空域里，最挤的簇 202 盏，比旧路径截断到 100 盏还贵。它的目标区间是「总数上千、每簇几盏」的稀疏分布，等有那种场景再翻默认。
- **踩到的两个坑（都会挂驱动）**：① `DescriptorManager::buildGlobalSet` 给 15~18 写的是占位，而它可能在 `updateClusterBindings` **之后**被调用（重建 G-Buffer 视图时），占位把真数据盖掉 → 着色器把材质 factor 缓冲当簇参数读 → 簇下标越界 → `range.y` 巨大 → 死循环 → `failed to submit compute command buffer!`。修法：`buildGlobalSet` 末尾补 `applyClusterBindings(globalSets[frame])`。② 平面反射通道复用同一组 G-Buffer，`viewDepth` 可能出现负值甚至 NaN；`clamp()` 挡不住 NaN，而 `uint(NaN)` 是未定义行为 —— 改成「不大于 near 就回落到 near」的写法。
- **顺带修的**：反射通道自建的 Set 0（`ReflectionPass.cpp` 里的 `allocateSet(globalSetLayout)`）只写 binding 0~12，新增的 15~18 必须用 `applyClusterBindings` 补齐，否则验证层直接报「never been updated」。
- **验收方法的教训**：引擎每帧把相机位姿写回 `config/engine.json`，两次独立运行的截图会因机位不同而完全不可比（我第一次量到的「583979 像素差异」就是这么来的）。要逐像素比对，必须固定机位或用同一次运行里的两个模式做 A/B。
- **不引入的**：compute 建簇、阴影 / 雾 / 贴花共用簇表、每簇位掩码（当前整数索引表够用）。等真实场景压出瓶颈再说。

## 决策 33：点光源要"看得见"，先摆对位置再谈效果（TM-13 补）

- **问题**：`--light-stress` 早就把灯填进了 UBO，`numLights` 也确实到达了着色器（红屏探针验证过），
  但截图和"一盏灯都不加"几乎逐像素相同。结论一度被写成"灯光没实现"，其实是灯摆错了地方。
- **数字**：压力场原本铺在相机前方 22 米、约 40 米高的半空。实测每个片元到最近一盏灯的中位距离
  **66 米**，而单灯半径只有 8 米 —— 绝大多数片元根本没被任何灯覆盖。
- **做法**：① 用体素地形高度把灯贴地（抬 1.2 米）；② 压力场中心距离取 `max(22, 铺开半径)`，
  半径越大铺得越远；③ 单灯半径 12 米、强度 2（强度 20 时红通道直接饱和，光斑变成硬边橘色圆盘，
  反而看不出衰减）；④ 默认铺开半径 60 米。
- **怎么验证"真的有光"**：同一次运行里做 A/B（`--light-stress 0` 与 `--light-stress 8` 各截一张），
  不要跨运行比 —— 引擎每帧把相机位姿写回 `config/engine.json`，两次独立运行的机位不同，截图不可比。
- **不引入的**：灯的可视化 gizmo（画发光小球）、地面投影贴花。当前用真实光照验证；
  等需要"在编辑器里拖动灯"时再说。

## 决策 34：发光体借粒子管线画，别为演示新开 Pass（萤火虫）

**问题**：点光源在屏幕上没有本体，`--light-stress` 只能证明"光照算得对"，证明不了"这里有灯"。
用户的原话是"做一个像萤火虫一样、有形状颜色的光源"，也就是要看得见本体。

**做法**：萤火虫 = `ParticlePass` 的 billboard（画形状）+ `LightSources`（参与光照），
共享同一份 `FireflySample`。布点用 splitmix64 做确定性伪随机，动画只依赖时间（游走 + 呼吸）。

**为什么不再写一个 Pass**：
- billboard 要的东西（面向相机的四边形、程序化圆形软边、加法/透明度混合、深度只测不写、
  按相机距离排序以便和玻璃/水面的排序一致、被 Bloom 吃到）`ParticlePass` 全都有，且已经接好了水与 TAA 的顺序。
- 新写一个"发光点 Pass"意味着再抄一遍这些细节，还要再回答一遍"它排在水之前还是之后"。
- 代价只是给 `ParticlePass` 加一个 `setExtraInstances`（一行 push_back），风险与代码量都最小。

**两个实测出来的坑**（都写进了代码注释，别再踩）：
1. 粒子着色器把颜色乘 1.4，喂纯饱和色会被削顶成白点 —— 冷绿两群立刻分不出来；喂之前先乘 0.62。
2. 群的中心沿视线摆在相机前方 25 米。试图"采样地面高度"来贴地反而更糟：地形网格与体素世界是两套数据，
   按体素定高会把灯埋进山里或浮到海面上 —— 屏幕上直接看不到灯。

**默认关**：`--fireflies` 缺省为 0。演示开关不允许改变默认画面，这是 §2 闸门里"不破坏基线"的一条。

**不引入的**：灯的可视化 gizmo（编辑器里画线框球）、地面投影贴花、GPU 粒子模拟。
萤火虫的动画在 CPU 上算 32 个位置，成本可以忽略；等要几千个再谈 compute。

## 决策 35：单趟降采样（SPD）先量后判，本机不做（TM-14）

**问题**：Bloom 的 down 链是三级独立 pass，看起来像"可以合成一次 dispatch"的典型目标。

**做法**：先只加 `--bloom` 开关（强制打开 Bloom）拿到基线，用既有的 `--gpu-timing --gpu-timing-csv` 逐 pass 读 GPU 时间，
再决定要不要写 compute 版。

**为什么先量**：如果 down 链本身只占一帧的千分之一，写一个 13 抽样 + 双线性必须逐像素一致的 compute 着色器
就是把风险换成了噪声。

**实测（1920x1080，Debug，40 帧平均）**：`Bright 0.1325 ms`、`Down0 0.0332`、`Down1 0.0102`、`Down2 0.0056`
（down 链 0.0490）、`Up0 0.0362`、`Up1 0.0113`、`Up2 0.0064`（up 链 0.0539）；Bloom 合计约 0.235 ms/帧。
同帧 `Cloud 2.3945`、`Geometry 0.9806`、`Lighting 0.9752` —— Bloom 不是瓶颈。

**结论**：合并三级最多省两个 pass 的固定开销（每级约 5 µs，合计约 10 µs/帧），13 抽样的真实工作量不会消失；
`--stats-rendergraph` 还显示 21 与 23 个 pass 的 CPU 录制时间没有可见差异，"少两个 pass 省 CPU"也不成立。
因此**不实现**，把力气留给 2.4 ms 量级的 Cloud、或等真的要做 F6 Compute Bloom 时顺手带上。

**保留的**：`--bloom`（量测/演示用，默认仍跟随场景组件）、这条决策里的基线数字、以及"何时重新评估"的触发条件。

**不引入的**：为省 10 µs 而写的 compute 降采样链、把 Bright 也并进 dispatch、up 链改造。

## 决策 36：格式能力集中成一张表，`--print-formats` 出口（TM-19）

**问题**：`VkFormatProperties` 的查询散在各处，同一个"能不能当 storage image"的判断写法不一；
改 G-Buffer 格式时没人知道它同时影响附件 / 采样 / blit / 线性过滤中的哪些。Vulkan 也没有 `vkFormatToString`，
日志里出现 "format 37" 无法阅读。

**做法**：`src/RHI/FormatTable.h/.cpp` 提供 `formatName`（只覆盖引擎用到的十几个格式，其余退化成 `VK_FORMAT_<数字>`）、
`bytesPerPixel`、`describe(format, props)`（只读 `optimalTilingFeatures`，映射成八个布尔位）与 `query/print`；
`--print-formats` 在 `run()` 最前面打印后直接 `return`，一帧都不渲染。

**为什么放在 run() 而不是 finalizeDiagnostics**：这个开关的用途是"改格式前查一眼"，跑完 600 帧再打印没人会用。

**一处定义的边界**：G-Buffer 的四个格式仍由 `GBufferFormat`（`src/RHI/GBuffer.h`）单点定义，
格式表只是消费者 —— 表不是新的第二事实来源，它打印的就是那份常量。

**验收**：`--print-formats` 输出八列能力位；`[Formats] selfTest PASS | 名字映射 + 字节数 + 能力位 + 未收录退化`；
`--selftest 30` 走完整自检链、验证层 0 errors。

**不引入的**：`VkFormat` 全量表（160+ 个）、把格式表变成运行时校验器（`findSupportedFormat` 保持原样）。

## 决策 37：资产路径按可执行文件位置回溯；着色器热重载只轮询 `.spv`（TM-16）

**问题**：`cmake/AssetPaths.h.in` 在 configure 阶段把**绝对路径**烧进宏，仓库换目录、或把
构建产物搬到另一台机器，引擎就去读一个不存在的目录；改 shader 必须重启引擎，一次迭代
几十秒。

**做法**：

1. `src/Core/AssetPath.{h,cpp}` 提供 `tiny::assetpath::resolve()`：构建期路径**仍然优先**
   （存在就原样返回，默认行为零变化），只有在它不存在时才按锚点（`/shaders/`、
   `/Resources/`、`/config/`）把前缀剥掉、拼到运行时根上；运行时根按
   「exe 目录 → exe/../ → exe/../../ → cwd」的顺序探测。协议写法 `asset://`、
   `shader://`、`config://` 也支持，空后缀等于根本身。
2. 接入点只挑"真正把路径交给第三方库"的地方：`Pipeline::readFile`、
   `Texture` 的 `stbi_load` / `stbi_loadf`、`GLTFLoader` 的 tinygltf 入口、
   `EngineConfig` 的配置文件路径。`--print-assets` 打印三个根与三个构建期宏的
   存在/失效状态。
3. 热重载（`src/RHI/ShaderReload.{h,cpp}`）只监控 `.spv` 的 mtime：`Pipeline` /
   `ComputePipeline` 在构造时登记、析构时注销，`poll()` 发现变化才
   `vkDeviceWaitIdle` 并 `reloadShaders()`。每帧的代价是几十次 stat（无变化时 ~0）。

**为什么不在运行期编译 GLSL**：构建期的 glslangValidator + spirv-val 是 CI 验证过的
行为；运行期再塞一份编译器，等于让"本机跑通"和"CI 验过"分家。

**为什么轮询而不是 `ReadDirectoryChangesW`**：几十个文件一次 stat 只要几十微秒，
没有线程、没有句柄时序；将来要监控上千个文件时换通知机制，`poll()` 的接口不用变。

**边界（实测）**：

- 只重建 `VkPipeline` / `VkShaderModule`。改了 binding 数量或附件格式要重启 —— 那是接口变了。
- 仓库整体搬家 + 原树仍然存在时，构建期路径命中，看不出回溯效果；把原 `Resources/`
  与 `build/shaders/` 改名后跑，shader、纹理、glTF、配置都能从新位置找到
  （`[GLTF]` 成功加载 Sponza 即为证据）。
- 上述"原树完全消失"的实验里，退出时仍会触发 VMA 的
  `Some allocations were not freed...` 断言（`vk_mem_alloc.h:10975`）—— 属于既有的
  teardown 顺序问题，与路径解析无关，已记入 [known-limits](../70-status/known-limits.md)。

**不引入的**：虚拟文件系统层（内存挂载、目录遍历抽象）；把 `Resources/` 打成 pak 后
再从 pak 里按协议读（`AssetPack` 已存在，但那是另一件事）。

## 决策 38：只用 latched 事件解「谁先初始化」，immediate 类不做（TM-17）

**问题**：交换链重建那条路上有十一步，顺序写死在
`Application::recreateSwapchain()` 里。加一个 Pass 就得记得去那里补一行
`resizeAll`；忘了就是描述符指向旧尺寸、或者截图还按旧 extent 拷。这类
「谁先初始化 / 谁先被通知」的耦合，编译器一句都不提醒。

**做法**：引入一个极小的 latched 事件总线（`src/Core/EventBus.h/.cpp`），
只做「状态变化广播」这一种语义：

- **事件值会被记住**。后订阅的人立刻收到一次补发，不用等下一次发布 —— 这
  正是解「初始化顺序」的关键：订阅晚的人也能拿到已经发生的事实。
- `Subscription` 是 RAII：析构自动退订，订阅者不用在析构里记得清理。
- 发布时先拷贝订阅列表再回调，所以回调里订阅/退订都是安全的（自检里有一条
  专门盯这个）。
- 目前只有两个事件：`SwapchainRecreated`、`RenderTargetsResized`
  （`src/Core/AppEvents.h`，载荷只用整数，不让 Core 的订阅者为了一个事件
  把 `vulkan.h` 再 include 一遍）。

**删掉的手工调用点**（这就是验收口径）：

| 以前 | 现在 |
|---|---|
| `recreateSwapchain()` 里 `screenshotCapture.onResize(extent)` | `ScreenshotCapture::init()` 里订阅 `SwapchainRecreated` |
| `recreateSwapchain()` 里 `renderPipeline.resizeAll(renderExtent)` | `RenderPipeline::attachResizeEvents()` 订阅两个尺寸事件 |
| `recreateRenderTargets()` 里 `renderPipeline.resizeAll(renderExtent)` | 发布 `RenderTargetsResized` |

`RenderPipeline::attachResizeEvents()` 必须在 `initAll()` 之后调用：Pass 的
`resize()` 会重建附件与导入视图，早于 `init()` 调等于拿半初始化的对象干活。
这条顺序现在写在一个函数里、有注释，而不是散落在 Application 的两条路径上。

**为什么不把 `Renderer::onSwapchainRecreated()` 也改成订阅**：它重建的是
「每张影像一个 renderFinished 信号量」，而发布事件之后订阅者可能立刻用新的
尺寸重建附件 —— 信号量必须先于广播就位。它保留在 `publish` 之前显式调用，
是**有意为之的顺序**，不是遗漏。

**不引入的**：immediate 类事件（键盘/鼠标：需要每帧分发、消费与顺序，属于
输入系统的活）；事件优先级与取消；跨线程发布（当前所有发布点都在主线程）。

**验证**：`--print-events` 打出两个事件的值与订阅数；`--selftest 30` 里
`[Events] selfTest PASS`；`--selftest 60 --resize-test --validation-features sync`
连做 4 次交换链重建 + 2 次动态分辨率，`errors=0`、stderr 0 字节；
`--resize-test --screenshot` 在 5 次重建后仍能写出 1920x1080 PNG。

## 决策 39：惰性分配只探测不改分配（TM-18）

**问题**：移动端 GPU 的 tile memory 可以承载"用完即丢"的中间目标
（`VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` + `LAZILY_ALLOCATED` 内存），
省带宽也省显存。本项目的中间目标（HDR 场景、TAA 输出、Bloom 金字塔、SSAO 两张）
每帧都在，看起来是候选。

**先纠正一处**：手头资料里写的"格式支持惰性分配（`VK_FORMAT_FEATURE_LAZILY_ALLOCATED_BIT`）"
并不存在 —— 第一次实现就撞 `error C2065`。Vulkan 只有两条判据：

1. 驱动接受这个用法组合：`vkGetPhysicalDeviceImageFormatProperties(...,
   TRANSIENT_ATTACHMENT | COLOR_ATTACHMENT | INPUT_ATTACHMENT, ...)` 返回 `VK_SUCCESS`；
   深度目标要换成 `DEPTH_STENCIL_ATTACHMENT`，把深度格式按颜色用法去问会被直接拒绝
   （实测第一版就报"不接受"，属于问法错，不是能力缺失）。
2. 设备上真的有 `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` 的内存类型。

**本机实测（RTX 2070 Ti + AMD 核显，1920x1080）**：6 个候选格式第 1 条全部"接受"，
第 2 条是 **0 个**惰性内存类型 → 结论"用法被接受，但本设备没有惰性分配内存类型"。

**为什么不改分配**：没有那个内存类型时，`TRANSIENT_ATTACHMENT` 只会让驱动退化成
普通 `device-local` 分配 —— 显存一分不省，而"这个目标能不能和别的目标重叠"的
生命周期推理（TM-08 已经量过：本结构下可省 0）全都要重做一遍。收益为零、风险非零。

**做了什么**：`src/RHI/TransientProbe.h/.cpp`（两条判据 + 三种结论 + 空设备安全）与
`--probe-transient`（打印 6 个候选目标，然后直接退出，不改任何分配策略）。

**何时重新评估**：出现移动端 / 带 tile 的集显目标；或 WSI 变成 Android surface。
那时才需要把中间目标改成 `TRANSIENT_ATTACHMENT`，并按 TM-08 的生命周期区间挑目标。

**不引入的**：运行期按设备能力切换分配路径（两条路径都要维护，收益只在移动端）；
`VK_IMAGE_CREATE_ALIAS_BIT` 的实际绑定（那是 TM-08 第二步的事，已判定不做）。

**验证**：`--probe-transient` 打印 6 行表 + "本设备 LAZILY_ALLOCATED 内存类型：0 个"；
`--selftest 30` 里 `[Transient] selfTest PASS`、`errors=0`、stderr 0 字节；
`--selftest 60 --gpu-scene` 96.9 FPS、`errors=0`。

## 决策 40：聚簇建簇放在图形流里做，一次 dispatch 取代 CPU 逐灯扫描（TM-13 第二步，已修）

> **已修（决策 43）**：簇表登记进渲染图、暂存区改 CONCURRENT 之后，建簇已挪回异步 compute 流。下面「为什么走 addGraphicsPass」与「不引入的」两条只保留当时的理由。

**问题**：TM-13 第一步的建簇在 CPU 上做（按灯剪枝 + 计数排序）。512 盏灯时
Debug 构建要十几毫秒一帧（早期实测 18.3 ms），Release 也有约 1 ms —— 建簇成了
「聚簇到底划不划算」里最大的一项开销，而且它随灯数线性增长。

**做法**：`shaders/cluster_build.comp` 每个线程算一个簇（3456 个线程 = 54 个 workgroup），
反投影出视空间 AABB、遍历灯做球-盒测试，命中就写进**定长槽位**
（`base = clusterID * 256`），最后把 `uvec2(base, count)` 写进 ranges。CPU 一侧什么都不做，
只把参数块与三张表的描述符准备好。

**为什么走 `addGraphicsPass` 而不是 `addComputePass`**：`addComputePass` 会把 dispatch
录进异步 compute 命令流，而簇表走的是每帧暂存区（没有在渲染图里登记成纹理 / 缓冲区），
渲染图无法为它建立跨队列依赖 —— 这正是 TM-09 撞上的那面墙。ClusterBuildPass 因此
挂在图形流里、自己发一条 buffer 屏障（COMPUTE_SHADER/SHADER_WRITE → FRAGMENT_SHADER/SHADER_READ）。
将来如果把簇表登记进渲染图（`importBuffer` 或 `createOwnedBuffer`），可以再挪回 compute。

**定长槽位换什么**：省掉了「前缀和 + 游标缓冲 + 第二次 dispatch」这一整套，
消费者（`lighting.frag`）读法完全不变；代价是索引表固定占
`3456 x 256 x 4B = 3.4 MiB / 帧槽`（两个槽位合一 6.8 MiB）。灯多到需要调大每簇上限时，
这份静态占用还会跟着涨 —— 那时才值得换成前缀和。

**踩到的三个坑**（都写进代码注释）：

- **`clusterLights.length()` 会让驱动在 `vkCreateComputePipelines` 里直接 0xC0000005**。
  改成从参数块读 `counts.x`（CPU 侧写入），问题消失。结论：这个驱动上不要用 SSBO 的 `.length()`。
- **`stageFlags` 被后面的赋值覆盖**：Set 0 binding 15~18 的 `stageFlags` 加了
  `COMPUTE_BIT` 之后，循环末尾还有一行把它改回只有 `FRAGMENT_BIT`，于是验证层报
  `VUID-VkComputePipelineCreateInfo-layout-07988`（4 条）。顺序要放在最后。
- **暂存区中途扩容会换 buffer**：`ScratchArena::allocate` 在容量不够时新建 buffer 并把偏移
  归零，于是「一次分配四段、之后按偏移当同一块内存写描述符」的做法会失效
  （验证层报 `range-00342`，range 超过 buffer 大小）。修法是先 `reserve` 再分配，
  并在 Application 里加「四块必须同 buffer」的兜底（不同就退回 CPU）。
  顺带把旧块的回收时机改成 **只在 arena 析构时**：扩容淘汰的旧 buffer 可能已经被
  别的描述符集记住（典型是平面反射自建的 Set 0），提前销毁就是 UAF（验证层 08114，12 条）。

**实测（本机 RTX 2070，Debug，`--voxel-world 1 --light-stress 512 --clustered-lighting`，60 帧）**：
CPU 建簇 51.77 FPS（1.159 s / 60 帧），GPU 建簇 65.37 FPS（0.918 s / 60 帧），**+26%**；
两种模式 `--validation-features sync` 均 `errors=0`、stderr 0 字节；8 灯同机位截图
CPU vs GPU 逐像素差 213 像素，全部落在 HUD 的 FPS 读数区（x 102..157 / y 119..129），
3D 画面一致。

**默认关闭**：`--clustered-gpu-build` 加在 `--clustered-lighting` 之上，两者都不影响默认渲染路径。
聚簇本身仍默认关闭（决策 32），这一步只是把「将来翻默认」时最大的那项开销先搬走。

**不引入的**：前缀和 / 游标缓冲（定长槽位已经够了）；把建簇挪回异步 compute（等簇表进渲染图再说）。

## 决策 41：排序批处理的触发条件还没到（TM-15）

**问题**：TM-15 留了一个触发条件（前向 Pass 超过 5 个，或单帧透明物体超过 2000），
清单里只剩这一项没有结论。按维护方式，触发条件要么量、要么删 —— 不能一直挂着。

**实测（RTX 2070，`--selftest 30 --voxel-world 1 --fireflies 32`）**：

| 指标 | 数值 | 来源 |
|---|---|---|
| 每帧 draw calls | 107 | `[SelfTest] telemetry: draw calls 107 \| desc binds 5 \| triangles 336953` |
| 每帧 descriptor 绑定 | 5 | 同上 |
| 前向几何 Pass | **5**（Vegetation / Water / Glass / Particle / ViewModel） | 逐个 Pass 的流水线；Geometry 走 G-Buffer 延迟 |
| 单帧透明物体 | **≈912**（玻璃面 879 + 粒子 32 + 水面 1） | `[Glass] sorted=879 drawcalls=1`、`[Particle] alive=32 drawcalls=1` |

**结论**：两条触发线都没到（5 个不超过 5，912 远低于 2000）。更要紧的是
每帧只有 5 次 descriptor 绑定、107 次 draw —— 排序批处理能省的 state change
本来就在噪声里，透明通道（Water / Particle / Glass）各自已经在内部排过序。

**保留什么**：`docs/80-roadmap/borrowed-techniques.md` 的 TM-15 条目改成
「已量测，触发条件未到」并写清这两个数字与量法；将来物体数或前向 Pass 数上去了，
照这条自己复测即可。

**不引入的**：通用 `RenderQueue`（现在的提交路径是 GPU 驱动的，见
[gpu-driven-submission](../10-architecture/gpu-driven-submission.md)，再插一层 CPU 队列
只会和它打架）。

## 决策 42：自动曝光先做"GPU 算均值 + CPU 适应"（F6-02）

**问题**：场景整体亮度会在几十倍范围内变化（洞穴、雪地、512 盏灯的压力场）。
现在曝光值来自场景设置的常数，滑块在 `src/ImGuiSystem.cpp:222`，一换场景就得手调。

**做法**：
- `shaders/exposure_reduce.comp` 每帧一次 dispatch，按步长 8 采样（1920x1080 约 3.2 万点），
  把 `log2` 亮度量化成 `uint` 后 `atomicAdd` 进一个 8 字节的 SSBO。
- `src/Render/AutoExposurePass.*` 每帧开头读**两帧前**那一份统计（帧槽复用后才轮到它，
  `Renderer::beginFrame` 已经等过这个槽位，所以读它不卡流水线），
  算出 `target = 0.18 / avgLuma`，再按 `1 - exp(-dt * 1.5)` 在对数域靠近，夹到 `[0.05, 20]`。
- 曝光值通过 `FrameInfo::exposureOverride` 递给 `PresentPass`，**不写 `SceneSettings::exposure`**。

**为什么适应放在 CPU**：`1 - exp(-dt * speed)` 这条曲线是纯算术，`AutoExposurePass::selfTest`
能直接证伪"不过冲 / 时间步越大越快 / 目标不变时不漂 / 夹取 / 坏输入不出 NaN"；
放 GPU 就得靠截图反推，且第一帧还会撞上"GPU 写、GPU 读、CPU 不知道"的循环。

**为什么不写场景设置**：`EngineConfig` 会把 `exposure` 存进 `config/engine.json`（见
`src/Core/EngineConfig.cpp:185`）。让自动曝光写进去，等于跑一会儿就把用户配置改成当时那一刻的亮度，
上次相机位姿被写回文件已经踩过一次。

**为什么不用 `ScratchArena`**：这份缓冲要被 CPU 在两帧后精确读回同一份，
而暂存区会扩容换 buffer；自己建两个 `HOST_COHERENT` 的 8 字节缓冲更省心。

**代价与边界**：曝光值有 2 帧延迟（60 FPS 下约 33 ms），对"缓慢适应"没有影响；
每帧 3.2 万次采样 + 一次 dispatch，实测 `--selftest 60 --gpu-scene --auto-exposure`
104.7 FPS（比不开的 46.7~87 FPS 同量级，属运行间噪声）。
默认关（`--auto-exposure` 才开）—— 默认画面必须与没有这个功能时逐像素一致。

**不引入的**：直方图 + 百分位裁剪（现在用均值，极端高光会拉偏一点；要更稳再上直方图）、
曝光纹理（把曝光写进一张 1x1 纹理让片段自己读，那是要上 GPU 自适应时才需要）、
自动白平衡。

## 决策 43：把簇表登记进渲染图，建簇挪回异步 compute 流（T-02）

**问题**：决策 40 把建簇挂在图形流里，代价是异步 compute 队列只有探针在用。`crossQueue` 通路本身是通的
（`--cross-queue-test` 双向都过），但没有真实消费者，于是"多提交能省多少"这件事一直没有可验证的落点。
当时的理由是簇表走每帧暂存区、没在图里登记，跨队列依赖建立不起来。

**做法**：两块砖，都不用改 `Renderer`（c2g 的提交顺序早就在 `Renderer::endFrame` 里按依赖方向排好了）。

- **暂存区改成 CONCURRENT**：`src/RHI/ScratchArena.cpp` 的 `createSlotBuffer` 在
  `device.hasDedicatedComputeQueue()` 为真时设 `VK_SHARING_MODE_CONCURRENT` +
  `{graphicsFamily, computeFamily}`。两个队列族同时拥有，跨队列读不需要 ownership transfer。
- **簇表登记进图**：`ClusterBuildPass::addToGraph` 用
  `RenderGraph::BufferDesc{ name="Scratch.ClusterTables", size=clusterTablesExtent(binding), usage=STORAGE|UNIFORM, crossQueue=true }`
  把暂存区那一整块 `VkBuffer` 包成图内句柄。`desc.size` 取 params / ranges / indices / lights
  四段里最大的 `offset + size` —— `normalizeBufferRange` 按它夹取，写小了会直接抛
  `RenderGraph: buffer range exceeds buffer bounds`。
- **dispatch 挂 compute 流**：`graph.addComputePass("Clusters.Build")` + 两次
  `writeBuffer(clusterBuffer, COMPUTE_SHADER, SHADER_STORAGE_WRITE, {offset,size})`；紧随一个
  **只声明不执行**的 `graph.addGraphicsPass("Clusters.ReadSync")` + 两次
  `readBuffer(clusterBuffer, FRAGMENT_SHADER, SHADER_STORAGE_READ, ...)`。
  原来手写的两条 `VkBufferMemoryBarrier2` 全部删除。

**为什么用零附件零回调的 Pass 表达消费关系**：`CrossQueueProbePass` 的
`TD02.GraphicsHostReadBarrier` 早就证明这种 Pass 合法，图会照常为它推导 c2g 依赖并接管屏障。
它比"让 lighting.frag 自己撞上依赖"更明确：谁读、读哪一段、在哪个 stage，全写在一处，
`lighting.frag` 本身不需要改一行。

**为什么 handle 必须自己记住**：`beginFrame` 只清 `passes`，不清 `textures` / `buffers`。
每帧盲调 `importBuffer` 会让条目无限增长（`importBuffer` 不查重、不更新已有条目）。
`ClusterBuildPass` 因此保存 `RenderGraph::BufferHandle clusterBuffer` 与 `VkBuffer clusterBufferVk`，
只在底层 buffer 变化（暂存区扩容换 buffer）时重新登记。

**为什么不用 `createOwnedBuffer`**：簇表的物理内存来自暂存区（一次 `reserve` 后四段必须在同一 buffer），
图的 allocator 不参与；再建一份只会让"四块必须同 buffer"的兜底更难维持。

**验证**：压力场景（`--voxel-world 1 --light-stress 512 --clustered-lighting --clustered-gpu-build --stats-frame`，
60 帧）打出 `barriers/frame=61.2 calls 61.2 entries | skipped: sameState=35.8 crossQueue=2.0`。
`crossQueue=2.0` 就是 ranges / indices 两段改由 timeline 信号接管后跳过的两条缓冲屏障，
说明通路真的被走通了，不是"声明了但没人走"。同一条命令加 `--validation-features sync` 得
`errors=0 warnings=0`、stderr 0 字节。

**收益与代价**：收益不在 FPS，而在"异步 compute 有了第一个真实消费者"——T-03（屏障批处理）、
T-07（FramePlan 多提交点）之后要量的一切，现在有落点了。120 帧 A/B（`--screenshot-frame 90`）：
旧代码 GPU 建簇 58.61 / CPU 45.80 FPS，新代码 56.23 / 43.63 FPS —— 两侧一起降，
比例仍是 GPU 快约 29%，说明 T-02 没吃掉决策 40 的收益，但绝对 FPS 不能跨构建横比。
代价是暂存区所有 slot buffer 都变成 CONCURRENT：驱动可能因此选更保守的内存放置，本机测不出差异；
真要退回 EXCLUSIVE，就得实现 TM-09 判掉的那套 release/acquire。

**不引入的**：异步 compute 的预算与优先级调度（UE 的 `AsyncComputeBudget` 对应物，等有第二个真实
compute 消费者再谈）；把建簇之后的光照也拆进异步流（那会把 c2g 变成双向依赖，图会直接抛异常）。
## 决策 44：图像屏障只在 Pass 内按桶合并，帧级有序屏障留给 T-07

**问题。** `--stats-frame` 量出来的第一批数字里，`barriers/frame=45.2 calls 45.2 entries` —— 两个数
相等，说明每追加一条屏障就调一次 `vkCmdPipelineBarrier2`。根源是 `emitBarrierIfNeeded` 对
`range.baseMipLevel × range.baseArrayLayer` 双层循环，逐子资源各发一次。

**试过的第一版（无收益）。** 把循环拆成两遍：先逐子资源判定发/跳过并把桶键算好，再按
`(srcStage, dstStage, srcAccess, dstAccess, oldLayout, newLayout)` 分桶，每桶一次
`vkCmdPipelineBarrier2`。实测 `calls 45.2 → 45.2`，一点没降。用临时探针（`TINY_RG_TRACE`，
跑 `--selftest 2` 抓两帧）才看清原因：默认场景 21 个 Pass 的每一次资源访问都是
`subres=1 buckets=1` —— 每张图只有 1 个 mip、1 个 layer，桶里永远只有一个成员。
整帧唯一能合的是启动期 `IBL.Irradiance` 与 `IBL.Prefiltered` 的 6 级 mip 链（`subres=6 emitted=6`）。

**真正的重复在哪。** 不在同一张图的多个子资源，而在**同一个 Pass 里多张图的同一类转换**：
G-Buffer 的 Position / Normal / Albedo / Pbr / Depth 一起从 `COLOR_ATTACHMENT_WRITE` 转到
`SHADER_READ`，Bloom 上采样每级两次转换。这些都各自是一条 `VkImageMemoryBarrier2`，但键完全相同。

**做法（第二版）。** `emitBarrierIfNeeded` 不再就地发射，只把
`VkImageMemoryBarrier2` 追加进 `RenderGraph::pendingImageBarriers`；录制循环在
`for (const Access& acc : prepared.accesses) emitBarrierIfNeeded(...)` 之后调
`flushImageBarriers(passCmd)`，由它挑出队尾那条当桶样板、把同键成员收进 `batch`
（用「与队尾交换 + `pop_back`」剔除，不做 N 次 `erase`），每桶一次
`vkCmdPipelineBarrier2`。缓冲屏障仍是就地发一条（每次访问只有一段）—— `emitBufferBarrierIfNeeded`
没改。

**结果。** `entries` 不变（正确性判据），`calls` 从 45.2 / 61.2 降到 38.0 / 47.0
（-16% / -23%）；`--validation-features sync` 两场景 `errors=0`。同批次 A/B 跑
`tools/bench.ps1 -Scenario lights-512 -Runs 3 -Frames 600`：合并前 89.62 FPS、合并后 89.71 FPS。

**结论要写清：-23% 的屏障调用没有换成 FPS。** 这个场景里 47 次 / 帧的屏障调用不是瓶颈，
瓶颈在别处（P3 的状态切换、P2 的每帧摊平）。所以这一单的价值不是性能，而是两件结构性的事：
屏障发射独立成一个翻译单元（`src/RHI/RenderGraphBarrier.cpp`），以及「屏障可以先攒后发」
这条通路存在了 —— T-07 把提交点从 1 个图形 + 1 个计算变成 3 个之后，真正需要的是在提交点
之间搬运屏障，而不是在单个 Pass 内省调用。

**为什么只合到 Pass 边界为止。** 跨 Pass 合并要求能证明「中间没有任何 Pass 碰这张图」，
而那需要帧级的有序屏障记录（UE RDG 那套：先编译出整帧的 barrier 列表、再按提交点分段插入）。
那件事的前提是提交点本身已经分段，所以排在 T-07 之后，不在这里抢跑。

**代价与边界。**
- `entries` 与 `calls` 从「恒等」变成两个不同的数，`baseline.md` 的列名相应改成「屏障调用/帧」，
  并在列含义里写明哪个是正确性度量、哪个是收益度量。
- 桶内成员的顺序不保证与原声明顺序一致（同一批屏障互不重叠，顺序不影响正确性）；但
  `prepared.accesses` 的状态回写仍严格按声明顺序，未改动。
- `pendingImageBarriers` 每 Pass 被 `flushImageBarriers` 清空，`beginFrame` 再清一次兜底 ——
  攒着不清会让下一帧的 Pass 莫名带上上一帧的屏障。

**不引入的**：帧级有序屏障记录（等 T-07）；把缓冲屏障也做成攒后发（每次只有一段，收益为零）；
异步屏障队列 / `vkCmdPipelineBarrier` 的 secondary 命令缓冲版本（本机没有第二个真实消费者）。
## 决策 45：图外资源要显式声明「进来的状态」和「交回去的状态」（T-01）

**问题。** `importTexture` / `importBuffer` 只把 `VkImage` / `VkBuffer` 包成句柄，图知道它叫什么、
多大、是不是跨队列，但不知道它**进来时**处于什么 stage/access。于是第一处使用它的 Pass 被当成
「从 NONE 开始」：屏障照发，不过发的是从 `VK_PIPELINE_STAGE_2_NONE` 出发的屏障 —— 真正的前一个
使用者（上一帧的命令、别的提交点、或者图外自己录的那一段）没有被这次屏障覆盖。截图缓冲、
`ShadowPass` 自己管的阴影图、暂存区里的聚簇表都站在这条缝上；区别只是它们此前都没接进图。

**做法。** 加一组最小 API：`declareExternalTexture(handle, start, end)` /
`declareExternalBuffer(handle, start, end)`，入口出口都是 `ExternalState{ layout, stage, access }`
（buffer 忽略 `layout`）。声明做三件事：把资源状态设成 `start`、记下 `end`、`endFrame` 尾部自动
把资源推到 `end` 并记为下一帧的期望入口。

三条不变量，违反直接抛 `std::runtime_error`：

- 每资源每帧最多声明一次（`externalDeclared`）；
- 声明必须早于本帧第一次使用（`externalUsed` 在状态回写时置位）；
- 本帧 `start` 必须等于上一帧交回的 `end`，否则报「状态漂移」，用 `layoutName` 把两边 layout 打出来。

**为什么「上一帧交回了什么」必须存在图里。** 只有图自己见过这个值，漂移才检得出来。让调用方
自己记入口状态，等于把不变量下放给每个调用点 —— 而漏记的那一刻不会有任何现象，直到某帧画面
开始间歇性闪。

**为什么交回屏障发在 graphics 流上。** 外部持有者就在 graphics 侧消费（交换链图像、截图回读、
阴影图采样）。图里没有「外部资源归哪个队列」的信息，选 graphics 是当前唯一的保守解；
真要跨队列交回，得先给 `ExternalState` 加队列字段，那是 T-07 拿到提交点信息之后的事。

**验收：负向 + 正向。** `--cross-queue-test` 的探针现在自建一块图外 buffer（VMA 创建，
`STORAGE|TRANSFER_SRC|TRANSFER_DST`，`hasDedicatedComputeQueue()` 时 CONCURRENT 两族共享）：

- 负向：等「声明 + 交回」跑满一轮，故意拿 `(VERTEX_SHADER, SHADER_READ)` 当入口再声明一次，
  必须抛「状态漂移」；没抛就由探针自己 `throw`。通过时打印 `[T01] external buffer drift detection verified`。
- 正向：compute 流 `vkCmdFillBuffer` 填 64 个 `0xA5A5A5A5`，图形流 `vkCmdCopyBuffer` 拷进图自己
  持有的 host-visible 回读缓冲，隔 `kFramesInFlight` 帧 CPU 逐字比对。通过时打印
  `[T01] external buffer verified: ... 64 words`。
- 结果：`--selftest 40 --cross-queue-test --validation-features sync` → `errors=0 warnings=0`、
  stderr 0 字节、EXIT 0；默认场景 150 帧 + 截图 + sync 仍是 `38.0 calls / 45.1 entries /
  sameState=18.9`，与 T-03 之后逐位一致 —— 没有调用者声明时它一分钱不花。

**代价与边界。**
- 只有声明过的外部资源受保护；没声明的导入资源仍然是「从 NONE 开始」。
- 漂移检查按 `(layout, stage, access)` 全等比较。入口写成更宽的 stage（例如 `ALL_COMMANDS`）
  会被判成漂移，而它可能是合法的保守写法。第一版故意严格：放宽等于放弃检查。
- 交回屏障不参与 T-03 的攒批（`endFrame` 里直接 `emitBarrierIfNeeded` 再 `flushImageBarriers`），
  因为它发生在所有 Pass 之后，不属于任何 Pass。

**不引入的**：外部资源的自动发现（扫 VMA 台账）；跨队列交回（等 T-07）；`ExternalState` 里带
队列族（同上）；把 `declareExternal*` 包成 RAII 持有者（图的 API 是「声明」不是「持有」）。
## 决策 46：RT 池默认关，先砍「每帧复制」再谈缓存（T-04）

**问题。** T-04 的验收原文是「稳定态每帧 `vkCreateImage` 为 0、显存不增长」，暗示稳态在
反复重建渲染目标。先量再动手：默认场景 600 帧的 `[Resources]` 是 `images/frame=0.27
total=160 peakFrame=160` —— 160 张图全在启动期建好，稳态每帧 0 张。判据的一半在 T-04
开工之前就已经成立。

真正的重建发生在尺寸变化：`--resize-test`（60 帧、6 次尺寸变化）得 `total=424`，
即一次尺寸变化约 44 张图，全部落在 `vkDeviceWaitIdle` 之后那一帧。

**做法。** 写跨帧 RT 池 `src/RHI/RenderTargetPool.*`：键是 `格式 + 精确尺寸 + usage +
mip/layer + createFlags`，释放时记帧号、过 `kRetireFrames=2` 帧才可复用，空闲超
`kMaxIdleBytes=256 MiB` 按最久未用淘汰。`RenderGraph::createOwnedTexture` 改成从池里
拿，`resizeOwnedTexture` 改成交回池。整体回退开关 `--rt-pool`，**默认关**。

**结果（同机同构建，60 帧，相机固定）**

| 配置 | images 总计 | heap0 末值 | 命中率 | 未命中 |
|---|---|---|---|---|
| 池关（回退） | 424 | 1220.7 MiB | — | — |
| 池开（256 MiB） | 367 | 1476.7 MiB | 24.8%（57/230） | 173 |
| 池开（512 MiB） | 360 | 1476.7 MiB | 27.8%（64/230） | 166 |

池能把一次回切的创建量砍掉一半左右，但代价是 256 MiB 常驻空闲图，帧时间没有可辨收益
（p95 与 max 在几次运行间本来就在 39~70 ms / 85~106 ms 之间漂）。

**为什么默认关。** 命中率只有 24.8%：6 次尺寸变化里只有「切回 1920x1080」那两次能命中，
窗口拖动产生的一串中间尺寸几乎全是未命中。用户付 256 MiB 换 13~15% 的创建量下降，
这笔账不划算。池留着是因为它的真正价值在两个还没落地的场景：T-05 的瞬时目标（同尺寸
反复借还）与 T-07 的多提交点（同一帧内的中间目标）。

**下一步该动的地方（本次量出来的）。** 池的 top creators 全是成对的名字：
`SSAO.Raw.Frame0 / SSAO.Raw.Frame1` 各 6 次、`SSAO.Frame0 / .Frame1` 各 6 次、
`TAA.History0 / History1` 各 6 次、`Reflection.Position.0 / .1`……也就是 44 张里
大约一半是「同一张图按 kFramesInFlight=2 复制两份」。写在本帧、读在本帧的目标（SSAO、
Bloom 各级、SSR、Reflection 的中间图）没有理由跨帧复制 —— 只有 TAA 历史、上一帧深度
这类「下一帧还要读」的才必须双份。把前者的复制去掉，每帧驻留的图会接近减半，
resize 的创建量随之从 44 掉到 25 上下，而且省的是常驻显存，不是缓存。

**不引入的**：按 `VkImage` 之外再缓存内存绑定（VMA 已经做了）；跨进程/跨设备的池序列化；
按字节 budget 做 LRU 的复杂策略（这次只做了「超预算淘汰最久未用」）；把池做成全局单例
（图与 Device 同寿命，多 Device 时会串味）。
## 决策 47：同帧目标合成一张，代价是让 GPU 侧两帧排开（T-04b）

**问题。** D-4 给每个「帧内瞬时目标」都留了 `kFramesInFlight = 2` 份，理由是两帧同时在飞。T-04 量出启动期 160 张图、`heap0` 1220.7 MiB，其中 15 张是全屏级的重复（HDR 与 TAA 输出各 16.6 MB）。

**先量再合。** 直接合成一张在同步验证层下报 `errors=120`：两帧确实重叠。所以问题不是"该不该合"，而是"能不能让两帧不重叠"。加一条提交侧依赖——`submitGraphics` 多等一次 `graphicsTimelineSemaphore` 的 `signalValue - 1`——之后，同帧目标合成一张就成立了。

**结果（600 帧 × 3 次，同批次）。**

| 配置 | lights-512 | default | 图 | heap0 |
|---|---|---|---|---|
| 回退 | 87.77 FPS | 116.93 FPS | 160 | 1220.7 MiB |
| 只加帧间等待 | 91.13 | — | 160 | 1220.7 MiB |
| 合成 + 等待（默认） | 92.50 | 118.90 | 145 | 964.7 MiB |

**为什么默认开。** 这条改动同时是"更快"和"更省"：+3.8% 来自排开两帧，+1.5% 与 −256 MiB 来自合并。反直觉，但可解释——两帧重叠时在争同一批带宽，而这个引擎的 GPU 工作量（约 10 ms）与 CPU（约 8 ms）本来就不平衡。

**为什么合到这些、不合别的。** G-Buffer 不合：Hi-Z 用上一帧的深度重建，需要前一份内容。TAA 历史不合：它本身就是两帧 ping-pong。聚簇表不合：它是暂存区的整块 buffer，走 `importBuffer`。

**代价。** GPU 侧不再重叠两帧，单帧 GPU 时间变长时会直接顶到帧时间上；`--no-single-frame-targets` 是整体回退开关，`--no-serialize-frames` 只用于复现"两帧重叠"这个事实（会故意制造竞争）。另外 `images/帧` 这个数的口径变了（160 → 145），跨版本比较要注意。
## 决策 48：管线绑定缓存按命令缓冲记账，冗余绑定归零但不指望它涨帧率（T-09）

**问题。** `--stats-frame` 长期显示 `pipelineBinds/frame=23.5 redundant=10.5 (44.7%)`：每帧近一半的 `vkCmdBindPipeline` 是把已经绑着的管线再绑一次。最集中的一处是 `src/Render/GeometrySystem.cpp` 的「地形合批（1314）→ GPU 场景合批（1355）→ 体素合批（1379）」三段，同一个 `geometryPipeline` 连绑三次。

**做法。** 新增 `src/RHI/StateCache.h/.cpp`（`tiny::statecache`）：按 `VkCommandBuffer` 分别记「当前绑着哪个图形管线、哪个计算管线」，同一个就不再发指令。`Pipeline::bind` / `ComputePipeline::bind` 是唯一的两个绑定入口（全仓只有这两处 `vkCmdBindPipeline`），所以改这两个函数就够了。

**为什么必须按命令缓冲记账。** 管线绑定是命令缓冲的状态，不是全局状态。图形与计算是两条命令缓冲，各自记各自的；否则「计算流刚绑过 X，所以图形流不用绑 X」会导致图形流根本没绑上。缓存在 `RenderGraph::beginFrame` 里清空——和 `framestats::beginFrame` 同一行，两张表因此可以互相校验。

**为什么还要 `invalidateAll()`。** `VkPipeline` 销毁后句柄值可能被驱动复用，缓存里"已绑 X"就指向了一个已经不存在的对象。所以 `Pipeline` / `ComputePipeline` 的析构里各调一次，宁可在重建后多绑一次。热重载走的正是这条路径。

**哪些命令缓冲可以记账。** 缓存只对**显式登记过**的命令缓冲生效（`statecache::trackCommandBuffer`）：引擎的两条主缓冲由 `beginFrame` 自动登记，地形构建（`src/Render/TerrainComputeGenerator.cpp`、`src/Render/TerrainGpuMeshBuilder.cpp`）、纹理上传（`src/RHI/Texture.cpp`、`src/RHI/Texture3D.cpp`）在 `vkBeginCommandBuffer` 之后各登记一次。没登记的缓冲一律照发 —— 它们可能刚被 reset 过，缓存里的记录不可信。这条规则是踩出来的：第一版对全部缓冲记账，`tools/Validate.ps1` 里 35 个地形/流式用例直接 `exit code -1073741819 (0xC0000005)`，因为地形构建缓冲每帧 reset 后句柄不变，缓存以为"管线还绑着"，整段派发一条管线都没绑。

**两个数字必须对得上。** 第一版把 `beginFrame` 放在 `Renderer::beginFrame`，两张表的窗口错开，`bindCalls` 比 `pipelineBinds` 每次多 1.5（启动期的 IBL / 纹理上传发生在第一帧之前，只进了 `g_total` 没进任何一帧）。改成同一处开窗、并在第一帧把 `g_total` 清零之后两表逐位相同：

| 场景 | 绑定/帧 | 真正发出 | 省掉 | 比例 |
|---|---|---|---|---|
| default（60 帧） | 23.5 | 13.0 | 10.5 | 44.7% |
| `--resize-test` | 23.3 | 13.0 | 10.3 | 44.3% |
| `--cross-queue-test` | 25.5 | 15.0 | 10.5 | 41.2% |
| lights-512 | 39.5 | 20.0 | 19.5 | 49.4% |
| lights-512 + `--no-pipeline-cache` | 39.5 | 39.5 | 0 | 0% |

**收益与代价。** 省掉 10~20 条 `vkCmdBindPipeline`，同批次交替跑（lights-512，600 帧 × 3 次，开关交替各三次）中位数 93.92 对 91.76 FPS，即 +2.4%；跨批次与 `docs/70-status/baseline.md` 的 94.56 比没有意义（批次间漂移约 4%）。这条改动的价值是**把"冗余绑定"这个数从 44.7% 打到 0，并给出一个以后不会再回来的基线**，以及为后续描述符/顶点缓冲的状态缓存留出同一个落点。不顺手的部分：`--stats-frame` 现在多一行 `[StateCache]`，两表的绝对条数必须一致才对，不一致就说明窗口或记账错了；`--no-pipeline-cache` 是整体回退开关，关掉后 `unmanaged` 会等于全部绑定、`saved=0%`。

**不引入的。** 没有做"按管线排序重排绘制"（那是 TM-15 的排序批处理，触发条件还没到）；没有做描述符集缓存（`desc binds/frame` 只有 2，没有可省的量）。

---

## 决策 49：Pass 剔除只认「图内读声明」，默认关（T-05）

### 问题

渲染图按声明顺序线性执行，不做重排，也从不跳过 Pass。于是只要某个特性的 Pass 被加进图、但这一帧没人读它的输出，它仍然会跑满整条管线：attach、清屏、绘制、屏障一个不少。UE 的 RDG 有 `GRDGCullPasses` 做这件事，T-05 的目标是补上同样的能力。

### 做法

`RenderGraph::cullDeadPasses()` 在 `validateWriteCoverage()` 之后、选 `activePasses` 之前跑，两轮：

1. **定根**（必须留下的 Pass）。六条：没有回调的纯声明 Pass（`Clusters.ReadSync` 这类，靠它建立跨队列依赖）；一条访问声明都没有（可能有图外副作用，看不懂就不动）；最后一个 Pass（present 转换）；写 persistent 纹理；写 host-visible 缓冲；以及两条「图看不见的消费者」——**在图上什么都没写**的 Pass（HiZ 建在 `SceneGpuBatch` 的自持图像里，外面的剔除 compute 读它），和**写了导入资源**的 Pass（`owned=false`：交换链、编辑器视口、簇表）。
2. **反向闭包**。对每个活着的 Pass，把所有「带读位的访问」逐条找最近的重叠写者，标记之，直到不动点。带读位而不是「无写位」，是这里最容易写错的一处：`writeColor(..., LOAD_OP_LOAD)` 的访问位含 `COLOR_ATTACHMENT_READ`，它是读改写；只按「无写位」判读会把它当作纯生产者，于是它的上游被整条判死。

被剔除的 Pass 与 `setEnabled(false)` 走**同一条记账路径**（`lastFrameSkippedPasses` + `countPass(false)`），`--pass-cull-dry-run` 只打印不跳过。

### 结果

| 配置 | `passes/frame` | `culled/frame` | errors |
|---|---|---|---|
| 默认 | 21.0 | 0.0 | 0 |
| `--pass-cull` | 21.0 | 0.0 | 0 |
| `--pass-cull --no-bloom` | 14.0 | 0.0 | 0 |
| `--pass-cull --validation-features sync --screenshot` | 21.0 | 0.0 | 0 |

**一条都没剔掉。** 原因是本引擎的 Pass 在特性关闭时**根本不进图**（`BloomPass::addToGraph` 首先看 `frameInfo.settings.bloomEnabled`），而不是「进了图但输出没人读」。计划里那条验收判据「`--no-bloom` 时图内 Pass 数减 7」在开工前就成立（21 → 14），但它归功于 Pass 自己不进图，与剔除无关。

### 为什么默认关

省不下来任何东西，却引入一类静默失败：新写的 Pass 只要通过预建描述符集采样上游、而那条读没有进 `pass.read(...)` 的声明，它的上游就会被判成死 Pass 剔掉，画面变黑而验证层不会报错。这条风险在实现过程中被实测撞到两次（LOAD 附件、导入资源），所以默认关、`--pass-cull` 显式打开，作为「未来某天真的出现死 Pass」时的兜底工具。

### 踩到的三个坑

1. **LOAD 附件是读**：不加这条，`Glass` / `Geometry` / `Lighting` / `SSAO` 全被判死。
2. **导入资源与「图上没写东西」的 Pass 都必须是根**：不加这条，Bloom 整链、`Present`、`HiZBuild` 被判死（`Present` 写的是 `importTexture(VK_IMAGE_LAYOUT_UNDEFINED)` 导入的交换链图，`persistent` 为假，只能靠 `owned=false` 认出来）。
3. **开关必须在 `renderGraph` 创建之后设**：一开始写在 `renderer` 初始化旁边（`renderGraph` 还没建），四次运行全部 `0xC0000005`，`stderr` 0 字节、日志停在 `[Renderer] D-4/D-5 active`。

### 不引入的

- 不按「输出被谁读」重排 Pass（T-07 的 FramePlan 才动执行顺序）。
- 不做跨帧的剔除结果缓存（第一版每帧重算，21 个 Pass 的闭包开销可忽略）。
## 决策 50：图形拆成两段提交，让建簇计算藏进 G0（T-07）

### 问题

`--stats-queue` 装上之后第一次量 512 灯场景（`--clustered-gpu-build`，sync 验证层）：`makespan=8.65 ms`、`graphics busy=8.36 ms`、`compute busy=0.25 ms`、**`overlap=0.00 ms`**。计算队列每帧只忙 0.25 ms，而且一点都不重叠。

原因不是调度器：同帧存在 c2g 依赖（建簇写簇表、光照读簇表），而每帧的图形命令只有**一次**提交 —— 整条图形流都在等 compute 跑完，0.25 ms 纯串行躺在关键路径上。

### 做法

把图形命令拆成两个命令缓冲，按 `G0 -> C0 -> G1` 三次提交：

- **G0**：第一个「读 compute 写过的资源」的 graphics Pass 之前的所有图形工作。等交换链图像（若开 `serializeGraphicsFrames` 再等上一帧图形），**不签** timeline。
- **C0**：compute 命令缓冲。不等 G0（依赖方向是 compute -> graphics），签 `computeTimelineSemaphore`。
- **G1**：其余图形工作。等 compute 的 timeline（c2g 时），签 `renderFinished` 与 `graphicsTimelineSemaphore`。

拆分点由图自己算（`RenderGraph::endFrame` 里）：先收集本帧所有 compute 写（纹理与缓冲，按 id + 子资源/字节范围），再顺序扫 graphics Pass，第一个「带读位且范围重叠」的就是拆点。三条保守规则：

- 拆点是第 0 个 graphics Pass 时不拆 —— 前面没有可重叠的工作，拆了只是多一次提交。
- `--no-async-split` 时 `setSecondaryGraphicsCommandBuffer(VK_NULL_HANDLE)`，整套退回单次提交。
- 默认场景没有 compute 写，`secondaryGraphicsUsed=false`，提交路径与改造前完全一致。

### 结果

512 灯场景、60 帧、sync 验证层（同一台机、同一构建）：

| 配置 | makespan | graphics busy | compute busy | overlap |
|---|---|---|---|---|
| `--no-async-split` | 10.06 ms | 9.73 ms (96.8%) | 0.27 ms (2.7%) | 0.00 ms (0%) |
| 默认（拆分） | 9.03 ms | 9.03 ms (100.0%) | 0.51 ms (5.7%) | 0.51 ms (100%) |

不叠验证层、600 帧 × 3 轮交替（`--selftest 600`，512 灯）：开 101.79 / 100.95 / 103.25，关 100.55 / 101.43 / 100.83 → 均值 **102.00 对 100.94（+1.05%）**。收益不大，但和「compute 只占帧时间的 2.5%、现在被完全遮住」对得上账。

默认场景（没有 compute 工作）的 `[FrameStats]` 与截图都逐位/在噪声内不变：`passes/frame=16.0`、`errors=0`；`on` 与 `off` 的截图差异 `mean=0.0262`，而同一二进制两次运行的噪声地板是 `mean=0.0173`。

### 代价与边界

- 每帧多一次 `vkQueueSubmit`（有拆点时三次，否则仍是三次：G0/C0/G1 合并回两次？不 —— 无拆点时走老路径，仍是两次）。
- G1 不签 timeline 之前，`getCompletedGraphicsFrame()` 的语义不变（仍是每帧一个值），因为只有 G1 签 `graphicsTimelineSemaphore`。
- g2c 方向（compute 读图形输出）本帧不拆：拆分前提是「图形前半段不依赖 compute」，g2c 时前半段的选择不成立。当前只有 `--cross-queue-test` 探针会走 g2c。
- 拆分只解决「同一帧内 compute 被图形遮挡」；跨帧重叠仍受 T-04b 的 `serializeGraphicsFrames` 约束（那是为了让同帧目标只留一张）。

### 不引入的

- 不做任意多段提交（FramePlan 的完整形态）。两段已经能覆盖「一个生产者 + 一个消费者」的真实场景；更多段要先把 Pass 依赖图与资源生命周期一起搬进图，那是 P1 之后的活。
- 不引入 `VK_KHR_synchronization2` 之外的队列优先级 / `vkQueueSubmit2` 权重调整 —— 本机两族队列，收益无法验证。

## 决策 51：帧间交接走图级黑板，`FrameInfo` 不再当万能总线（T-06）

### 问题

`FrameInfo` 原本承担"每帧把所有东西递给 Pass 与 UI"的职责，字段只增不减。到 T-06 开工时，里面躺着八个**只有一个生产者（`Application`）、消费者只有三四个**的交接字段：

| 字段 | 生产者 | 消费者 |
|---|---|---|
| `renderAlpha` / `renderTick` | `Scene`（编辑器播放态） | `GeometrySystem`、`SceneGpuBatch` |
| `exposureOverride` | `AutoExposurePass::update` | `PresentPass` |
| `editorSelection` | `EditorLayer` | `EditorOverlayPass` |
| `onSaveConfig` / `onLoadConfig` / `onBuildEditorUI` / `isEditorMode` | `Application` | `ImGuiSystem` |

每加一个这样的交接点，就要改 `FrameInfo` 的定义、所有聚合初始化点、以及"谁负责填"的约定 —— 而 `FrameInfo` 是**聚合初始化**（位置对应），插在中间会让后面所有字段错位，编译期报一堆莫名其妙的类型转换错误。这条成本每次都付。

### 做法

新增 `src/RHI/RenderGraphBlackboard.h`（header-only）与 `src/Render/FramePayloads.h`：

```cpp
class tiny::RenderGraphBlackboard
{
public:
    template <typename T, typename... Args> T& getOrCreate(Args&&... args);
    template <typename T> T* find();
    template <typename T> const T* find() const;
    template <typename T> void set(T value);
    template <typename T> bool has() const;
    void clear();
    std::size_t size() const;
};
```

- 键是 `std::type_index`，值是 `Entry{ void* object; void (*destroy)(void*); }` —— 析构函数在创建时按 `T` 记下来，`clear()` 时逐个调。
- `RenderGraph::getBlackboard()` 暴露给 Pass；`beginFrame()` 的第一件事是 `blackboard.clear()`，所以**载荷生命周期严格一帧**。
- 四个载荷类型放在 `src/Render/FramePayloads.h`：`ExposureOverride{float}`、`SelectionHighlight{entt::entity}`、`RenderInterpolation{float alpha; uint64_t tick;}`、`ConfigCallbacks{ save/load/buildEditorUI/isEditorMode }`。
- `FrameInfo` 只留一个句柄 `tiny::RenderGraphBlackboard* blackboard`，给**没有 RenderGraph 的消费者**用（`ImGuiSystem`）。八个旧字段全部删除。

顺带必须改的一处时序：`imguiSystem->prepareDrawData(frameInfo)` 原本在 `renderGraph->beginFrame(...)` **之前**，而 `beginFrame` 会清空黑板 —— 不改的话面板永远读到空载荷。现在它挪到"黑板填好之后、`renderPipeline.registerAll` 之前"。

### 为什么

- **生产者与消费者的对应关系变成局部知识**：加一个交接点只动 `FramePayloads.h` 的类型与两个调用点，不动 `FrameInfo` 的布局。
- **类型即键，不需要字符串注册**：`find<T>()` 在编译期就确定，写错类型是编译错误而不是运行期空值。
- **生命周期一帧是刻意的**：黑板不承担跨帧状态（TAA 历史、曝光适应曲线这类仍在各自的 Pass 里），避免变成"第二个全局变量池"。

### 代价与边界

- 每帧 `clear()` 会把载荷 `delete` 一次、下一帧再 `new` 一次。`ConfigCallbacks` 里四个 `std::function` 每帧一次分配，实测在噪声内（三个场景的 `[FrameStats]` 与 T-07 后逐位一致），但如果将来放重对象要考虑池化。
- 只有拿到 `RenderGraph` 的消费者能直接用 `getBlackboard()`；`ImGuiSystem` 这类不持有的，靠 `FrameInfo::blackboard` 句柄 —— 所以 `FrameInfo` 还留着这一个指针字段，`FrameInfo.h` 因此需要前置声明 `tiny::RenderGraphBlackboard`。
- 黑板**不校验写入者**：两个 Pass 写同一类型时后写的覆盖前面的。约定是"一个类型一个写者"，没有运行期检查。

### 踩过的坑（值得单独记）

第一次是一次性改完九个文件，编译过了但启动即崩：`EXIT=-1073741819`（0xC0000005）、stderr 0 字节、日志停在自检开始处。回退后分四步重做，最后定位到 `ImGuiSystem::renderConfigPanel`：`callbacks` 的四行声明被插到了

```cpp
if (!ImGui::CollapsingHeader("Config", ImGuiTreeNodeFlags_DefaultOpen))
    return;
```

这条 `if` 与它那句 `return;` **之间** —— 等价于把 `return` 挪到了声明之后，后面的 Save/Reload 读到未初始化的指针。教训：往"没有花括号的单语句 `if`"中间插代码，会静默切断控制流；插之前先把它改成花括号块，或者把声明放到函数体第一行。

### 验证

- 默认场景 40 帧 + `--validation-features sync`：`EXIT=0`、`errors=0 warnings=0`、`stderr 0` 字节；`[FrameTime] median=13.12ms p95=41.62ms max=95.70ms`、`[Resources] total=145 peakFrame=145`、`[SceneGpu] visibleRatio=0.61`。
- `--cube-field 10000` 120 帧：`85.45 FPS`、`median=8.62ms p95=29.88ms max=132.36ms`、`instances/frame=10063`、`visibleRatio=0.62`、`errors=0`。
- `--cross-queue-test`：`[TD02]` 两个方向 + `[T01]` 两条断言全绿、`errors=0`、`stderr 0` 字节。
- 三个场景的 `[FrameStats]`（`passes/frame`、`pipelineBinds`、`redundant`）与 T-07 之后逐位一致 —— 黑板没有改变渲染图的录制内容。

### 不引入的

- 不做跨帧黑板：生命周期一帧就是它的定义，跨帧状态有明确所有者（Pass 自己的成员）。
- 不做字符串键或反射注册：类型即键已经够用，字符串键会把拼写错误推到运行期。
- 不把 `Scene` / `EngineSettings` 搬进黑板：它们有明确的生命周期与所有者，进黑板只会变成第二个全局变量池。
## 决策 52：先给实例与图元一个跨帧编号，再谈实例级剔除（T-08 只读骨架）

**问题。** P2 要「visible/submitted ≤ 0.6」和「每帧摊平 CPU 时间 −70%」，但今天一万个立方体的世界矩阵每帧都在 CPU 侧重新拼成一个 `instanceScratch`（`--cube-field 10000` 实测 `flattenUs/frame` 在 18~60 ms），GPU 侧拿到的实例编号随帧漂移 —— 上一帧的编号在这一帧可能指向另一个物体，所以任何"按实例记忆上次结果"的东西（Hi-Z、LOD 选择、页标记）都无从谈起。

**做法（只读骨架，不改任何渲染路径）。** 新增 `src/Render/GpuScene.{h,cpp}`：

- `Primitive` 64 字节（`boundsMin/boundsMax` 两个 `vec4` + `draw = firstIndex/indexCount/vertexOffset/materialSlot`），`Instance` 144 字节（`transform`、`normal` 两个 `mat4` + `ids = primitiveIndex/instanceInPrimitive/flags/保留`）。
- `ensurePrimitive(uint64_t key, ...)` / `ensureInstance(uint64_t key, ...)`：键相同就 `memcmp` 比对，内容没变不产生脏区间；键不存在则分配新编号。图元键是 `(模型指针 >> 4) ^ (子网格 << 48) ^ 0x51ED2701`，实例键是图元键再混一个黄金比例常数乘以组内序号。
- `flush(cmd)`：把本帧脏区间 `memcpy` 进一块一次性 host-visible 暂存缓冲，再用 `vkCmdCopyBuffer` 搬到持久缓冲。不用 `vkCmdUpdateBuffer` 是因为它有 64 KB 上限，一万实例一次 1.4 MB。
- 缓冲容量从 256 KB 起按 2 倍扩，`usage | TRANSFER_DST`，`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`。

接线只有三处：`SceneGpuBatch::buildInstances` 在把 `buckets` 拼进 `instanceScratch` 之后镜像一份进 `GpuScene`；`SceneGpuBatch::prepare` 开头 `beginFrame()`、`cullPipeline->bind` 之前 `flush(cmd)` + `endFrame()`；`Application` 在 `renderPipeline.initAll()` **之后**把开关落到 `SceneGpuBatch` 上。

**为什么开关要放在 `initAll()` 之后。** `SceneGpuBatch` 是在 `GeometryPass::init()` 里 `std::make_unique` 出来的，而 `initAll()` 之前所有 Pass 的成员都还是空的。第一次把 `setEnabled` 写在 `geometryPassPtr = geoPass.get();` 之后，`getSceneGpuBatch()` 返回 `nullptr`，启动期直接 `0xC0000005`，日志停在 `[Tiny] 材质就绪`，stderr 0 字节 —— 和 T-06 那次一样，崩溃点只能靠"日志最后一行在哪"来夹。

**结果（60 帧、`--cube-field 10000`、Debug）。**

| 读数 | 关（`--no-gpu-scene-persist`） | 开（默认） |
|---|---|---|
| primitives / instances | 0 / 0 | 104 / 10103 |
| dirtyFrames | 0.0% | 1 帧（首帧建结构） |
| upload | 0.0 KB/frame，0 次 | 35.6 KB/frame（摊到 40 帧），1 次 |
| resizes | 0 | 2（0.5 → 2.2 MiB） |
| 渲染结果 | 一致 | 一致（`errors=0 warnings=0`） |

默认场景（Sponza）是 `primitives=103 / instances=103`、`dirtyFrames=1/120`、`upload=0.2 KB/frame` —— 静态场景的结构只上传一次，稳态上传量接近 0。

**代价与边界。** 骨架阶段没有任何消费者：`primitiveBuffer/instanceBuffer` 建了但没被任何着色器绑定，剔除仍走原来的 `GroupRecord` 路径。这批缓冲占 2.2 MiB 显存（cube-field）/ 0.5 MiB（Sponza），换来的是"编号跨帧稳定"这个不变量 —— 下一步 P2 的实例级剔除可以直接在这个编号上记账。

**踩坑。** `GpuScene.h` 第一次只在头里写了 `class Device;`，用 `VkCommandBuffer`/`VkBuffer`/`VmaAllocation` 却忘了 `#include <vulkan/vulkan.h>` 与 `<vk_mem_alloc.h>`，报 `error C2061: 语法错误: 标识符"VkCommandBuffer"` 加一串 C3646/C2059/C2238 —— 头文件日志里第一个错误常常不是根因，要从最靠前的那条往下读。

**不引入的。** 不做实例池压缩（`instances` 只增不减，编号不复用）、不做 GPU 侧分配、不做跨帧持久化到磁盘。这些都要等 P2 的实例级剔除落地、确认"编号稳定"真的能换成剔除率之后再说。
## 决策 53：世界 AABB 用中心/半长算，子网格包围盒在加载期预热（P2 第一步）

### 问题

`flatten breakdown` 量出两笔钱：`--cube-field 30000` 的 `external` 段 69.2 ms（其中 8 角点变换 30.5 ms），默认场景 `entities` 段 17.9 ms（其中 `subMeshBoundsFor` 首次索引扫描 17.3 ms）。

### 做法

1. 世界 AABB 改成中心/半长：`worldCenter = M * vec4(center,1)`、`worldHalf = |M3x3 列| · half`。对仿射变换精确，9 次乘法对比 8 次 `mat4 * vec4`。
2. `subMeshBoundsFor()` 的首次扫描挪到加载期：新增 `SceneGpuBatch::prewarmModelBounds(const Model&)`，`Application` 在场景就绪后按 `view<MeshComponent>` 去重预热，打 `[SceneGpu] prewarm | models=N`。

### 结果

| 场景 | 改前 flatten | 改后 | 首帧 `max` |
|---|---|---|---|
| default | 33.55 ms | 9.75~10.94 ms | 76.5 ms |
| cube-field 10000 | 56.52 ms | 34.31 ms | — |
| cube-field 30000 | 116.99 ms | 72.05 ms | — |

帧率不变（cube-field 10000，120 帧 × 3：88.75 → 88.20 FPS，噪声内）。

### 为什么帧率可以不变

摊平只在**结构重建帧**跑（`structureSignature` 变了才重建），稳态帧完全不走这段。所以这一步买到的是首帧与结构变化帧的 hitch，而不是每帧成本。要动稳态帧率，得打剩下的 `external`（逐实例 1.3 µs）与 `mirror`（T-08 骨架），那属于 P2 的实例级剔除本体。

### 代价与边界

- `abs(basis[i])` 的写法要求 `instanceMatrix` 是仿射（无投影行）。本引擎所有实例矩阵都来自 TRS 或 `entityMatrix * nodeMatrix`，成立。
- `prewarmModelBounds` 对每个模型扫一遍索引，启动时间增加（默认场景 2 个模型，几十毫秒量级）；换来首帧不再有 16 ms 尖峰。
- 归并（`merged`）与镜像（`mirror`）没动，仍是 9~13 ms / 5.5~16 ms。

### 不引入的

- 没在这步做实例级剔除：剔除的输入（每实例世界 AABB）现在仍在 CPU 算，只是便宜了。上 GPU 是下一步的事。
## 决策 54：持久镜像默认关，摊平的真实成本是「看不见的那 80%」（P2 第二步）

### 问题

决策 53 之后 `extAabb` 只剩 0.2 ms（3 万实例），逐实例 AABB 已不是成本。但那 40~44 ms 的 `external` 段里，细分计时只认领了约 6 ms（`extPush` 4.9、`extRecord` 0.6、`extAabb` 0.2、`extNormal` 0.3），剩下约 38 ms 落在「循环体其余部分」——按 3 万实例算每实例 1.27 µs。

同时 T-08 的持久镜像（决策 52）没有任何消费者，却每次结构重建都按实例数线性收费：10k 1.6 ms、30k 4.7 ms（且这还只是 `unordered_map` 查表，真正上传另算）。

### 做法

- `GpuScene` 改成 opt-in：`--gpu-scene-persist` 打开（`diag::Options::gpuScenePersist` 默认 `false`），`--no-gpu-scene-persist` 保留为显式回退；`Application` 里合成 `gpuScenePersist && !noGpuScenePersist`。
- 镜像整段包在 `if (gpuScene.isEnabled())` 里，关闭时连 `unordered_map` 查表都不做（`mirror` 段实测归零）。
- 外部循环再加两个细分计时：`extRecord`（`InstanceRecord` 构造）、`extPush`（`buckets[group].push_back`），与 `extNormal`/`extAabb` 一起进 `[SceneGpu] flatten breakdown`。

### 结果

| 场景 | total（ms/rebuild） | mirror | external | extPush | 未计入 |
| --- | --- | --- | --- | --- | --- |
| default | 11.56 | 0.00 | 0.0 | 0.0 | 0.0 |
| cube-field 10k | 26.46 | 0.00 | 13.7 | 1.1 | ≈12.6 |
| cube-field 30k | 57.60 | 0.00 | 43.9 | 4.9 | ≈38.2 |

`mirror` 三档全部归零；`total` 的下降幅度被 `external` 段自身的运行间波动（±3 ms）盖住 —— 这说明镜像本来就只占 8~10%，不是摊平的大头。

### 为什么不再往「AABB 上 GPU」走

AABB 已经只占 0.2 ms/3 万实例（决策 53 的中心/半长算法把 8 次 `mat4*vec4` 换成 3 次 `abs` 缩放）。剩下的 1.27 µs/实例是循环脚手架、`std::map` 组查表、`std::vector<std::vector>` 的 `push_back` 扩容拷贝、以及 Debug 构建下 4 次 `steady_clock` 采样的开销总和。把 AABB 搬上 GPU 一点也不会动这些。

P2 的 −70% 只能靠改结构拿到：持久缓冲变成权威数据、逐实例直写映射内存（不再经 `std::vector` 中转）、组查表换成开放寻址或直接下标。

### 代价与边界

- `mirror` 关掉后 `[GpuScene]` 一行会打 `primitives=0 instances=0`，这是预期（骨架未启用），不是回归。
- `structureSamples` 的累加被挪到镜像判断**之外**，否则关掉镜像后 `[SceneGpu] flatten breakdown` 一行都不打（本次踩到，日志里只剩 `cull summary`）。
- 本决策的读数全部来自 Debug 构建；Release 下每实例成本会显著低于 1.27 µs，但相对结构不变。

### 不引入的

- 没做实例级剔除：`shaders/scene_cull.comp` 现在是组级（`GroupRecord` 5 word/组 + `stats[3]`），实例级要改成 remap + 逐实例两段式，属于下一步。
- 没给镜像加消费者：消费它的应该是实例级剔除与视锥/遮挡查询，等那条链路通了再打开。

## 决策 55：P2 的尺子要先换成 Release，「直写映射内存」这一刀实测无效（P2 第三步）

### 问题

决策 54 把 P2 的方向定成「持久缓冲变权威、逐实例直写映射内存」。动手前先做了一次对照：同一份代码在 Release 下重跑三档摊平。

| 场景 | Debug total | Release total | Release external | Release merged | Release extPush |
| --- | --- | --- | --- | --- | --- |
| default | 9.5~11.6 ms | 4.55 ms | 0.0 | 4.5 | 0.0 |
| cube-field 10k | 26.5~32.5 | 7.83 | 2.9 | 4.5 | 0.9 |
| cube-field 30k | 57.6~60.2 | 14.67 | 9.0 | 4.5 | 2.9 |

两个结论直接改写了计划：

1. **Debug 把逐实例成本放大 4 倍**（30k：`external` 43.9 → 9.0 ms，每实例 1.27 µs → 0.30 µs）。按 Debug 数字做优化决策会系统性地高估收益。
2. **default 场景 98.7% 的摊平花在 `merged`（4.5 ms）**，不是逐实例：`ensureMergedMesh` 在结构重建时把 103 个组的几何合并一遍。逐实例那条线在 Release 下只剩 0.3 µs/实例，已经不是瓶颈。

### 做法与结果（试过，回退）

按原计划实现「预分配 `instanceScratch` + 只记已写条数 + 组装时逐条拷贝」：

- 把 `instanceScratch.clear()/insert()` 换成 `instanceScratchCount = 0` + `instanceScratch[count++] = record`，`ensureCapacity` 里 `resize(instanceCapacity)`。
- 结果（Debug 30k）：`assemble 1.2 → 3.2 ms`、`total 57.60 → 58.78 ms`，`extPush` 仍 4.5（`buckets` 的扩容没动）。

**这是负收益**：`std::vector::insert` 对 POD 会走 `memmove`，比逐条赋值的循环快一倍多；而真正想省的「扩容拷贝」在稳态下本来就不发生（`buckets.clear()` 保留容量，稳态重建不再扩容）。已 `git checkout --` 回退，工作树与 `3dd2865` 一致。

### 为什么

- 收益前提错了：以为每实例 1.27 µs 是「构造 + 拷贝」的锅，实际是 Debug 构建 + GLM 不内联的锅。
- 结构前提也错了：`buckets` 的容量跨帧保留，第一次重建之后就没有扩容成本；`instanceScratch` 的 `insert` 已经是 memmove。

### 代价与边界

- 本轮**没有代码改动**，只加了两处文档与一个判断：P2 的验收口径必须写明「Release 构建 + 稳态重建帧」。
- `merged` 之前被当成常数（9~15 ms 的固定开销），现在看它才是 default 场景的大头；要动它得改合并策略（按材质合并、持久化合并网格），是独立一单。

### 不引入的

- 不做「逐实例直写映射内存」：实测更慢。
- 不按 Debug 数字继续调优：后续 P2 的数字一律同时给 Debug 与 Release，判据看 Release。

## 决策 56：稳态摊平几乎为零，「合并网格是大头」是首帧假象（P2 第四步）

### 问题

决策 55 据 Release 首重建样本得到两个结论：default 场景 98.7% 的摊平花在 `merged`、`merged` 恒为 4.5 ms。这两个结论都不成立 —— 那 4.5 ms 是**合并网格唯一一次构建**（192496 顶点 / 786801 索引，两次设备本地上传），不是每次重建都付。

同时前几轮的 `flatten breakdown` 只有一个样本（`samples=1`）且必然是首重建，把一切一次性成本都算进了「每帧摊平」。

### 做法

- 新增 `--rebuild-structure <n>`（`diag::Options::rebuildStructure`，默认 0 关）：每 n 帧把 `structureBuilt` 置假，强制走一次 `buildInstances`，用来取**稳态**样本。
- `SceneGpuBatch` 新增 `mergedRebuiltThisCall` 与 `CullTotals::mergedBuildSamples`；`ensureMergedMesh` 在真正重建成功时置标记，汇总行多打 `mergedBuild=<次数>`。这样「建了一次」和「每帧都建」在日志里能区分。

### 结果（Release，40 帧，`--rebuild-structure 10`）

| 场景 | samples | total（稳态） | merged | external | extPush | mergedBuild |
| --- | --- | --- | --- | --- | --- | --- |
| default | 4 | **0.11 ms** | 0.0 | 0.0 | 0.0 | 0 |
| cube-field 10k | 4 | 3.80 | 0.0 | 3.5 | 1.3 | 0 |
| cube-field 30k | 4 | 10.08 | 0.0 | 9.4 | 3.2 | 0 |

`merged` 稳态为 0：签名（FNV-1a 覆盖实体 id、`WorldTransformComponent::sourceRevision`、材质槽与指针、`geometryId`、包围盒）命中即返回，4 个样本里 `mergedBuild=0`（那次构建发生在预热阶段，不计入稳态）。

### 为什么

- 「每帧摊平」这个指标本身要带重建频率才有意义。静态场景（cube-field）结构签名不变，摊平**根本不发生**；只有实体增删、变换版本变化、材质换绑时才会重建。
- 所以 P2 的 −70% 不能按「帧数」摊，只能按「每次重建」算：现在 30k 实例一次重建 10.08 ms（Release），default 一次 0.11 ms。
- 首帧那 4.5~9.6 ms 的合并网格构建属于加载期成本，压低它的办法是预热（和 `prewarmModelBounds` 同型），不是每帧优化。

### 代价与边界

- `--rebuild-structure` 是度量开关，会真实地每 n 帧重建一次，帧时间与 `structureRebuilds` 都会涨，不要和基线数字混用。
- `mergedBuildSamples` 只证明「这一批样本里没有重建」，不证明「永远不重建」；材质或模型集合变化时仍会重建。

### 不引入的

- 不做「合并网格持久化」：它已经按签名复用，跨帧不会重建。真正要做的是**按需增量重建**（只重建变化的组），那是 P2 之后的独立一单。

## 决策 57：外部实例按「绑定 + 变换」缓存记录，重建只重算变化的那部分（P2 第五步）

### 问题

`--rebuild-structure` 量出来的稳态里，30k 立方体一次重建 10.08 ms（Release），其中 `external` 9.4 ms。可这批实例是静态的：变换、模型、材质都没变，每次重建却重做一次 `transpose(inverse(mat3))`、一次中心/半长 AABB。

### 做法

`SceneGpuBatch::ExternalRecordCache`：每个外部实例留一份 `{model, subMeshIndex, materialSlot, transform, record, worldCenter, worldHalf, valid}`。重建时先比三个整型绑定，再 `std::memcmp(&cache.transform, &external.transform, sizeof(glm::mat4))`；全同就复用 `record` 与中心/半长，只做一次 `buckets` push 与 AABB 合并。汇总行加 `extReused=` 计数。

**为什么不用逐浮点哈希**：第一版用 FNV 把 16 个 float 混成一个键，30k 实例下光哈希就吃掉 0.4 µs/实例（Debug 实测 `external` 只从 40.9 降到 30.8 ms）。换成 64 字节 `memcmp` 后同一场景降到 27.6 ms。

### 结果（`--rebuild-structure 10`，相机固定）

| 场景 | 改前 total | 改后 total | external |
| --- | --- | --- | --- |
| Release 30k | 10.08 ms | **8.91 ms** | 9.4 → 7.0 |
| Release 10k | 3.80 | 3.83（合并网格 1.2 ms 计入本批样本） | 3.5 → 2.3 |
| Debug 30k | 57.60 | **35.32** | 40.9 → 27.6 |
| Debug 10k | 26.46 | **14.11** | 13.7 → 8.6 |

`extReused=90000`（30k 实例 × 3 个样本）说明缓存确实命中；`extPush` 仍是 3.2 ms（Release 30k），是对外可见的下限 —— 要把这段也压掉，必须让持久缓冲直接成为剔除输入，而不是每帧重建 `buckets`。

### 代价与边界

- 缓存按 `externalInstances` 下标对齐；数量变化时整表重建（`assign`）。
- 变换比较是逐位相等：同一实例被写入一个数值相同但符号位不同的矩阵（`-0.0f` vs `0.0f`）会被判为不同，重算一次，不影响正确性。
- 只覆盖外部实例。ECS 实体的逐实例成本在 30k 场景里不可见（`entities=0.0 ms`），暂不做。

### 不引入的

- 不做「组级签名 + 局部重摊」：cube-field 只有一个组，组级签名命中就直接是 0.11 ms 的既有快速路径；组内有实例变化时仍要全组重摊，本轮收益在"单组内实例未变"这一档，已经吃到。

## 决策 58：剔除统计缓冲先扩到 4 个 uvec4，只验布局不动读数（P2 第六步）

### 问题

第二步要在 `scene_cull.comp` 里按实例做视锥判定、并 `atomicAdd` 新计数器，但统计缓冲只有 `uvec4 stats[3]`（48 字节），C++ 侧 `SceneGpuBatch::Stats` 是 13 个 `uint32_t`（52 字节）。两边既不相等、也都放不下第 4 组：C++ 多出来的那一个是纯 CPU 写的 `externalInstances`。反过来先加计数再发现缓冲不够，越界踩的是同一块映射内存，验证层抓不到，读出来的会是一堆看着合理的数。

### 做法

- `shaders/scene_cull.comp`：`uvec4 stats[3]` → `stats[4]`，第 4 组（`stats[3]`）标注为实例级剔除的保留槽，本步不写。
- `src/Render/SceneGpuBatch.h`：`Stats` 末尾补 `uint32_t reserved[3] = {}`（13 → 16 个 uint），并加 `static_assert(sizeof(Stats) == 16u * sizeof(uint32_t))`。
- `src/Render/SceneGpuBatch.cpp`：stats 缓冲分配从 `12u * sizeof(uint32_t)` 改为 `sizeof(Stats)`，尺寸只留一个来源；`readStats` 注释写明只读前 10 个 GPU 字，`words[10..15]` 本步没有写入者。
- 把跨语言布局锁进编译期：`SceneGpuBatch.cpp` 直接 include 生成的 `scene_cull.comp.reflect.h`，断言 `kStatsBufferBlockSize == sizeof(SceneGpuBatch::Stats)` 与 `kStatsBufferStatsCount == 4`。数字来自 SPIR-V 反射而不是手抄常量 —— 以后谁只改 shader 的槽位，编译就失败。

### 为什么不是整块 `memcpy`

`Stats` 的字段顺序和 GPU 字序**并不一致**：`groups` / `drawCalls` 是纯 CPU 字段，位置正好落在 words[6..7]，而 GPU 在同一位置写的是 `occlusionTests` / `occlusionCulled`。所以这块映射内存只能按名字逐字读（`readStats` 一直是这么做的），`reserved[3]` 不改变这个性质。本轮把这条边界写进头文件注释，并用上面的反射断言把「想 memcpy 的人」挡在编译期。

### 验收

同一机位、120 帧、三档场景（default / cube-field 10k / 30k），改前改后逐行比对 `[SceneGpu] cull stats` 与 `cull summary`：

| 读数 | default | cube-field 10k | cube-field 30k |
| --- | --- | --- | --- |
| groups / visible / culled | 103 / 63 / 40 | 104 / 64 / 40 | 104 / 64 / 40 |
| instances | 63 | 10063 | 30063 |
| hizTests / hizCulled | 63 / 0 | 64 / 0 | 64 / 0 |
| external | 0 | 10000 | 30000 |
| visibleRatio | 0.61 | 0.62 | 0.62 |
| instFrustum（CPU 侧） | 0/0 | 6756/10000 | 11157/30000 |

所有整数计数器逐位相同，`[Validation] errors=0 warnings=0`；只有 `flattenUs` 与 `ms/rebuild` 这类计时列不同（单样本噪声）。`--validation-features sync`（cube-field 10k）同样 `errors=0 warnings=0`、stderr 0 字节。

全量回归 `tools/Validate.ps1 -Incremental -SkipSetup` **96 项** ALL PASS（stderr 全 0），日志 `build/validation/20261125-103927`；DocCheck PASS（documents=81 lines=18000）。

### 代价与边界

- 每帧槽位 48 → 64 字节，四帧在飞共多 64 字节，可以忽略。
- `reserved[3]` 只是占位：真正消费 `stats[3]` 的第二步要在 `readStats` 里显式加 words[12..15]，不能指望 memcpy。
- 本步没有新计数、没有新 binding、没有动 `computeBindings`。它的价值就是证明「多出来的 16 字节不会被别人踩」，以及下一次扩槽时编译期会先报错。

### 不引入的

- 不顺手把 `Stats` 重排成与 GPU 逐字对应：`groups`/`drawCalls` 是 CPU 字段，重排会让「这块内存可以直接 memcpy」变得更像真的，反而增加误用风险。
- 不在本步就加实例计数：那样一次要动 shader 槽位、描述符、计数三处，出问题无法定位。

## 决策 59：实例级视锥统计放进第二遍 dispatch，这一步只数不裁（P2 第七步）

### 问题

P2 的目标是把 `visible/submitted` 从组级的 0.62 压到 0.6 以下，靠的是逐实例剔除。但组级 `main()` 是「一个 invocation 一个组」：cube-field 把 30000 个实例压在**同一个组**里，在组内串行遍历实例等于一个线程跑完 30000 次视锥测试，比不做还慢。而且这一步不改绘制，先要证明的是「GPU 算出的实例可见比例与 CPU 只读度量一致」（`instFrustum`：10k 0.6756、30k 0.3719），否则后面把绘制接到这套数据上就是盲接。

### 做法

- `shaders/scene_cull.comp` 加 binding 5：`readonly buffer InstanceBuffer{InstanceRecord instances[]}`，与 `src/Render/SceneGpuBatch.h` 的 `InstanceRecord` 逐字节同构（224 字节）。`instanceInFrustum()` 用记录里的**局部** AABB（`boundsMin/boundsMax`，`w <= 0.5` 视为无界）与世界矩阵复现 CPU 的算式：`center = model * vec4(localCenter, 1)`、`worldHalf = abs(basis[0])*localHalf.x + abs(basis[1])*localHalf.y + abs(basis[2])*localHalf.z`，逐平面 `distance + radius < 0` 判剔除。
- push constant 16 → 32 字节（追加 `uvec4 more`：`x = count`、`y = mode`）；同一 command buffer 里两次 `vkCmdPushConstants` + 两次 `vkCmdDispatch`：第一遍 `mode = 0` 跑原来的组剔除，第二遍 `mode = 1` 跑 `countInstanceVisibility()` —— `gl_GlobalInvocationID.x` 直接当实例下标，`(count + 63) / 64` 个工作组。
- 计数落在 `stats[3]`：`x` = 测过的实例数、`y` = 其中在视锥内的；`Stats` 尾部把 `reserved[3]` 换成 `instanceTests` / `instanceVisible` / `reserved`（仍是 16 个 uint），`readStats` 读 `words[12]` / `words[13]`（结构体位置 13/14），`CullTotals` 加 `gpuInstanceTested` / `gpuInstanceVisible`，`logCullSummary` 多打一行 `[SceneGpu] instance summary | frames= gpuInst=… (visible/tested) | tested/frame=`。
- 描述符同步：`computeBindings[6]`、`bindingCount = 6`、storage 池 `5u * kFramesInFlight`、`writeDescriptors` 的 `infos[5]` / `writes[5]`（第 5 项 `dstBinding = 5`）。三条新的编译期锁：`kPushBlockSize == sizeof(CullPushConstants)`（32）、`kPushMoreOffset == 16`、`kInstanceBufferBinding == 5`。

### 为什么测整条实例缓冲，而不是只测可见组里的实例

backlog 原文写的是「对可见组的实例做视锥测试」，实现改成**全量**，两个理由：

1. CPU 侧的度量就是全量：`buildInstances` 末尾遍历 `externalRecordCache` 时完全不看组可见性。只有同口径，「0.676 / 0.372 对得上（±2%）」这条验收才有意义；GPU 若只测可见组，分母变小、比例必然偏高，等于没有对照物。
2. 组门控需要「实例 → 所属组」的归属表，而第一步的布局里没有这张表。这张表在第三步做紧凑索引时本来就要建（要按组写连续区间），到时门控是顺带的，现在单开一份数据是重复投资。

代价是 GPU 多测了非外部的实体实例：10k 场景 `tested/frame = 10103` 对 CPU 的 10000，比例差 0.2%（0.6747 对 0.6756），正好落在验收容差内，方向可解释。

### 为什么两遍之间不加屏障

第二遍只读实例缓冲与帧数据（dispatch 前已写好）、只写 `stats[3]` 的两个 word；第一遍写的是 5 word 的 indirect 命令与 `stats[0..2]`。两组写入不重叠，读的也不是第一遍写的内容，所以不需要 `vkCmdPipelineBarrier`。真正需要屏障的位置在第二遍之后 —— 那里原本就有一条 `SHADER_WRITE → INDIRECT_COMMAND_READ | SHADER_READ` 的全屏障，覆盖两遍的全部输出。

### 验收

同一机位、120 帧、三档场景，`[SceneGpu] instance summary` 与 CPU 侧 `flatten breakdown … instFrustum=` 对照：

| 读数 | default | cube-field 10k | cube-field 30k |
| --- | --- | --- | --- |
| GPU `tested/frame` | 103 | 10103 | 30103 |
| GPU `gpuInst` | 0.58 (7080/12154) | **0.6747** (804288/1192154) | **0.3726** (1323606/3552154) |
| CPU `instFrustum` | 0/0（无外部实例） | 0.6756 (6756/10000) | 0.3719 (11157/30000) |
| 差值 | — | 0.09 个百分点 | 0.07 个百分点 |

组级读数与第一步逐位相同（default `103/63/40`、10k `instances=10063 / total 9.06 ms`、30k `30063 / 17.33 ms`），三档 `[Validation] errors=0 warnings=0`、stderr 0 字节；`--validation-features sync`（10k）同样干净。

全量回归 `tools/Validate.ps1 -Incremental -SkipSetup` **96 项** ALL PASS（stderr 全 0），日志 `build/validation/20261125-111243`；DocCheck PASS（documents=81 lines=18077）。

### 代价与边界

- 第二遍在 cube-field 30k 上的开销 ≤0.5%，落在噪声内：600 帧 × 3 轮交替 A/B，开启时 `4.69599 / 4.71483 / 4.72523 s`（127.0~127.8 FPS），把这条 dispatch 的组数临时改成 0 后 `4.68706 / 4.69427 / 4.70678 s`（127.5~128.0 FPS）；两种配置的 FrameTime median 都是 7.62 ms。
- 两侧共用同一套近似（未归一化平面 + 沿法线投影的世界半长 = 包围盒判定），所以差值只剩「非外部实体实例」，不是算法差异。
- 无界实例（`boundsMin.w <= 0.5`）直接按可见算，并且**也计入 tested** —— 保守口径与组级剔除一致；本轮三档场景里没有这种实例，所以不影响上表。
- 本步没有任何裁剪：indirect 的 `instanceCount` 仍是整组实例数，`cull summary` 的 `visibleRatio` 仍是组级。新读数只是把第三步要用的数字先量出来。

### 不引入的

- 不在组内循环里跑实例测试（cube-field 是单组 30000 实例，会退化成单线程）。
- 不在本步改绘制路径：先把统计与 CPU 对上，第三步再把 `instanceCount` 换成 remap 后的数量，出问题时能分清是「测错了」还是「裁错了」。
- 不把实例计数塞进 `stats[0..2]`：那三组已被组级计数占满，且 `Stats` 的 words[6..7] 是 CPU 字段，塞进去只会诱发「整块 memcpy」。

### 踩到的两个坑

- GLSL 里 `half` 是保留字（`error : 'half' : Reserved word.`），局部变量得叫 `worldHalf`。
- 用 `Copy-Item` 覆盖源码会带回旧 mtime，MSBuild 认为 `.obj` 已是最新而跳过重编，跑出来读数是 0（实际在跑上一次 A/B 的旧二进制）。改完源码要 `(Get-Item <file>).LastWriteTime = Get-Date`，并确认构建日志里出现该 `.cpp` 的名字。

## 决策 60：实例级剔除用「每组一段紧凑区间 + remap 表」，四段 dispatch 接力（P2 第八步）

### 问题

组级剔除只能整组保留或整组剔除：cube-field 一万个立方体合成 1 个组，组一旦可见就画满整组。`visibleRatio 0.62` 说的是「104 个组里 64 个可见」，与实例数无关；而实测实例级视锥可见比例是 10k `0.6756`、30k `0.3719`（决策 59 的度量）。要把这个比例变成真正的提交比例，得同时解决三件事：决定哪些实例进绘制、让顶点着色器按紧凑后的序号取记录、让 indirect 命令的 `instanceCount` / `firstInstance` 指向紧凑区间 —— 同时**保持「所有组共用一条 multidraw」**，否则组级剔除省下来的提交次数就白省了。

### 做法

- **命令写两处**：组剔除阶段在实例级剔除开着时（`push.more.z != 0`）不再累加实例级统计；紧凑化阶段按每组可见实例数重写 `commands[base + 1] = count`、`commands[base + 4] = running`（`running` 是前缀和得到的紧凑起点），并顺手累加 `stats[0].w` / `stats[1].x` / `stats[1].y`。
- **remap**：`instanceRemap[i]` = 紧凑序号 `i` 对应的真实实例下标。名次由每个实例一次 `atomicAdd(cullGroupBuffer.cullGroups[g].x, 1)` 领号，先暂存在 scratch 后半段（`push.more.z` 是分界，前一半是 remap、后一半是名次），最后一段把它搬成 `remap[base + rank] = instanceIndex`。
- **`mrt.vert` 只在 `push.gpuScene != 0` 时过 remap**：`const uint instanceIndex = push.gpuScene != 0u ? instanceRemap[gl_InstanceIndex] : uint(gl_InstanceIndex);`，取矩阵与 `fragInstanceIndex` 都用这个真实下标，所以顶点取的矩阵与片元取的材质参数是同一份记录。地形批处理（`terrainBatched`）不过 remap。
- **实例 → 组用 shader 内二分**：CPU 端按 `stable_sort(order)` 的顺序把实例拼进缓冲（`group.firstInstance = instanceScratch.size()`），所以组区间按 `firstInstance` 升序、连续无空隙；二分找「最后一个 `draw.w <= instanceIndex` 的组」即可。
- **回退写恒等表**：`--no-instance-cull` 时跑第 5 个模式（一个 invocation 一个实例写 `remap[i] = i`），命令保持组剔除写下的原值。

四段 dispatch 全部挂在同一个 compute pipeline 上，用 `push.more.y` 选阶段：`0` = 组剔除、`1` = 测视锥 + 领名次、`2` = 组前缀和 + 改命令、`3` = 搬 remap、`4` = 恒等 remap。

### 为什么是四段，而不是两段或塞进组内循环

- **塞进组内循环不可行**：`layout(local_size_x = 64)` 一个 invocation 一个组，cube-field 就是一个组压着 30000 个实例，等于用一个线程跑完所有测试（决策 59 已经踩过这个结论）。
- **名次必须在起点之前领，起点必须在可见数之后算**：要避免重复测视锥，就得把「名次」暂存下来，于是拆成「测 + 领号」「前缀和」「搬运」三段。少一段就要多测一次视锥，多一段就要多一块暂存。
- **前缀和单 invocation 串行**：组数只有几十到几百（实测 103 / 104），而这一步本来就要「每组只写一次 5 word 命令」—— 天然是单写者，串行比并行扫描省事得多。
- **段间用 compute → compute 屏障**（`SHADER_WRITE` → `SHADER_READ | SHADER_WRITE`）：第 1 段写的 `cullGroups` / scratch 后段被第 2、3 段读，第 2 段写的命令被第 3 段读。第 3 段之后仍然走原来那条 `SHADER_WRITE` → `INDIRECT_COMMAND_READ | VERTEX | FRAGMENT` 的全屏障。

### 为什么回退路径也要写 remap

两条路径共用同一段顶点取下标代码，才能让「关掉开关画面逐像素一致」成为可验证的结论 —— 否则关掉开关跑的是另一条着色器路径，不一致时分不清是裁剪错了还是路径不同。顺带避免了 host 侧往在飞帧里写 remap（那需要额外的同步）。

### 验收

| 场景 | 组级 `groups/visible/culled` | 提交实例数 `instances/frame` | GPU `gpuInst` | 第二步的 `instances/frame` |
| --- | --- | --- | --- | --- |
| default | 103 / 63 / 40 | 59.0 | 0.58 (7080/12154) | 63.0 |
| cube-field 10k | 104 / 64 / 40 | **6815.0** | 0.6747 (804288/1192154) | 10063.0 |
| cube-field 30k | 104 / 64 / 40 | **11216.0** | 0.3726 (1323606/3552154) | 30063.0 |

- `cull summary` 的 `instances/frame` 现在就是真正提交的实例数（6815 / 11216），与 `instance summary` 的 `submit/frame` 逐位一致；`gpuInst` 仍是「测过里可见的」比例，比提交数每帧多 1 个实例 —— 那个实例所在的组被组级剔除掉了，紧凑化阶段按 `commands[base+1] == 0` 跳过，不会复活。
- **逐像素一致**：cf10k 第 30 帧 1920×1080 截图，开 / 关两版共 832 个像素不同，全部落在左上角 HUD 的「Performance」面板内（x 60~330 / y 90~300），面板外 **0 个像素不同**；两次同样配置的运行本身也有 285 个像素不同（同一区域，FPS 数字在跳），所以几何完全一致。
- 三档 + `--no-instance-cull` + 体素（`--voxel-world 1` / `12`）+ `--validation-features sync`（10k）在 **Debug** 构建下全部 `[Validation] errors=0 warnings=0`、stderr 0 字节；`--cube-field-motion 500 --rebuild-structure 7`（200 帧、每帧强制重建）也 `errors=0`、`gpuInst=0.6705 (1341261/2000394)`。
- 注意验证层只在 Debug 里存在（`src/RHI/Device.h:145-148`，`NDEBUG` 下 `enableValidationLayers = false`），所以「Release 跑出 errors=0」不能当证据 —— 这一版第一次全量回归就是被它骗过去一次，见下方踩坑。
- 全量回归 `tools/Validate.ps1 -BuildDir build -Config Debug -Frames 120 -Incremental -SkipSetup`：**96 项 ALL PASS**（stderr 全 0，日志 `build/validation/20261125-121228`），DocCheck PASS（documents=81）。上一轮 96 项里有 9 项体素/水面/TAA/SSR 用例变红，就是下面第一条坑。

### 代价与收益

- cube-field 30k（600 帧 × 3 轮，带 `--stats-frame`）：开 `142.19 / 142.86 / 142.08 FPS`、median 6.88 ms、p95 7.62 ms；关（`--no-instance-cull`）`125.86 / 126.09 / 125.66 FPS`、median 7.88 ms、p95 8.12~8.38 ms —— **+13.1% FPS、median −1.00 ms（−12.7%）**。这就是本步的目的：少提交约 63% 的实例，顶点与片元两边都少跑。
- default 场景（103 组 / 63 实例）：开 `202.55 / 203.82 FPS`，关 `204.81 / 204.65 FPS` —— **−0.6~1.1%**，median 都是 4.88 ms。实例数很少时这四段 dispatch 是纯开销，收益要千级实例往上才盖得住。
- 组级读数与第二步逐位相同（`104/64/40`、`external=10000/30000`、`instFrustum=6756/10000`、`11157/30000`）。`flatten breakdown` 的 `total` 单样本 9.06 → 8.11、17.33 → 16.19 ms 波动全在 `merged` 列（4.9 → 3.8 ms），不当收益计入。

### 不引入的

- 不做并行前缀和 / GPU 排序：组数只有百余个，单 invocation 串行足够，代码少一半。
- 不动 `shadow.vert` 等其它顶点着色器：它们不读实例记录，这一步只改 `mrt.vert`。
- 不改 `Stats` 的 16 个 word 与 `readStats`：提交量复用 `stats[0].w`（`CullTotals::visibleInstances`），语义从「可见组的实例数之和」变成「紧凑后真正提交的实例数」，`gpuInst` 仍由 `stats[3]` 提供。
- 不给地形批处理做 remap：它有独立的实例表与 indirect 路径，set 3 的 binding 1 只是为了让同一套布局能绑上（指向同一块实例缓冲，内容不被读）。

### 踩到的坑与边界

- `scene_cull.comp` 的 binding 1（命令缓冲）必须去掉 `writeonly`：前缀和阶段要读 `commands[base + 1]` 来判断这一组是否已被组级剔除。
- 地形 chunk 实例表的描述符池 `descriptorCount` 必须同步 ×2（`src/Render/GeometrySystem.cpp`）：池容量是按「set 数 × 每 set 描述符数」配的，binding 变多后不加会 `vkAllocateDescriptorSets` 失败。
- remap 与名次共用一块 `2 × capacity` 的缓冲，`push.more.z` 同时兼任「实例级剔除是否开着」的开关：0 = 关（此时走恒等模式），非 0 = 开。
- 组剔除把整条 5 word 命令清 0，所以紧凑化阶段用 `commands[base + 1] == 0` 判「这组没实例要画」；可见组里一个实例都没通过视锥时也会把命令清 0，两条路径同一个出口。
- **用同一套实例布局的不止一处**：除了地形 chunk 实例表（`GeometrySystem::ensureTerrainChunkInstances`），体素 chunk 实例表（`GeometrySystem::ensureVoxelBatchResources`）也从 `sceneInstanceSetLayout` 分配 set。加 binding 1 时只改了地形那边，体素场景每帧报一条 `VUID-vkCmdDrawIndexedIndirect-None-08114`（`Descriptor in binding #1 index 0 is being used in draw but has never been updated via vkUpdateDescriptorSets()`），`f3-voxel*`、`f3v-taa/water/ssr` 共 9 项回归全红。两处都要「池 `descriptorCount` ×2 + 写满两个 binding」，binding 1 都指向同一块实例缓冲（这两条路径不过 remap，内容不会被读）。
- **Release 里没有验证层**（`src/RHI/Device.h:145-148`）：`NDEBUG` 下 `enableValidationLayers` 直接是 `false`，验证回调不注册，`[Validation] errors=0` 恒成立。所以本步的「验证干净」必须用 `-Config Debug` 的 `TinyEngine.exe` 或 `tools/Validate.ps1` 证明；Release 版的 `--validation-features sync` 只验证了参数解析。

## 决策 61：逐实例 Hi-Z 遮挡收益为 0，不落地（P2 第九步，测量后否决）

### 问题

P2 的验收还差 `visible/submitted ≤ 0.6` 这一条：第三步落地后 10k 实例是 `6815/10000 = 0.68`、30k 是 `0.37`。组级 Hi-Z 遮挡剔除一直报 `hizCulled/frame=0.0`，怀疑是「一个组太大、整片 cube-field 是一个组」把遮挡判定稀释掉了 —— 于是先按第二步的路子「只数不裁」，量一下逐实例 Hi-Z 的潜在收益。

### 怎么量的

- `shaders/scene_cull.comp` 的模式 1（`rankInstanceVisibility`）里，对每条通过视锥的实例算世界 AABB（与 CPU `worldCenter`/`worldHalf` 同一算式），再调组级那支 `occludedByHiZ(min, max)`；命中就 `atomicAdd(statsBuffer.stats[3].z, 1)`。**只统计，不让它影响名次**，所以画面与提交量完全不变。
- `SceneGpuBatch::readStats` 读 `words[14]`（`stats[3].z`）累加进 `CullTotals::gpuInstanceOccluded`，`instance summary` 行打印 `occInst=<比例> (<次数>)`。
- 探针验证分支可达：把 `occludedByHiZ` 临时改成 `return true`，`occInst` 立刻跳到 `0.66 (787640)`、组级 `hizCulled/frame` 从 0 涨到 34.4 —— 说明统计链路本身没断。

### 读数（Release，`--cube-field 10000`）

| 机位 / 场景 | 执行次数 | 判定被遮挡 |
|---|---|---|
| 参考机位（`-4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`），120 帧 | 790656 | **0** |
| 同一机位 60 帧（对照） | 381696 | 0 |
| 平视（eye y=1.2、pitch 0） | 376264 | 0 |
| 平视 pitch −0.05 | 391384 | 0 |
| 平视 pitch −0.15 | 409416 | 0 |
| `--voxel-world 1`，60 帧 | 5768 | 0 |

另测：level 0 采样值 `>= 1e6`（背景 sentinel）的次数为 0，所以不是「背景把金字塔填满了」这种实现 bug。

### 为什么必然是 0

1. **自遮挡**：Hi-Z 金字塔来自上一帧的 G-Buffer 位置，被测试的实例自己的像素就在金字塔里，它的最近深度约等于该处的场景深度，`nearest > farthest + 0.05` 只有「整块被别的东西完全挡住」才成立；静止场景里没有东西挡它，结论就是「可见」，这是**正确**的答案。
2. **背景 sentinel**：G-Buffer 位置 `w <= 0.5` 的像素在金字塔里写 `kFarDepth = 1e9`，而 mip 级别按投影矩形尺寸选（4 px 的方块取 level 2），矩形一旦擦到天空就是 1e9，比较必然失败。平视机位地平线以上的方块全部撞这条。
3. **场景几何没有遮挡**：参考机位是俯视平地上的 100×100 阵列，方块彼此在屏幕上不重叠，`visible/submitted=0.68` 是视锥覆盖的几何事实，不是缺了遮挡剔除。组级 `hizCulled` 恒为 0 是同一个原因。

### 代价

探针（每实例多做 8 次视投影 + 一次 `textureLod`）与回退后的 A/B：cf30k 600 帧带探针 `144.28 / 146.61 / 146.52 FPS`，回退后 `147.00 / 146.47 / 146.77 FPS` —— 差异落在噪声里（≤1.9%，且方向不一致）。所以否决的理由**不是成本**，而是收益为 0。代码已整段回退，工作树与 `f5ad4f3` 一致（`git status` 干净）。

### 不引入的

- 不做「只含静态遮挡体」的第二份金字塔（要额外一遍不含实例的深度 pass，且当前两个场景都测不出收益）。
- 不做逐实例屏幕尺寸剔除：它会真的少画东西（远处的亚像素方块消失），属于 LOD（P6）的活，需要单独的画面验收口径。
- 不改 `≤ 0.6` 的判定文字，但读数口径按实例记录：10k `0.67`（视锥）、30k `0.37`，并注明这是机位相关的几何值。

## 决策 62：灯数据搬出 UBO，放进 set 0 binding 19 的灯缓冲（P2 第十步）

### 问题

点光源数组一直躺在 `GlobalUbo.pointLights[100]`（`src/Core/FrameInfo.h`）：3.2 KiB / 帧槽，「最多 100 盏」是硬编码上限，而且每帧整块重写的流数据占着 uniform 的语义。P7-5 的灯随机采样要先有「上限不是一个数组长度」这个前提（`docs/80-roadmap/ue-borrowing-plan.md:738`）。

### 做法

- `src/Core/FrameInfo.h`：删 `struct PointLight` 与 `pointLights[MAX_POINT_LIGHTS]`，`#define MAX_POINT_LIGHTS 100` 换成 `constexpr uint32_t kMaxPointLights = 1024;`。UBO 里只剩 `int numLights`，`sizeof(GlobalUbo)` **3680 → 480 字节**（`previousProjectionView` 144、`shadowMatrices` 208、`shadowCascadeSplits` 400、`shadowParams` 416、`fogParams` 432、`shadowFilterParams` 448、`clusterParams` 464）。
- `previousProjectionView` 显式 `alignas(16)`：这个偏移以前是**搭便车**搭在 `pointLights` 数组的 `alignas(16)` 上的。数组搬走后前面的 `float exposure` 只到 136，不写 alignas 就会与 GLSL std140 的 144 错位（`static_assert` 当场报 `GlobalUbo.previousProjectionView offset mismatch`）。
- 新 SSBO 放在 **set 0 binding 19**：`layout(set = 0, binding = 19) readonly buffer LightBuffer { PointLight lights[]; } lights;`，`lighting.frag` 的逐灯循环改成 `lights[i]`，`ubo.numLights` 的语义不变。
- 缓冲由 `DescriptorManager` 持有：每帧槽一块 `kMaxPointLights × 2 × vec4` = 32 KiB（`CPU_TO_GPU` + 常驻映射），`uploadLights(frameInFlightIndex, lights, bytes)` 只做 `memcpy` + 一次整块 `vmaFlushAllocation`；描述符每帧槽写一次（缓冲身份不变）。`applyLightBinding(set)` 供 `buildGlobalSet` 与平面反射通道自建 set 0 复用。
- `src/Core/Application.cpp` 把「逐盏写 UBO」换成一行上传 + `[Lights] lightBuffer | applied=… requested=… capacity=1024`（数量变化时才打）。
- `shaders/present.frag`、`shaders/present_no_bloom.frag`、`shaders/taa.frag` 里那三份 `struct PointLight` + `pointLights[100]` 一并删掉：它们不读灯，但 std140 下这 3200 字节占位还在，留着就与新的 C++ 结构整体错开。

### 为什么是 SSBO，而不是「UBO 再大一点」

1. UBO 每条 range 的硬上限是 64 KiB，1024 盏就是 32 KiB，再加别的每帧参数会顶到天花板；SSBO 没有这个限制。
2. std140 下 `PointLight` 数组的元素 stride 要取整到 16 字节、结构体尾部还要 padding；SSBO 的 std430 里 32 字节一条就是 32 字节，C++/GLSL 只需要 `static_assert` 一个 `sizeof`。
3. 灯是每帧整体重写的流数据，不该占 uniform 的「常量」语义；换成常驻映射缓冲后，上传就是 `memcpy`。

### 为什么不新开一个 set，或者挂到 set 3

set 0 已经绑在所有需要灯的着色器上（`lighting.frag` 与三个后效 shader），加一条 binding 只是多一个 layout entry；新开 set 要同步改 pipeline layout、池容量、每个 Pass 的绑定顺序与重绑时机。灯只需要和 UBO 一起被片元着色器看到。

### 验收（2026-11-25）

- **容量解除**：`[Lights] lightBuffer` 日志 —— 默认场景 `applied=0 requested=0`、`--light-stress 100` → `100/100`、`512` → `512/512`、`1024` → `1024/1024`、`1500` → **`1024/1500`**（夹取到容量，不崩）。旧代码在 1500 时只会上传 100 盏。
- **灯真的亮起来**（截图，1920×1080、第 30 帧、固定机位 `-4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`，`--light-stress-radius 12`）：0 盏 vs 100 盏 118,653 个像素不同（其中 ≥8 的 311 个）；100 盏 vs 512 盏 **208,703 个像素不同（≥8 的 2,446 个）**，平均亮度 97.87 → 98.01 —— 超过 100 的那 412 盏确实参与着色。（默认半径 60 时同一对照是 190,951 / 404，因为灯铺得稀、每个片元命中的少。）
- **两条路径第一次对齐**：逐灯 512 盏 vs 聚簇 512 盏现在只差 9,785 个像素（0.47%，迭代顺序/浮点差异）；而「只上 100 盏」的逐灯结果对聚簇 512 盏差 182,807 个像素（9.5%）—— 差距缩小约 19 倍，说明逐灯路径真的看到了全部 512 盏。
- **验证必须用 Debug**（`src/RHI/Device.h:145-148` 在 `NDEBUG` 下把 `enableValidationLayers` 设成 `false`）：`--light-stress 512`（逐灯）、`512 --clustered-lighting`、再加 `--clustered-gpu-build`、`--light-stress 1024`、默认场景、`--voxel-world 1 --light-stress 200` 六组，全部 `errors=0 warnings=0`、stderr 0 字节、exit=0。
- 全量回归 **96 项 ALL PASS**（`build/validation/20261125-132707`，stderr 全 0）。

### 代价与边界

- 每帧多一次 32 KiB `memcpy` + 一次整块 flush（灯整体重写，没有脏区跟踪）。这一步没做 A/B：它的验收是「上限解除 + 画面真的变」，而 512 盏灯下的帧时间由聚簇/光照主导；要量的话沿用 600 帧 × 3 轮交替的同一套方法。
- 灯仍然每帧全量上传（哪怕一帧只动了一盏）；`applied` 的上限是 1024，`numLights` 仍是 `int`。
- 簇里的 `lightId` 仍是 32 位索引：roadmap 里说的 16 bit `lightId` 属于聚簇编码，与本步解耦，没有做。

### 不引入的

- 不做「只上传变化的灯」的脏区/版本号机制（当前没有能测出收益的用例）。
- 不做 16 bit `lightId`，不动 `ClusteredLighting` / `clusterBuildPass` 的簇表布局。
- 不改 `--light-stress` 的默认半径 60（它决定压力场铺多大，与本步无关）。

### 踩到的坑

1. `previousProjectionView` 的 16 字节对齐以前是搭在 `pointLights` 的 `alignas(16)` 上，删掉数组后必须显式写 —— 好在 `static_assert` 当场报，不是留到运行时画面错乱。
2. 声明 `GlobalUbo` 的 10 个 shader 里有 3 个（present / present_no_bloom / taa）也声明了 `PointLight pointLights[100]`：不读灯，但会占 std140 的 3200 字节，漏删就整体错位。
3. `DescriptorManager::buildGlobalSet` 的 writes 是「先绑占位、末尾用最近的表覆盖」，灯表必须同样在末尾 `applyLightBinding`，否则着色器会把材质 factor 缓冲当灯数组读。
4. 平面反射通道自己建 set 0（`ReflectionPass::rebuildDescriptors`），也必须显式 `applyLightBinding`，否则每帧一条 `VUID-vkCmdDrawIndexedIndirect-None-08114` —— 与 P2 第二步体素实例表那处同一类问题：**静态引用了新 binding 的 set，每一处分配者都要写满**。

## 决策 63：退役组级剔除，组只保留合并网格的索引区间（P2 第十一步）

### 问题

P2 的剔除从第一步起就是「一条组 → 一次判定」：组的 AABB 过视锥、过屏率、过 Hi-Z，不过就把整条 5 word 的间接命令清零。实例级剔除（决策 60）落地之后，这套东西立刻露出两处结构性问题：

1. **两把尺子打架**。组级判定拿组的 AABB 当球，实例级判定拿实例 AABB 逐平面投影半长。前者更粗，经常「整组通过」；后者更细。`visibleRatio` 是组口径（至少一个可见实例的组 / 全部组），`submit/frame` 是实例口径，两个数永远对不上，`visible/submitted ≤ 0.6` 这条 P2 验收也因此说不清在量哪一层。
2. **组级视锥本身低估半径**。旧 `frustumVisible(minimum, maximum)` 用 `radius = length(extent)`（欧氏长度），却去跟 `dot(plane.xyz, center) + plane.w` 比 —— 那个距离被未归一化平面的 `|plane|` 缩放过了，半径没跟着缩放。于是组级判定比逐实例判定更爱剔：实测每帧恰好漏提交 1 个逐实例判为可见的实例（default 59 / 10k 6815 / 30k 11216，而逐实例可见数是 60 / 6816 / 11217）。10k 场景漏掉的那条在画面左下角露头（包围盒 `(0,1000)-(151,1079)`，就藏在调试面板左边）。

### 做法

- `GroupRecord` 从 64 字节缩到 32 字节：只剩 `uvec4 draw`（firstIndex / indexCount / vertexOffset / firstInstance）与 `uvec4 counts`（instanceCount / materialSlot / merged / 保留）。组 AABB 只留在 CPU 侧（`gpuScene.ensurePrimitive` 还要用它），不再上传给剔除。
- 模式 0 由「组剔除」改名 `kModeCommandSetup`：只把组记录里的索引/实例区间原样写成基线命令，再加一次 `stats[0].x`。**不再测视锥、不再测屏率、不再测 Hi-Z。**
- 可见性完全由模式 1 的逐实例判定决定：`instanceWorldBounds` → `instanceInFrustum` →（`frame.screen.x > 0` 时）`tooSmallOnScreen`。
- 模式 2 变成唯一的裁决策：按「本组可见实例数」写 `instanceCount` 与 `firstInstance`（紧凑区间起点），为 0 就把 5 个命令字清零并把该组计进 `stats[0].z`；否则计 `stats[0].y` 并累加可见实例/三角形/顶点。
- 屏幕空间误差从组粒度降到**实例粒度**：`tooSmallOnScreen(center, worldHalf)` 用同一套投影公式，在模式 1 里对通过视锥的实例判定，命中计 `stats[2].x/y`。这不是删功能，是把尺子换细 —— `--gpu-scene-min-px` 与 Validate 的 `f1e-screen-space` 仍然成立，而且现在真的能剔到东西。
- 组级 Hi-Z 遮挡随组级剔除一起退役：删掉 `occludedByHiZ`、`kOcclusionBias` 与 shader 里的 `sampler2D hizPyramid`。金字塔本身仍在建（`buildHiZ`、`--no-hiz`、`hizReady` 都不动），C++ 侧计算布局的 binding 4 与描述符写入也保留 —— 拆整条 Hi-Z 链是另一件事（决策 61 已量过逐实例 Hi-Z 收益为 0）。

### 为什么组不再是剔除单位

一条组存在的理由是「合并网格的一段索引区间」：所有模型拼进同一份顶点/索引缓冲后，每个模型子网格对应一段 `firstIndex/indexCount`，那才是组的意义 —— **draw call 的收益在合并，不在剔除**。组一旦不承担剔除，`visibleRatio` 也就没有存在必要，实例口径的 `submit/frame` 成为唯一读数，`--no-instance-cull` 的语义也变得干净：以前它退回组级剔除，现在它退回「完全不剔除」。

### 为什么屏率误差要降到实例粒度，而不是跟着删

组粒度那版对一个 100×100 的立方体阵列毫无意义：整块的 AABB 投影巨大，永远「不够小」。降到实例粒度之后它才有意义，也正好是 LOD 选择的前置件 —— 等 LOD 链落地，这里会从「剔掉」改成「按误差选级」。所以这一步保留公式、只换粒度。

### 为什么回退路径仍然是「写恒等 remap」

`--no-instance-cull` 不是「跳过 remap」，而是让 GPU 写一张恒等表（模式 4）。顶点着色器里只有一段取下标代码，两条路径都走它，画面才能做到逐像素一致；如果回退路径改成「host 侧不写 remap」，就要处理和正在飞的帧的竞争，还得让着色器分叉出第二套取下标逻辑 —— 用一次 dispatch 换零分支是划算的。关掉时提交量等于实例总数（10k 场景 10103），组口径 `visibleRatio` 1.00。

### 验收（2026-11-25）

Release，120 帧，固定机位 `--camera -4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`，对照决策 60 的读数：

| 场景 | 组（可见/剔除） | instances/frame | submit/frame | visibleRatio | gpuInst |
| --- | --- | --- | --- | --- | --- |
| default | 63/40 → **60/43** | 59.0 → **60.0** | 59.0 → **60.0** | 0.61 → **0.58** | 0.58（不变） |
| cube-field 10k | 64/40 → **61/43** | 6815.0 → **6816.0** | 6815.0 → **6816.0** | 0.62 → **0.59** | 0.6747（不变） |
| cube-field 30k | 64/40 → **61/43** | 11216.0 → **11217.0** | 11216.0 → **11217.0** | 0.62 → **0.59** | 0.3726（不变） |

- **提交量第一次与逐实例可见数完全相等**：60 = 7080/118、6816 = 804288/118、11217 = 1323606/118。在此之前每帧都漏 1 个。
- 组读数变化是口径变化而不是退化：43 = 40 条没有实例的组 + 3 条「有实例但一个都不可见」的组；`visibleRatio` 现在是「至少一个可见实例的组 / 全部组」。
- `flattenUs` 落在噪声里（default 2259.3、10k 8534.3、30k 21163.7，单样本，与决策 60 的 1982.9/8388/20560 同一区间）。
- 屏率误差剔除修好之后可以分级响应（default 场景）：`--gpu-scene-min-px` 1/4/8 → 剔 0 个；60 → 剔 12 个；120 → 剔 18 个；300 → 剔 28 个。
- 画面：default 在 HUD 面板外 **0 像素差异**；10k 在面板外 9,425 个像素不同，全部落在左下角 `(0,1000)-(151,1079)` —— 就是那条此前被组级判定误杀的可见实例，现在画出来了。
- 验证必须在 **Debug** 下做（`src/RHI/Device.h:145-148` 在 `NDEBUG` 下把 `enableValidationLayers` 关掉）：三档 + `--no-instance-cull` + `--gpu-scene-min-px 8/60` + `--no-hiz` + `--voxel-world 1` + `--validation-features sync` 共 9 组，全部 `[Validation] errors=0 warnings=0`、stderr 0 字节。
- 全量回归 **96 项 ALL PASS**（stderr 全 0，`build/validation/20261125-140206`）；DocCheck PASS（documents=81 lines=18387）。

### 代价与边界

- 组级三个测试（视锥/屏率/遮挡）合起来只省一次 `atomicAdd` 和一点比较，本来就不是性能项：30k 场景 `total` 17.12 ms 与决策 60 的 16.19 ms 之间是单样本噪声（同一二进制重复跑能差 1 ms）。
- 组 AABB 还在 CPU 侧算（`gpuScene.ensurePrimitive` 用），只是不再往 GPU 传 —— 所以 `buildInstances` 里那两次 `min/max` 不能一起删。
- Hi-Z 金字塔暂时没有消费者：仍在建、仍可 `--no-hiz` 关，`stats.occlusionTests/Culled` 保留但恒 0。要么后面给逐实例遮挡复活（决策 61 说收益为 0），要么整条拆掉 —— 这是显式遗留项，不是漏做。
- `--no-instance-cull` 的语义变了（旧：退回组级；新：完全不剔除）。文档与 `CliCatalog` 的说明都已改，老脚本如果靠它「只关实例级」需要改。

### 不引入的

- 不保留「组级剔除」作为第二档回退：两把尺子并存正是这一步要消掉的东西。
- 不把组 AABB 一起删掉：`gpuScene` 的 primitive 还靠它做粗筛与调试显示。
- 不把 Hi-Z 金字塔改成「实例化遮挡」的默认路径：决策 61 已经量过收益为 0。
- 不动 `recordDraw` 的一条 multidraw：命令内容变了，提交方式没变。

### 踩到的坑

1. **`radius` 与未归一化平面**：旧组级 `frustumVisible` 的 `radius = length(extent)` 少乘了平面的 `|plane|`，比逐实例那套 `abs(p.x)*half.x + ...` 更爱剔 —— 这才是「每帧漏 1 个实例」的根因。删函数的时候要把「它一直在误杀」写清楚，否则下次还会有人照着老公式写第三份。
2. **`projectionMatrix[1][1]` 是负的**（`src/Scene/Camera.cpp:15`，Vulkan 的 Y 轴朝下）。屏率公式里 `radius * (0.5 * screen.y * screen.z) / distance` 拿到负数，于是 `screen.x > 0` 时**每个实例都被判为「太小」** —— 这个 bug 从组级那版就在，只是组 AABB 太大、从来触发不到这条分支，所以没人发现。修法是 `abs(frame.screen.z)`（与 `SunFlarePass.cpp:176`、`ViewModelPass.cpp:245-247` 同一个理由），改完 `--gpu-scene-min-px` 才第一次真的分级响应。
3. 组的 `instanceCount == 0` 是常态（default 场景 103 条组里有 40 条没有实例），旧代码把它算「可见」，退役后算「剔除」，`visibleRatio` 的分布跟着变 —— 看读数时别把它当退化。

## 决策 64：实例记录先补上「上一帧世界矩阵」，这一步只铺数据不消费（P2 第十二步）

### 问题

P5-1 的运动矢量要算「这一帧的裁剪坐标」与「上一帧的裁剪坐标」之差，后者需要**上一帧的模型矩阵**。`InstanceRecord` 只有 `model`（当前帧），`src/Render/GpuScene.h` 的 `Instance` 也只有 `transform`；计划里 P2-2 的字段清单本来写了这一项（「世界矩阵 / 法线矩阵 / primitiveId / flags / 上一帧世界矩阵」），T-08 落地时只做了只读骨架。

这一刀只铺数据：加字段、填值、把「上次重建以来有多少实例动过」打出来，**画面与所有读数必须一位不变**。消费者留给 P5-1。

### 做法

`InstanceRecord` 从 224 涨到 288 字节，尾部加 `glm::mat4 previousModel`（offset 224，`static_assert` 锁住）；`src/Render/GpuScene.h` 的 `Instance` 同步从 144 涨到 208，尾部加 `previousTransform`。两个 shader（`shaders/scene_cull.comp`、`shaders/mrt.vert`）里的 `InstanceRecord` 镜像也要加同名字段 —— 这一刀不读它，加它只为 stride 与 C++ 一致。

填值不需要新的持久化状态，两套路径都现成：

| 路径 | `previousModel` 的来源 |
|---|---|
| 实体（`src/Render/SceneGpuBatch.cpp` 的实体分支） | `worldTransform.previousMatrix * nodeMatrix`（渲染插值本来就在用 `previousMatrix`） |
| 外部，命中 `ExternalRecordCache` | `external.transform`（缓存命中即「变换没变」，上一帧就是当前帧） |
| 外部，未命中但同绑定 | `cache.transform`（上一条记录的变换） |
| 外部，首次出现 | `external.transform`（没有上一帧，按当前帧算） |

同时在 `buildInstances()` 里统计 `historyInstances` / `historyMoved`（用 `memcmp` 比 `model` 与 `previousModel`），每次重建后由 `logCullSummary()` 打一行 `[SceneGpu] instance history | instances=… moved=… (last rebuild)`。

### 为什么塞进同一条记录，而不是另开一块缓冲

三条：其一，shader 只有两处按实例取记录（`shaders/mrt.vert` 的 set 3 binding 0、`shaders/scene_cull.comp` 的 set 0 binding 5），字段放同一条记录，stride 天生一致，没有第二套索引；其二，另开缓冲每条实例省 64 字节，代价是多一个 binding、一套描述符、一次额外的按实例寻址（着色器里就是多一次 `instances[i]` 之外的 load）；其三，现在没有按需流式加载，实例缓冲是整块常驻，带宽不是瓶颈。

代价是显存的固定开销：30k 场景 × 3 帧槽 × 288 字节 = 26.0 MB（原 224 字节时 20.2 MB），多 5.8 MB。这个数写在 `SceneGpuBatch::ensureCapacity` 的容量推导里，改记录尺寸时会被 `static_assert` 与这里同时提醒。

### 为什么「上一帧」等于「上一次结构上传」

决策 56 的结论：稳定帧不重建结构，移动实例改签名让每帧重建。所以「上一次结构上传」在静止场景与「上一帧」等价（`moved=0`，`previousModel == model`），在运动场景就是上一帧（`moved=10000`，实测）。

边界：`--rebuild-structure n` 强制每 n 帧重建时，粒度退化成 n 帧 —— 此时 P5-1 算出来的 velocity 是「n 帧的平均位移」，会偏大。这一条要在 P5-1 落地时重新确认（要么禁止与 `--rebuild-structure` 同用，要么把 `previousModel` 改成逐帧更新）。

### 验收（2026-11-26）

Release，120 帧，固定机位 `--camera -4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`：

| 场景 | `instance history` | `instances/frame` | `gpuInst` |
|---|---|---|---|
| default | `instances=103 moved=0` | 60.0 | 0.58 (7080/12154) |
| `--cube-field 10000` | `instances=10103 moved=0` | 6816.0 | 0.6747 (804288/1192154) |
| `--cube-field 30000` | `instances=30103 moved=0` | 11217.0 | 0.3726 (1323606/3552154) |
| `--cube-field 10000 --cube-field-motion 500` | `instances=10103 moved=10000` | 6788.7 | 0.67 (801071/1192154) |

三档的 `instances/frame`、`submit/frame`、`gpuInst` 与 P2-5 逐位相同 —— 这是「shader 侧 stride 对齐」的间接证明（少声明一个 `mat4` 不会报错，只会从第 1 个实例开始读错位的数据，`gpuInst` 立刻会掉）。

Debug 五组（default / cf10k / motion / `--no-instance-cull` / `--voxel-world 1`）全部 `[Validation] errors=0 warnings=0`、stderr 0 字节；全量回归 **96 项 ALL PASS**（`build/validation/20261126-092708`）；DocCheck PASS（documents=81 lines=18673）。MSBuild（`build`）与 Ninja（`build-ninja`）两套构建各跑一遍三档 + motion，读数一致。

### 代价与边界

- 记录 224 → 288 字节，`GpuScene::Instance` 144 → 208；30k 场景实例缓冲 20.2 → 26.0 MB。
- `flattenUs` 在噪声内（10k 8388 → 8435 us、30k 20936 vs 21106 us），`moved` 的 `memcmp` 是每实例 64 字节比较，摊平期一次性成本。
- `historyInstances/historyMoved` 只反映**最后一次**重建，稳定帧不刷新 —— 读数是「上次重建的快照」，不是累计。
- 这一刀没有消费者：如果 P5 最终不做，这 64 字节要能整段删掉（`previousModel`、`previousTransform`、两个 shader 的镜像字段、统计行）。

### 不引入的

- 不做「按实例脏标记 + 只重写变化的那几条」—— 结构上传本来就是整块 memcpy（决策 56 摊平过，稳态≈0）。
- 不做双缓冲历史（现在的按帧槽缓冲天然就是三份历史）。
- 不在 shader 里读 `previousModel`（本步零消费者，读它只会让「读数不变」这条验收失去意义）。

### 收尾补丁：谁还按 224 假设（2026-11-26）

第一版只改了 `src/Render/SceneGpuBatch.h` / 两个 shader 的 `InstanceRecord`，漏了三处按 224 字节写死的消费者：

1. `shaders/mrt.frag` 也声明了一份 `InstanceRecord`（片元阶段用它取材质），第一版没加 `previousModel` → 片元侧 stride 仍是 224，从第 1 个实例开始材质参数读错位。验证层看不出这个问题，因为越界的只是「读到的值不对」，不是越界访问。
2. `src/Render/GeometrySystem.h:187` 的 `kTerrainChunkInstanceStride = 224u` —— 地形合批把每个 chunk 写成一行 `InstanceRecord`（`src/Render/GeometrySystemGpu.cpp:371-400` 按 `row[0..43]` 逐字段写），行距仍是 224，而 `shaders/mrt.vert` 按 288 索引 → 除第 0 个 chunk 外，地形全部读到错位的世界矩阵。
3. `src/Render/GeometrySystem.h:206` 的 `kVoxelBatchInstanceStride = 224u` 同理（体素合批的 chunk 行）。

修法：两个 stride 常量改成 288 并注明来源是 `sizeof(SceneGpuBatch::InstanceRecord)`；`shaders/mrt.frag` 的 `InstanceRecord` 补 `mat4 previousModel`；`shaders/scene_cull.comp` 里「224 字节记录」的注释改 288。行内布局没动（`row[38] = misc.z` 这类偏移都还在 224 字节之内），所以写行代码一行没改，多出来的 64 字节保持 0。

怎么发现的：A1 的验收靠「三档读数与 P2-5 逐位相同」——但读数只走 `scene_cull.comp` 与 `mrt.vert`，碰不到 `mrt.frag` 与地形/体素行距，所以「读数不变」证明不了 stride 全对齐。补做的检查是「全仓搜 224 与 `InstanceRecord` 的镜像声明」，再用 `--streaming-world 2` 的合批地形截图 A/B：修前地形沉在水面以下（行距错位把世界矩阵读花），修后地形正常覆盖近景，两张图 1920×1080 有 1,230,977 个像素不同（59.4%）。这类「只有像素能抓到」的回归要记进 P5 的验收清单。

补丁后复验：Debug 四组（`--streaming-world 2` 合批地形 130 帧、`--no-terrain-batch` 60 帧、`--voxel-world 1` 40 帧、默认 40 帧）全部 `errors=0 warnings=0`、stderr 0 字节；全量回归 **96 项 ALL PASS**（`build/validation/20261126-094557`）；DocCheck PASS（documents=82 lines=18689）。

## 决策 65：反射视图有自己的剔除与紧凑命令，实例与组表只上传一次（P2 第十三步）

### 问题

平面反射通道一直走 CPU 逐实体提交：`GeometrySystem::render(cmd, frameInfo, &viewOverride)` 里每个 GPU 批分支都带 `!reflection`。P2-5 之后组级剔除退役，间接命令完全由 compute 按「逐实例可见性」每帧重写，命令里的 `firstInstance` / `instanceCount` 是主视锥下的紧凑区间 —— 反射视图换个视锥，这段区间就不成立，所以它不能复用主视图的命令。

### 做法

- `SceneGpuBatch` 拆出 `ViewResources`：`commands` / `frameData` / `stats` / `cullGroups` / `instanceRemap` 加 `computeSet` / `instanceSet`，`kMaxViews = 2`（0 = 主视图，1 = 平面反射）。`FrameResources` 只留两个视图共享的 `instances` 与 `groups`。
- `prepare()` / `recordDraw()` 变成 view 0 的薄包装，新增 `prepareView(cmd, frameInfo, viewIndex, viewProjection, view)` 与 `recordDrawView(cmd, frameInfo, viewIndex)`。
- 结构上传只由主视图做：`uploadStructure = viewIndex == 0u && (签名或容量变了)`。反射视图读同一份实例/组表，不重复搬 30k 条记录。
- 描述符池按 `kFramesInFlight * kMaxViews` 扩，`writeDescriptors()` 按视图各写一套；graphics set 的 binding 0 都指向同一份实例缓冲，binding 1 各指自己的 remap 表。
- 反射的剔除在 RenderGraph 里单独加一个不带附件的 pass（`PlanarReflection.Cull`），排在 `PlanarReflection.GBuffer` 之前；`GeometrySystem::render` 按 `viewIndex` 选 instanceSet、push `clipY` / `clipMode`，末尾调 `recordDrawView`。
- 门槛与主视图一致：`useGpuPath(frameInfo) && hasInstances()`。实例数低于 `gpuSceneMinInstances`（默认 256）时，反射继续走 CPU 逐实体 —— 小场景逐实体反而更省，这条判据不该按视图分叉。

### 为什么反射的剔除要放进独立的空 pass

compute dispatch 与 `vkCmdPipelineBarrier` 不能出现在动态渲染里。第一版把 `prepareView` 直接写在反射的 G-Buffer pass 里，Debug 12 帧就报 96 条验证错误：`VUID-vkCmdDispatch-renderpass`（48 条，`vkCmdDispatch: It is invalid to issue this call inside an active VkRenderPass`）与 `VUID-vkCmdPipelineBarrier-shaderTileImageColorReadAccess-08718`（48 条）。挪到独立 pass 后归零。

### 验收（2026-11-26）

场景 `--selftest 60 --streaming-world 2 --gpu-scene-external 400 --camera -4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765`：

| 项 | 读数 |
| --- | --- |
| 反射视图 | `[SceneGpu] reflection view | groups=103 instances=503 cull=on` |
| 主视图 | `visibleRatio=0.58`、`gpuInst=0.91 (26680/29174)`、`submit/frame=460` |
| 与 HEAD（反射走 CPU 逐实体）逐像素 | 1920×1080 差异 347 像素（0.0167%），HUD 面板外 8 个像素，差值均为 1/255（水面合成舍入） |

Debug 六组全部 `errors=0 warnings=0`、stderr 0 字节：水面 + 外部实例 40 帧、水面无外部实例 40 帧、`--no-instance-cull` 40 帧（反射视图也走恒等 remap，日志 `cull=off`）、`--no-hiz` 40 帧、`--voxel-world 1` 30 帧、默认 40 帧。

全量回归 **96 项** ALL PASS（`build/validation/20261126-102435`）。

### 代价与边界

- 每视图多 5 块缓冲：`commands`（组数 × 20 B）、`frameData`（256 B）、`stats`（64 B）、`cullGroups`（组数 × 16 B）、`instanceRemap`（2 × 实例容量 × 4 B）；描述符集数量翻倍（3 帧 × 2 视图）。
- 反射视图每帧多一遍四段 dispatch。水面场景实例数少（503），这一遍的摊平时间在 4~5 ms 量级，与主视图同数量级。
- 反射视图的 `stats` 不读回来（`readStats` 仍只读 view 0），所以只有一次性 `reflection view` 行，没有反射视图的逐帧可见/提交量；要拿这个读数得再开一条通路。
- 阴影视图仍未接（A2 的另一半）：`src/Render/ShadowPass.cpp` 还没绑 set 3，也没有自己的实例集。
- 反射不能复用主视图命令，这是有意的：两张视锥 + 不同 `clipY`，共用一段紧凑区间必然对不上。

### 踩到的坑

`config/engine.json` 会被运行中的引擎写回相机位姿。回归用例 `terrain-render`（`--selftest 20 --empty-scene --terrain-test`）沿用配置里的相机，相机一旦被前面的截图跑漂到水面以下（y=0.766 < `waterLevel = 2.0`），地形 chunk 全被剔掉，`[Terrain] GPU selection selfTest` 的 parity 记录为空，用例报 `missing stdout marker '[Terrain] GPU selection selfTest PASS'`。恢复 `config/engine.json` 后同一二进制立刻 PASS。规矩：跑回归、提交前先 `git checkout -- config/engine.json`；用例本身不该依赖配置里的相机，这一条留给后续清理。
## 决策 66：阴影的每个级联各有一份剔除与紧凑命令，两条管线共存（P2 第十四步）

### 问题

到 P2 第十三步为止，主视图与平面反射都有自己的视图层（`kMaxViews = 2`）。阴影是最后一个还在 CPU 里逐子网格提交的通道：`ShadowPass::recordCascade` 每帧对 3 个级联各遍历一遍场景，每个静态子网格一条 `vkCmdDrawIndexed`（`[Shadow] first frame | cascades=3 entities=3 terrainChunks=0 drawCalls=309`）。类头注释原本写着取舍：「GPU 批量只为一个视图（主相机）生成命令，阴影要每级联一份……等体素/粒子真接进来再考虑」。视图层落地后这条前提不再成立。

### 做法

- `SceneGpuBatch::kMaxViews` 2 → **5**：0 主视图、1 平面反射、2/3/4 三个级联；每个视图一套 `ViewResources`（命令、帧数据、统计、组计数、remap），实例与组表只由主视图上传。
- `prepareView(..., bool screenSpaceTest = true)`：级联传 `false`，`frameData.screen.x = 0` —— 阴影不看屏幕像素阈值，也不开 Hi-Z（`frameData.params.z` 只在视图 0 有效）。
- 把「绑合并网格顶点/索引 + `vkCmdDrawIndexedIndirect`」抽成公开的 `drawMergedIndirect(cmd, frameInfo, viewIndex)`，阴影绑定自己的深度管线后直接调它。
- 新着色器 `shaders/shadow_gpu.vert`：set 3 binding 0 = `InstanceRecord[]`、binding 1 = remap，push constant 只剩 `mat4 lightViewProjection`（64 字节），模型矩阵从实例缓冲取。
- `ShadowPass` 并存两条管线：逐模型路径（`shadow.vert`，0 个描述符集、128 字节 push）与 GPU 批（`shadow_gpu.vert`，4 个 set、64 字节 push）。GPU 批在 `addToGraph` 里、进入渲染通道**之前**给三个级联各派发一次四段 `prepareView(cmd, frameInfo, 2 + cascade, cascade.viewProjection, glm::mat4(1.0f), false)`；`recordCascade` 里每个级联 `gpuPipeline->bind` → 绑 set 3 → 一条 `drawMergedIndirect` → 切回逐模型管线画地形/体素/动态物体。静态网格在 GPU 批生效时跳过（它们已经在合并网格里）。
- 应用侧在 `Application::initAll()` 之后调 `shadowPass->setSceneGpuBatch(geometryPass->getSceneGpuBatch())`：`SceneGpuBatch` 在 `GeometryPass::init` 里才创建，阴影 Pass 的 `init` 阶段拿不到。

### 为什么是两条管线，而不是一条

`shadow.vert` 一个描述符集都不绑，这是它的优点（与材质/实例表完全解耦）。让两条路径共用一条管线、把 set 3 静态写进 `shadow.vert`，会同时坏两件事：

1. `ShadowPass::init()` 时 `SceneGpuBatch` 还不存在，拿不到实例布局 —— 管线布局与 shader 的引用对不上，Debug 直接报 `VUID-VkGraphicsPipelineCreateInfo-layout-07988`（`Set 3 Binding 0 in shader ... uses descriptor slot ... but not declared in pipeline layout`）。要修就得把管线创建推迟到 `initAll` 之后，等于把初始化顺序的耦合换个地方放。
2. 逐模型路径每条 draw 也得给 set 3 绑一份（哪怕内容不用），CPU 路径平白多一次绑定。

两条管线各自一个 push range、互不干扰，代价只是一份 2048² 深度配置的重复创建。

### 为什么级联不做屏幕误差剔除

`--gpu-scene-min-px` 的语义是「在主视图上小于 N 像素的实例」，跟阴影图分辨率无关；级联里做这件事只会让远处物体在阴影里闪。级联只需要视锥剔除（Hi-Z 目前不启用）。

### 验收（2026-11-26）

- Debug 四组，全部 `[Validation] errors=0 warnings=0`、stderr 0 字节、exit=0：
  - `--selftest 20 --streaming-world 2 --gpu-scene-external 400` → `[Shadow] gpu batch | cascades=3`
  - 同上再加 `--no-instance-cull`
  - `--selftest 30 --gpu-scene`
  - `--selftest 20 --voxel-world 1` → 不强开 GPU 批，逐模型路径照常
- 阴影 A/B（同二进制、临时环境变量探针退回 CPU 逐模型路径，`--no-taa --no-water`，第 40 帧 1920×1080）：总差异 77,254 像素，其中 δ≥8 只有 **2,681（0.13%）**，全部落在阴影边缘；裁图对比两版一致 —— 合并网格把 `lightViewProjection * model` 从 CPU 挪进顶点着色器，矩阵乘的顺序与舍入不同，阴影图纹素级抖动。（带 TAA/水时同对照 123,964 像素 —— TAA 会把亚像素差放大成整块。）
- 主视图与反射不受影响：`gpuInst=0.91 (17480/19114) submit/frame=460`、`reflection view | groups=103 instances=503 cull=on`。
- 全量回归 **96 项** ALL PASS（`build/validation/20261126-111338`）；DocCheck PASS（documents=82 lines=18838）。

### 代价与边界

- 每个级联多一次四段 dispatch；30k 实例场景的逐实例剔除 ×3，计入总帧时间后仍在噪声内。
- 阴影视图的组/实例表来自**上一帧**的结构上传（视图 0 的 `prepare` 在几何 pass 里，晚于阴影 pass）；静止场景一致，`--cube-field-motion` 这类每帧重建的场景滞后一帧。
- 合并网格重建时旧缓冲不能立刻销毁（同帧早先的阴影命令还引用它），改成按帧槽退役（`retiredMergedBuffers`）。
- 地形、体素合批、动态物体仍走逐模型路径：前两者有自己的 arena/合批，后者需要每帧矩阵。

### 不引入的

- 不做「阴影复用主视图的可见表」：级联视锥与主视图不同，复用会漏投影。
- 不把采样/过滤改动放进这一步；接触硬化阴影仍是 P7 的内容。

### 踩到的坑

1. `VUID-VkGraphicsPipelineCreateInfo-layout-07988`：一条管线同时服务两条路径时，`set 3` 在 shader 里静态引用、但管线布局创建时 `sceneInstanceSetLayout` 还是 `VK_NULL_HANDLE`。
2. `UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkBuffer`（8 帧 220 条）：`ensureMergedMesh` 重建时立刻 `destroyBuffer`，而同帧的阴影视图已经把旧缓冲绑进了命令缓冲。改成「推进 `retiredMergedBuffers[currentFrameSlot]`，下一轮同槽的 fence 等到后再销毁」。
3. `setSceneGpuBatch` 必须在 `Application::initAll()` 之后调，否则 `getSceneGpuBatch()` 还是空指针。
## 决策 67：运动矢量进 G-Buffer，P5-1 只铺数据（2026-11-26）

### 问题

TAA 一直用「当前帧与上一帧的矩阵」反推重投影，没有逐像素的运动矢量：相机在原地、物体在动时，TAA 会把移动物体的旧像素混进来，出现拖影与边缘抖动。P5-1 要求 G-Buffer 里有一张速度图；P5-2 再把 TAA 改成读它。P2-2 铺的 `InstanceRecord::previousModel` 正是这张图缺的那一半数据。

### 做法

- `GBufferFormat::Velocity = VK_FORMAT_R16G16_SFLOAT`：存「NDC 的一半」= UV 增量，TAA 可以直接加到采样坐标上。16F 的 ULP 在 0.5 像素判据下仍有半个数量级余量，带宽只有 32F 的一半。
- `GBuffer` 多一张附件（`VK_IMAGE_USAGE_TRANSFER_SRC_BIT`，给后续 `--velocity-probe` 拷回主机统计），`GeometryPass` 导入并在每帧开头 `CLEAR`。
- `GeometrySystem` / `VegetationPass` 的管线从 4 个颜色附件扩到 5 个（混合状态同步），`mrt.vert` / `mrt.frag` 写第 5 个输出；`skybox_mrt.frag` / `grid_mrt.frag` / `sky_geometry.frag` / `vegetation.frag` 显式写 `vec2(0.0)`（它们不产生屏幕空间运动）。
- `mrt.vert`：UBO 尾部补 `int numLights; float exposure; mat4 previousProjectionView;`（std140 下 `previousProjectionView` 必须落在偏移 144 —— 这两个标量不能省），顶点算两个裁剪空间位置 `fragCurrClip` / `fragPrevClip`，片元 `outVelocity = (curr.xy/curr.w - prev.xy/prev.w) * 0.5`。
- 上一帧的模型矩阵：`push.gpuScene != 0u` 时取 `instances[instanceIndex].previousModel`，其余路径（普通/地形）恒等于当前矩阵。
- 反射通道复用同一批管线，它的 G-Buffer 也必须补齐第 5 张（不读，见下）。

### 为什么速度用 UV 单位，而不是像素

TAA 的重投影是「上一帧的 UV → 本帧 UV」，直接相减即可；像素单位还要跟着 extent 变，resize 后容易忘。NDC 的一半正好等于 UV 增量（Vulkan 的 NDC 是 [-1,1]，UV 是 [0,1]）。

### 为什么「上一帧的模型矩阵」塞在实例记录里

见决策 64：着色器只有一处按实例取记录，`model` 与 `previousModel` 在同一条 stride 里，`mrt.vert` / `shadow_gpu.vert` / `mrt.frag` 的声明天生一致；另开缓冲要省 64 字节却要多一个绑定和一次下标计算。

### 验收（2026-11-26）

- Debug 三组 `[Validation] errors=0 warnings=0`、stderr 0 字节、exit=0：默认场景 30 帧、`--streaming-world 2 --gpu-scene-external 400`、`--cube-field 10000 --cube-field-motion 500`。
- 第一版在流水场景报了 89 条 `VUID-vkCmdDrawIndexed-Indirect-colorAttachmentCount-06179`：反射通道的 G-Buffer 还是 4 张、却复用了 5 个颜色附件的管线。补上第 5 张后归零。
- 本步没有消费者，画面不变；逐像素 A/B 与「velocity 与 CPU 投影差分 < 0.5 像素」的判据留到 P5-2（TAA 改读它）一并验。

### 代价与边界

- 每像素多 4 字节写带宽；G-Buffer 常驻内存 +4 字节/像素（1920×1080 ≈ 8 MB）。
- 反射视图的 velocity 只被清零，不读也不算 —— 它的相机在主视图镜像位置，若将来要让反射也用运动模糊，需要单独一份重投影矩阵。
- 地形/体素合批的实例行 `previousModel` 目前是 0 矩阵；这两条路径 `push.gpuScene == 0`，着色器不读它，所以不构成错误，但等 P5-2 想给它们算运动时要补上。

### 不引入的

- 不做「相机运动补偿」的额外 pass：`previousProjectionView` 已经在 UBO 里，顶点算一次就够。
- 不做半分辨率 velocity：TAA 的采样点在整像素上，降采样省下来的带宽还不够补插值的麻烦。

### 踩到的坑

1. UBO 偏移：只在 `mrt.vert` 里加 `mat4 previousProjectionView` 会落在 128（`cameraPos` 之后），而 C++ 侧是 144 —— 中间那两个标量（`numLights` / `exposure`）必须一起声明。
2. 共用管线的通道必须同样多：反射的 G-Buffer 少一张就报 06179，且只在走反射的场景里出现（默认场景水面关着，看不出来）。

**全量回归（2026-11-26，Debug）**：96 项 ALL PASS（`build/validation/20261126-114143`），stderr 全 0 字节；DocCheck PASS（documents=82 lines=18906）。

### 反射视图的帧级统计（2026-11-26 补）

`readStats(frame, viewIndex = 0)` 现在对 view 1 也回读一次，只累加「组数 / 可见组 / 提交实例 / 实例可见率」到独立的 `CullTotals reflectionTotals`（不碰 `stats`、不参与摊平计时 —— 反射视图没有自己的摊平，实例与组表由主视图上传）。`logCullSummary()` 在 `instance history` 之前多打一行：

`[SceneGpu] reflection summary | frames=37 visibleRatio=0.30 | groups/frame=103.0 visible=31.0 culled=72.0 | submit/frame=430.0 | gpuInst=0.85 (15910/18611)`

同一帧里主视图是 `visibleRatio=0.23 / visible=24 / submit=423 / gpuInst=0.84` —— 镜像相机看到的东西不同，量级一致，说明两份剔除都在按自己的视图工作。
