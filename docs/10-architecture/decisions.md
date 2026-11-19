---
id: architecture/decisions
title: 关键设计决策与取舍
type: architecture
scope: 空（跨代码库的设计理由）
read-when: 想改变某个既有设计之前；或评审"这个方案为什么不用更好的做法"
size: ~289 行
updated: 2026-11-19
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