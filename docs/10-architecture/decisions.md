---
id: architecture/decisions
title: 关键设计决策与取舍
type: architecture
scope: 空（跨代码库的设计理由）
read-when: 想改变某个既有设计之前；或评审"这个方案为什么不用更好的做法"
size: ~700 行
updated: 2026-11-23
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
