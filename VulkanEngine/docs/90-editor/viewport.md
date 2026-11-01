---
id: editor/viewport
title: 离屏视口
type: system
scope: 3D 画面如何从"铺满整窗"变成"ImGui 里的一张图"
read-when: 改视口尺寸/格式、加多视口、或编辑器里画面拉伸/发暗
size: medium
updated: 2026-10-30
---

# 离屏视口（TD-09）

## 问题

改造前 3D 画面直接铺满交换链，面板浮在它上面。三个后果：

1. 面板遮住画面内容，而不是"围绕"画面
2. 截图会把 UI 一起拍进去
3. 相机纵横比按**窗口**算，但可见区域比窗口窄 —— 画面横向拉伸

## 结构

```
PresentPass    ──写──> editorViewport[frame]   (交换链格式, COLOR_ATTACHMENT|SAMPLED)
                            │
                    ImGui::Image(句柄)
                            │
UiCompositePass ──写──> 交换链（清底色 + 提交整个 ImGui 绘制列表）
```

关键点：**同一条链上 PresentPass 写、UiCompositePass 读同一张图**，所以必须是两个 Pass。
同一 Pass 里既当渲染目标又被采样是布局冲突。

## 为什么纹理按 frame slot 各一张

和 `hdrScene` 同理：两帧在飞时，第 N+1 帧写视口会覆盖 GPU 仍在采样的第 N 帧内容。
所以 `editorViewport` 是 `std::array<TextureHandle, kFramesInFlight>`，
PresentPass 用 `frameInfo.frameInFlightIndex` 索引，UiCompositePass 用同一个下标。

**这里踩过坑**：一开始 `PresentPass` 只存了一个 `outputTarget` handle，
两帧都写同一张，而 `UiCompositePass` 按 slot 读另一张 —— 渲染图的
"瞬时资源必须有人写"校验当场报错：

```
Pass "UiComposite" 读取了瞬时纹理 "EditorViewport.Frame0" 的 mip 0 / layer 0，
但该子资源这一帧到目前为止没有任何 Pass 写过它。
```

这条校验的存在价值就在这里：它把"写错槽位"这种极难肉眼发现的 bug
变成了一句明确的中文错误。

## 格式必须和交换链一致

`present.frag` 里手写了 gamma 编码（`pow(color, 1/2.2)`），它假设目标是 UNORM 格式。
视口纹理如果换成 SRGB，硬件会在写入时再编码一次，编辑器里的画面会比运行时模式亮一档。

## 尺寸协商：编辑器报、引擎建

```
ViewportPanel  ──requestViewportSize(w,h)──>  EngineServices 回调
                                                      │
                                     存进 pendingEditorViewportExtent
                                                      │
                          下一帧 update() 开头才真正 createEditorViewportTextures()
```

为什么不在 UI 构建期直接重建：那时命令缓冲已经在录制，
重建纹理需要 `vkDeviceWaitIdle`，会直接死锁。

## ImGui 纹理句柄

引擎用 `ImGui_ImplVulkan_AddTexture(sampler, view, SHADER_READ_ONLY_OPTIMAL)`
把视口注册成 ImGui 纹理，编辑器通过 `getViewportTextureId(frameSlot)` 拿句柄。

布局必须写死 `SHADER_READ_ONLY_OPTIMAL`：`UiCompositePass` 里声明了 `read`，
渲染图会保证 ImGui 真正绘制时它确实处于这个布局。

## 坑：ImGui 描述符池容量

`ImGuiSystem` 的池原来只有 `maxSets = 1` / `COMBINED_IMAGE_SAMPLER = 1`。
注册第二张纹理时 `vkAllocateDescriptorSets` 失败并返回未初始化句柄，
随后绘制时解引用空描述符 —— **0xC0000005，没有任何线索**。

现在池容量是 64（`kImGuiTextureCapacity`）。以后要注册缩略图/资产预览时不用再改。

## 尺寸变更防抖

拖拽窗口边缘时面板尺寸每帧都在变。每帧重建 = 每帧一次 `vkDeviceWaitIdle`
+ 两张全屏纹理重建，缩放会卡成幻灯片。

判据是"尺寸连续 `kViewportSettleFrames`（2 帧 ≈ 33ms）没再变"，也就是用户松手了：

```cpp
const bool settled = (viewportDebounceClock - viewportRequestFrame) >= kViewportSettleFrames;
if (sizeDiffers && settled)
    createEditorViewportTextures(pendingEditorViewportExtent);
```

`pendingEditorViewportExtent` **粘住不清零** —— 编辑器只在尺寸变化时上报，
引擎靠"它连续若干帧没再变"判断拖拽结束。每帧上报都会刷新 `viewportRequestFrame`。

## 重建必须原地 resize

```cpp
// 对的
renderGraph->resizeOwnedTexture(handle, extent);

// 错的
handle = renderGraph->createTexture(desc);
```

`createTexture` 会在渲染图的资源表里**新增**一个条目，旧条目要等渲染图析构才释放。
拖拽缩放时每秒重建几十次 = 每秒泄漏几十张全屏纹理（1300x861x4B ≈ 4.5MB/张），
几秒钟就能把显存吃光。

## UI-free 截图

```
VulkanEngine.exe --editor --screenshot-viewport --selftest 70 \
    --screenshot pure.png --screenshot-frame 66
```

输出的是**视口纹理**（1300x861），不含任何 ImGui。默认 `--screenshot` 仍读交换链（含 UI）。

两个必要前提：

1. 视口纹理必须带 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`，否则
   `VUID-vkCmdCopyImageToBuffer-srcImage-00186`。
2. `recordCopy` 拷完必须**转回原布局**（`SHADER_READ_ONLY_OPTIMAL`）。
   渲染图记录的布局必须与真实布局一致，否则下一帧它会用错误的 `oldLayout` 插屏障。

## 已知限制

- 只有一个视口。多视口需要 `editorViewport` 从"每帧槽一张"变成"每视口每帧槽一张"。
- 防抖窗口 2 帧，快速来回拖动时画面会短暂按旧尺寸拉伸（松手即恢复）。

## 流送 chunk 拾取

射线命中 `TerrainChunkComponent` 后，Viewport 调用
`Scene::resolveTerrainChunkOwner()` 返回 manager 并选中它。这样 selection 不会
指向下一帧可能被卸载的 runtime 实体；日志会附带命中 chunk 坐标。
