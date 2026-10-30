---
id: debug-profiling/pitfalls
title: 已踩过的坑
type: debug
scope: 空（跨代码库）
read-when: 改任何代码之前；或遇到"看起来不可能"的怪异现象时
size: ~430 行
updated: 2026-10-30
---

# 已踩过的坑

都是**真实踩过并已修复**的。每条给"现象 / 原因 / 判据 / 修法"，只写结论等于没写。

## 1. CRLF 混杂导致补丁静默失效

- **现象**：脚本按行替换，跑完不报错，但文件没变。
- **原因**：工程里同时存在 CRLF 与 LF 文件。模式串用 LF 而文件是 CRLF 时**匹配不上却静默成功**。
- **判据**：改完立刻 `Select-String` 确认目标串真的变了。
- **修法**：按"整段唯一子串"替换，而不是按行；或先统一行尾。

## 2. `tiny_gltf.h` 的 include 顺序是硬要求

- **原因**：`tiny_gltf.h` 的**声明在 include guard 内**，而**实现部分在 guard 外**。
- **必须这么写**：

```cpp
#include "Scene/GLTFLoader.h"      // 1) 先拿声明（此时宏未定义）
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>             // 2) 再定义宏包含，实现恰好展开一次
```

- **反过来写**（先定义宏再包含）实现会展开两次，直接重定义编译失败。
- `TINYGLTF_NO_STB_IMAGE` / `TINYGLTF_NO_STB_IMAGE_WRITE` 由 CMake 全局注入，不要挪进单个 `.cpp`。

## 3. ImGui 动态渲染的字段位置（1.92.6）

- **现象**：按网上示例写 `init_info.PipelineRenderingCreateInfo` 编译不过。
- **原因**：1.92.6 的字段在 **`init_info.PipelineInfoMain`** 下面。
- **另一个坑**：`pColorAttachmentFormats` 指向的格式变量必须是活到 shutdown 的成员，
  不能是临时变量。详见 [imgui](../30-systems/imgui.md)。

## 4. 绝对不要用 0 当"未分配"哨兵

- **现象**：天空盒/程序化网格"悄悄"用上了第 0 号材质——只是恰好那槽已分配才没崩。
- **原因**：`materialSetIndex` 默认 0，而 0 是**合法槽位号**。
- **修法**：统一 `kInvalidMaterialSlot = UINT32_MAX`（`src/Core/MaterialSlot.h`）。
- 同理 `GeometrySystem::render` 的 `lastBoundSlot` 初值也必须是 `kInvalidMaterialSlot`，
  否则第一个子网格会被误判为"已绑定过"而跳过 bind。
- **这是一类问题**：任何"索引/句柄"的无效值都不能用合法值。

## 5. 录制完就销毁命令缓冲引用的资源是错的

- **原因**：命令缓冲只是**录进去**了，还没提交执行。此时销毁它引用的 image/view/buffer，
  后续 `vkQueueSubmit` 指向失效对象。
- **现象**：验证层刷屏，而且**往往刷在几十帧之后**，很难联想到根因。
- **修法**：等 `cmdEndSingleTime()`（内部 `vkQueueWaitIdle`）或对应 timeline 完成后才销毁。
  见 [resource-lifetime](../10-architecture/resource-lifetime.md) §4。

## 6. 动态 viewport/scissor 必须在录制时显式设置

- **原因**：所有图形管线启用 `VK_DYNAMIC_STATE_VIEWPORT` + `VK_DYNAMIC_STATE_SCISSOR`，
  这两个状态**不会被烘焙进管线**。忘了设就是未定义行为。
- 渲染图会在 `vkCmdBeginRendering` 之后自动调 `cmdSetFullViewportAndScissor()`。
- **但不走渲染图的路径**（`IBLBaker::renderFaces` / `renderBrdfLut`）必须自己调。
- ImGui 后端自己设，PresentPass 里不用补。

## 7. 非一致（non-coherent）映射内存必须显式 flush

- **现象**：GPU 读到上一帧的旧数据（UBO 更新"没生效"）。
- **原因**：持久映射的 UBO 不保证落在 `HOST_COHERENT` 内存。
- **修法**：`memcpy` 之后调 `vmaFlushAllocation(...)`。它对 coherent 内存是空操作，
  所以可以**无条件调用**。

