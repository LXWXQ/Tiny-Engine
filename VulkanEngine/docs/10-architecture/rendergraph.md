---
id: architecture/rendergraph
title: 渲染图：屏障推导与语义校验
type: architecture
scope: src/RHI/RenderGraph.h, src/RHI/RenderGraph.cpp, src/Render/IRenderPass.h
read-when: 修改任何 Pass 的资源声明、新增资源、调整 Pass 顺序、调试 layout/hazard 报错
size: ~400 行
updated: 2026-10-30
---

# 渲染图：屏障推导与语义校验

`RenderGraph`（约 1200 行）是这个项目的核心。它只做三件事：

1. 记录"每个 Pass 读/写了哪些资源的哪些子区域"；
2. 据此**自动插入** `VkImageMemoryBarrier2` / `VkBufferMemoryBarrier2`；
3. 在 `endFrame()` 时校验语义不变量，违反就抛异常。

**Pass 从不手写屏障。** 这是全项目最重要的一条纪律。

---

## 1. 线性模型

Pass 严格按 `addGraphicsPass` / `addComputePass` 的调用顺序执行，**不做**重排、
自动剔除、内存别名化。原因见 [decisions](decisions.md) 决策 2。

因此"当前状态"就是"上一次访问后的状态"，顺序扫描即可，不需要拓扑排序。
这让整个语义校验只有几十行。

---

## 2. 核心 API

```cpp
// 资源创建 / 导入
TextureHandle createTexture(const TextureDesc& desc);      // 图持有，随图销毁
TextureHandle importTexture(VkImage, VkImageView, ...);     // 外部持有（交换链等）
BufferHandle  createBuffer(const BufferDesc& desc);
BufferHandle  importBuffer(VkBuffer, ...);

// 在 Pass 内声明访问
pass.read(handle);                                  // 整张图
pass.read(handle, SubresourceRange{mip, layer});    // 指定子资源
pass.writeColor(attachmentIndex, handle, LoadOp::CLEAR);
pass.writeDepth(handle, LoadOp::CLEAR);
pass.readBuffer(handle, BufferRange{offset, size});
pass.writeBuffer(handle, BufferRange{offset, size});
```

`Pass` 内部把声明累积成 `ResourceAccess` 列表，`endFrame()` 再统一处理。

### 2.1 句柄与所有权

| 来源 | 谁销毁 | 用途 |
|---|---|---|
| `createTexture` / `createBuffer` | `RenderGraph`（随图析构或 `resize` 重建） | G-Buffer、SSAO、HDR、Bloom 金字塔 |
| `importTexture` / `importBuffer` | 调用方 | 交换链图像、外部常驻资源 |

`importTexture` 会把"初始布局 != `UNDEFINED` ⇒ 常驻资源"这个结论**回写**进
`TextureDesc::persistent`。这个回写是 W2 语义校验豁免的依据——**同一事实只能有一个来源**。

---

## 3. 屏障推导

### 3.1 每张图记住的状态

```cpp
struct SubresourceState {
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};
// TextureEntry 里: std::vector<SubresourceState> subresources;  // 按 mip/layer 展开
struct BufferEntry { BufferRange 已写区间; VkPipelineStageFlags2 stage; VkAccessFlags2 access; };
```

### 3.2 发射规则

对每次访问，比较"要的 (layout, stage, access)"与"上次的"：

| 情况 | 动作 |
|---|---|
| layout 相同、无写冲突 | 只更新 access/stage，不发射屏障 |
| layout 不同 | 发射 `VkImageMemoryBarrier2`（`oldLayout`/`newLayout` 取自记录） |
| 有写 -> 写 / 写 -> 读 / 读 -> 写 | 发射屏障并设置正确的 `srcStageMask`/`srcAccessMask` |
| 同一 Pass 内对同一资源多次访问 | **只保留最后一次声明**（Bloom 上采样累加就靠这条） |

### 3.3 三个容易漏的细节（都已踩过）

1. **`LOAD_OP_LOAD` 是读。** `writeColor`/`writeDepth` 若只声明 `*_WRITE`，
   syncval 报 `SYNC-HAZARD-READ_AFTER_WRITE`。修法：`loadOp == LOAD` 时把
   `*_ATTACHMENT_READ_BIT` 也 OR 进 access。（TD-10）
2. **`imageAvailable` 的等待阶段必须是 `ALL_COMMANDS`。** 只等
   `COLOR_ATTACHMENT_OUTPUT` 时，交换链图像的布局转换可能发生在更早阶段，
   与 `vkAcquireNextImageKHR` 竞争，syncval 报 `WRITE_AFTER_READ`。（TD-10）
3. **`presentTransition` 必须显式声明。** 它不是普通 Pass：没有颜色/深度附件，
   渲染图看到"无附件"就只发屏障、不 `vkCmdBeginRendering`。

> 修完这两条之后 `--validation-features sync` 才是 **0 errors / 0 warnings**。
> 在这之前 syncval 的噪声会淹没新引入的问题，等于没有这个工具。

---