## 8. SSAO 的遮挡判断必须在视图空间做

- **现象**：转相机时遮蔽随朝向乱变（侧面失效、地面出现方向性条纹）。
- **原因**：原实现在**世界空间直接比较 z**——只有相机恰好朝 -Z 时才成立。
- **修法**：把位置和法线都转到视图空间再比较（`ssao.frag` 有详细注释）。

## 9. 法线贴图 UV 接缝与退化

- **现象**：模型上一条条黑缝。
- **原因**：跨界像素的 `dFdx(uv)` 巨大，TBN 爆掉。
- **修法**：`mrt.frag::getNormalFromMap()` 两道拦截：UV 跨度 > 0.5 回退顶点法线；
  行列式 < 1e-5 回退。
- **当前状态**：该函数**有意不调用**（模型还不能提供可靠切线）。

## 10. glTF 的 UV 原点与 OBJ 相反

- glTF 的 UV 原点在**左上**，与 Vulkan 一致 -> 直接用。
- OBJ 的 UV 原点在**左下** -> `Model.cpp` 的 OBJ 路径要做 `1.0 - v`。
- **两条路径翻转策略不同，别统一。**

## 11. 脚本生成 C++ 字符串字面量时 `\n` 会被二次转义

- **现象**：写出来的文件里出现真实换行，C++ 报 `newline in constant`。
- **原因**：Python/脚本先把 `\n` 解释成了换行符。
- **修法**：写 `\\n`，或用 raw string（`r"..."`），或干脆不用脚本改源码。

## 12. `find_package(glfw3)` 与手动 include 路径并存

CMake 里既 `find_package(glfw3 REQUIRED)`，又把 `${GLFW_DIR}/include` 塞进 include，
同时链接目标写 `glfw`。这套组合在"3rdParty 有源码 + 系统也装了 glfw3"的机器上能工作，
但**依赖顺序脆弱**。换机器报 glfw 找不到时优先查这里。

## 13. 新增 `src/*.cpp` 后不重新 configure 会在链接期才炸

- **现象**：`unresolved external symbol`，但代码明明写了。
- **修法**：重跑 `cmake -S . -B build ...`。这是**最高频**的构建坑，见 [build](../40-build/build.md) §3.1。

## 14. 「把某个 Pass 关掉」是最容易漏验证层错误的分支

- **原因**：`setEnabled(false)` 一跳过，该 Pass 的**输出图整帧没人写**——布局停在 `UNDEFINED`，
  而后面照样有 Pass 声明要采样它。
- **为什么难发现**：这类问题在"正常跑"的分支里**永远测不到**；ImGui 复选框又没法在
  无人值守流程里点。
- **真实案例**：SSAOPass 跳过后，`lighting.frag` 里 `ambient *= ssao` **无条件**执行，
  环境光被乘上未定义内容。而且**验证层对这个洞一言不发**。
- **修法 A（当时）**：开关下沉到着色器，Pass 永远执行，`enabled` 判断提到最前面。
- **修法 B（现在）**：`RenderGraph::validateWriteCoverage()` 在 `endFrame` 里机器检查，
  "有人读的瞬时图没人写"直接抛异常，并告诉它本来该由谁写、那个 Pass 是不是被跳过了。
- **给新 Pass 的规矩**：只要"可以被关掉"或"有条件跳过"，就必须
  ① 给它命令行开关；② 在 `tools/Validate.ps1` 里加一项回归。

## 15. 销毁顺序不能靠成员声明顺序表达

- **现象**：退出时 VMA 断言崩溃。
- **原因**：`renderPipeline` 是值成员、声明得比 `vulkanDevice` **晚**，逆序析构时排在它之后，
  Pass 持有的 VMA 分配在 `vmaDestroyAllocator` **之后**才释放。
- **为什么藏了这么久**：验证脚本用 `TerminateProcess` 杀进程，**根本不跑析构函数**
  ——"验证层 0 字节"从未覆盖退出路径。