## 4. 子资源跟踪（TD-05）

早期实现以"整张图"为单位记录状态，问题：写了 mip0 再读 mip1 不会被发现。

现在：

- 纹理访问带 `SubresourceRange{baseMip, mipCount, baseLayer, layerCount}`（默认全图）；
- 每张图按 (mip, layer) 展开成独立的 `SubresourceState`；
- 屏障**只覆盖实际声明的子资源**，不误伤其它 mip；
- buffer 访问带 `BufferRange{offset, size}`，写覆盖校验按区间并集判断。

探针：

```powershell
.\build\Debug\VulkanEngine.exe --selftest 20 --subresource-test      # 正例
.\build\Debug\VulkanEngine.exe --subresource-error-test              # 反例：写 mip0 后读 mip1，必须被拒
```

---

## 5. 语义校验：瞬时资源必须有人写（W2）

**问题**：屏障正确不等于语义正确。如果下游读的瞬时图这一帧根本没人写，
它的布局会从 `UNDEFINED` 转出来——**这在同步上完全合法**，验证层一言不发，
但读到的是垃圾。

真实案例：`SSAOPass` 被 `setEnabled(false)` 跳过时，`lighting.frag` 里
`ambient *= ssao` 会乘上一片未定义内容。（见 [pitfalls](../60-debug-profiling/pitfalls.md) 坑 14）

**机制**：`RenderGraph::validateWriteCoverage()` 在 `endFrame()` 里按声明顺序扫一遍：

```
对每个被 read 的瞬时资源：
    如果本帧没有任何 Pass 写过它：
        抛异常，并指出
          - 资源名
          - 哪个 Pass 读了它
          - 它本来应该由哪个 Pass 写
          - 那个 Pass 是不是被 setEnabled(false) 跳过了
```

**豁免**：`TextureDesc::persistent == true` 的资源（IBL 烘焙结果这类整帧只读的）跳过检查。

**覆盖范围**：image（含子资源）+ storage buffer。目前**不覆盖**"单个 mip/层是否被写过"
的跨帧一致性——见 [tech-debt](../70-status/tech-debt.md)。

---

## 6. 跨队列依赖推导（TD-02）

### 6.1 资源侧

跨队列访问的资源必须 `crossQueue = true`，走 `VK_SHARING_MODE_CONCURRENT` 简单路径
（未实现 EXCLUSIVE ownership transfer，见 [known-limits](../70-status/known-limits.md)）。

### 6.2 依赖侧

渲染图增量推导 `CrossQueueDependencies`：

```
对每个资源，按 Pass 顺序看"谁写、谁读"：
    若 写者队列 != 读者队列  -> 记录一条 producer -> consumer 依赖
    若同一帧内出现 C->G 和 G->C 两个方向 -> 明确报错（环）
```

`Renderer::endFrame()` 据此决定 submit 顺序，并用 timeline semaphore 插入跨队列 wait。

### 6.3 验收

```powershell
.uild\Debug\VulkanEngine.exe --selftest 20 --cross-queue-test
# 期望输出：
#   [TD02] compute->graphics verified
#   [TD02] graphics->compute verified
```

两条探针分别验证 `compute SSBO -> graphics` 与 `graphics storage image -> compute`。

---

## 7. 声明合法性校验

在录制前检查：

| 检查 | 违反时 |
|---|---|
| 句柄是否属于本图 | 抛异常并报句柄 id |
| 访问类型与 `usage` 是否匹配（例如对非 `STORAGE` 图做 storage write） | 抛异常并报纹理名 |
| 子资源范围是否越界（mip/layer 超上限） | 抛异常并报 mip/layer |
| buffer 区间是否越界 | 抛异常并报 offset/size |

**为什么要点名**：只说"非法访问"的话读者还得自己翻是谁。直接报"`hdrScene` 的
mip 3 超出 mipLevels 1"才是可行动的。

---

## 8. GPU 计时挂钩

`RenderGraph` 在每个 Pass 前后调 `GpuTimer`，但**只在 `--gpu-timing` 开启时**插 timestamp：

```cpp
#ifdef SENTINEL_ENABLE_GPU_TIMING
if (gpuTimer) { /* vkCmdWriteTimestamp2 */ }
#endif
```

关闭时不创建 query pool、不插命令。零开销模型见
[zero-overhead](../60-debug-profiling/zero-overhead.md)。

---

## 9. 改动检查清单

修改渲染图或 Pass 声明后：

- [ ] 新资源是 `createTexture` 还是 `importTexture`？所有权写对了吗？
- [ ] 每个读操作都有对应的写？（否则 `validateWriteCoverage` 会抛）
- [ ] 跨队列资源标了 `crossQueue`？
- [ ] `LOAD` 语义的附件，读访问位是否声明了？
- [ ] 跑 `--selftest 120 --no-bloom --no-ssao` 四种组合，stderr 0 字节
- [ ] 跑 `--validation-features sync`，0 errors / 0 warnings
- [ ] 如果新 Pass 可被关闭，加了 CLI 开关并纳入 `tools/Validate.ps1`