- **修法**：`~FirstApp` 把顺序全部显式写出（Pass -> 依赖窗口的 -> GPU 资源 -> Device -> 窗口），
  并给 `RenderPipeline` 加 `clearAll()`。
- **教训（更重要）**：验证脚本的"成功退出"方式决定你验证到了什么。
  必须用 `WM_CLOSE` 让主循环自己退出，不能 `terminate()`。

## 16. A/B 截屏之前，先确认两张图都是"本次运行"产出的

- **现象**：对比出 0.83% 像素不同，以为是回归。
- **原因**：上一次的截图文件是旧的，那一轮窗口位置不同，把标题栏截了进去
  （几十个像素从 `[24,25,26]` 跳到 `[239,239,239]`，那不是渲染差异）。
- **判据**：同构建连跑两次，排除 UI 面板后差异应该是 **0.000**。先拿到噪声下限再比。
  详见 [acceptance](../50-validation/acceptance.md) §2。

## 17. shader / 源码带 UTF-8 BOM 会编译失败

- **现象**：`glslangValidator` 报 `'#' : unexpected token` 或
  `ES shaders for SPIR-V require version 310 or higher`。
- **原因**：BOM 三个字节让 `#version` 没被识别。
- **修法**：去掉前 3 字节；检查脚本见 [shaders](../20-pipeline/shaders.md) §4。
- **预防**：不要用 PowerShell `Set-Content` / `Out-File` 写 shader 或源码。

## 18. `--target-env vulkan1.3` 要求启用 `maintenance4`

- **现象**：验证层报 `VUID-RuntimeSpirv-LocalSizeId-06434`（2 条，对应 2 个 compute shader）。
- **原因**：SPIR-V 1.6 的 compute 改用 `LocalSizeId` 执行模式，规范要求 `maintenance4`。
- **修法**：`features13.maintenance4 = VK_TRUE;`
- **反例（踩过）**：单独挂 `VkPhysicalDeviceMaintenance4Features` 会违反
  `VUID-VkDeviceCreateInfo-pNext-06532`（`Vulkan13Features` 已在链上时不允许再出现被提升的结构体）。
  因为 `maintenance4` **本身就是 `VkPhysicalDeviceVulkan13Features` 的成员**。

## 19. `Start-Process -ArgumentList` 不会给数组元素加引号

- **现象**：仓库路径含空格（如 `Vs code Demo`）时，
  `powershell -File <path>` 报 `Processing -File 'C:\Users\Admin\Desktop\Vs' failed`。
- **原因**：`Start-Process -ArgumentList` 把数组用空格 join，**不自动加引号**。
- **修法**：所有 `tools/*.ps1` 里有 `Quote-NativeArg` 辅助函数，
  传路径参数前必须过它：`($args | ForEach-Object { Quote-NativeArg $_ }) -join " "`。
  新增脚本时照做。

## 20. 回归脚本里 `%TEMP%` 不可写会误判为代码 bug

- **现象**：`--config-test` 报 `cannot create temp directory`。
- **原因**：受限沙箱下进程只能写工作目录子树。
- **修法**：把 `TEMP` / `TMP` 指到可写目录再跑。

## 21. `tools/*.ps1` 必须纯 ASCII 且无 BOM

- **现象**：脚本里"赋值/if 不执行"，行为极其诡异。
- **原因**：Windows PowerShell 5.1 读**无 BOM 的 `.ps1` 按 ANSI 解码**，
  中文注释会吞掉下一行代码。
- **修法**：所有 `tools/*.ps1` 只写 ASCII 注释。文档（`.md`）不受此限。
## 22. MSBuild 节点复用会让"重定向 + -Wait"的构建挂起

- **现象**：`tools/Validate.ps1` 构建完成、exe 已生成，但脚本卡在 Build 步骤不再前进。
- **原因**：MSBuild 默认保留后台 worker 节点（node reuse）。父进程用
  `Start-Process -Wait` + 重定向输出等待时，会一直等到被复用的节点退出。
- **修法**：`Validate.ps1` 里设置 `$env:MSBUILDDISABLENODEREUSE = "1"`，
  让每次构建都是自包含进程。
- **判据**：脚本正常走到 `[validate] Test: jobtest`。