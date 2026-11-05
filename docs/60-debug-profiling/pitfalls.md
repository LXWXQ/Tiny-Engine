---
id: debug-profiling/pitfalls
title: 已踩过的坑
type: debug
scope: 空（跨代码库）
read-when: 改任何代码之前；或遇到"看起来不可能"的怪异现象时
size: ~430 行
updated: 2026-11-05
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
## 23. 等距柱状天空盒的两类经典缺陷（接缝亮线 + 上下颠倒）

天空盒用 `sampler2D` 直接采样等距柱状 HDR 时，有两个**独立**的坑，且都不报验证层错误。

### 23.1 atan 接缝处的一条亮虚线

- **现象**：天空中出现一条**竖直的亮虚线**（断续的白色短线）。
- **原因**：经度用 `atan(v.z, v.x)` 求，在 ±PI 处从 +PI 跳回 -PI，`envUV.x` 在接缝
  两侧相差约 1.0。硬件用 `dFdx/dFdy` 自动选 mip，接缝那一列的导数 ≈ 1.0（整整绕一圈），
  被判定为"极度缩小"，直接退到最低 mip（2048x1024 退到 1x1），于是出现一条亮线。
- **判据（决定性）**：把 `textureQueryLod(environmentMap, envUV).x` 当颜色输出，
  整片天空 LOD ≈ 0（黑），**只有那条线是 LOD = 8（纯白）**。
- **修法**：不用隐式导数，手工算完把跨接缝的那一份折回 `[-0.5, 0.5]`，再交给 `textureGrad`：

```glsl
vec2 dx = dFdx(envUV);
vec2 dy = dFdy(envUV);
dx.x -= round(dx.x);      // 把「绕一圈」折回「几乎不动」
dy.x -= round(dy.x);
vec3 envColor = textureGrad(environmentMap, envUV, dx, dy).rgb;
```

纹理本身用 `REPEAT` 寻址，所以采样值天然连续，线就消失了。
**注意**：`addressModeU/V = REPEAT` 是前提；改成 `CLAMP_TO_EDGE` 会重新出现接缝。


### 23.3 体素图集：fract() 格边界让整张图集变成一条白边

- **现象**：体素地形每个方块边缘出现白色细线，越远越密连成白网格；相机移动时闪烁，
  远处更明显，Bloom 把白线扩散成绿色散光。普通程序化地形只有轻微锯齿。
- **原因**：顶点 `uv.x` 用 `tile * 64 + 面内跨度` 编码图块，片元用 `fract()` 取格内 UV。
  `fract()` 在方块边界从 ≈1 跳回 0，`dFdx/dFdy` 出现整格大小的尖峰，硬件把 mip 直接
  选到最粗一级（256×256 图集 → 1×1）。而图集 16 格里只有 9 格有内容，其余 7 格是纯白，
  最粗 mip 平均后就是白色，于是每个边界都出现一圈白线。距离越远 mip 越高、边界像素
  占比越大，所以“越远越强”。
- **判据（决定性）**：把 `textureQueryLod(albedoMaps[matId], albedoUV).x` 当颜色输出，
  格内 LOD 正常，只有方块边界那一列/行跳到最大级；或直接看 256×256 图集预览，
  确认未使用格子是白色。
- **修法**：不用隐式导数，改用显式梯度 `textureGrad`：
  ```glsl
  vec2 dx = dFdx(fragUV) * 0.25;  // 体素：图集 UV = 格内 UV * 0.25
  vec2 dy = dFdy(fragUV) * 0.25;
  albedoUV = (tileOrigin + clamp(local, 0.5/64.0, 1.0 - 0.5/64.0)) * 0.25;
  vec4 c = textureGrad(albedoMaps[matId], albedoUV, dx, dy);
  ```
  同时把未使用格子填成非白（这里填草绿），即使高 mip 跨格混合也不会再出现白边。
  `clamp` 用半纹素内缩而不是 2%，否则每格边缘会留下一条常量色带。
- **注意**：这不是 TAA 问题。修补前管线没有 TAA/FXAA/MSAA，但纯时域锯齿不会只沿
  每个方块边界出现；先修纹理 LOD，再考虑后处理 AA（TAA 已于 2026-11-05 接入，
  是本条之外的独立项）。
- **后续（2026-11-05）**：本条修完仍有规则白点，根因是全局 mip 跨格渗色；最终图集扩为
  512×512、每格 64px 内容 + 32px edge-clamp gutter，`mrt.frag` 与 `glass.frag` 同步映射。
  详见 23.4。

### 23.2 天空上下颠倒 / 与 IBL 光照方向相反

- **现象**：天空又灰又平，云都挤在画面下方，顶部一片雾；但 IBL 照亮的结果却是对的。
- **原因**：等距柱状贴图的**图像顶部是天顶**，所以 `+Y` 方向必须映射到 `uv.y = 0`。
  错误写法 `asin(y)/PI + 0.5` 把"向上"映射到了 `uv.y = 1`（图像底行 = 雾/地面）。
  而 `ibl_equirect_to_cube.frag` 一直用的是 `0.5 - asin(y)/PI`。两者相反，于是
  **"看到的天空"和"照亮场景的环境光"来自相反方向**。
- **判据**：先量出贴图的亮度分布（本例顶部 25% 均值 194、底部 25% 均值 143 且方差极小），
  确认顶部是天顶；再把 `uv.y` 分四段染成红/绿/蓝/白渲染出来，看地平线是否落在 `v = 0.5`。
- **修法**：天空盒与 IBL 使用**同一个** `sampleSphericalMap`：

```glsl
vec2 sampleSphericalMap(vec3 v) {
    return vec2(atan(v.z, v.x) * (1.0/(2.0*PI)) + 0.5,
                0.5 - asin(clamp(v.y, -1.0, 1.0)) * (1.0/PI));
}
```

**教训**：同一个数学约定出现在两个 shader 里时，必须有一处是"权威实现"并被交叉引用。
本项目的权威是 `ibl_equirect_to_cube.frag`（它的注释里写明了图像顶部是天顶）。

### 23.4 体素图集：显式梯度 + 半纹素内缩后仍有规则白点（mip 跨格）

- **现象**：2026-11-04 的白线修完后，远处草地/石面仍有一层规则的浅色点阵，越远越密；
  旋转视角时点阵闪烁，TAA 只能压时域闪烁，静态摩尔纹还在。放大到 4x 后，点是去饱和的
  浅色（约 `(187,201,153)`，草地中位色 `(114,177,89)`），沿每个方块面边缘分布。
- **已排除**：逐 texel 噪声、顶点色/AO、天空洞、方向光、Bloom/SSAO。决定性判据是把
  `lighting.frag` 的输出临时改成 raw albedo：白点仍在；把输出改成常量 0.5：白点消失。
  再把 `mrt.frag` 的体素分支改成只输出 `atlasUV`，确认白点确实来自图集采样。
- **原因**：`textureGrad` 的显式梯度只解决 `fract()` 的导数尖峰，**不能阻止高 mip 的采样点
  落到图集格边界**。`0.5/64` 半纹素内缩在 mip 0 是纹素中心，但 mip N 下只离格边 0.25 个
  mip texel，线性过滤仍会采到邻格（草/土），高 mip 平均成浅色。逐格 mip 单独也不够：
  UV 不内缩到当前 mip 的纹素中心，仍会越界。
- **修法（当前）**：图集从 256×256 扩为 512×512，每格 64px 内容 + 32px **edge-clamp gutter**
  （128px 间距）；gutter 复制最近内容边缘像素，不填纯基色。`mrt.frag` 与 `glass.frag`
  使用同一组映射：
  ```glsl
  const float kAtlasSize = 512.0, kPitch = 128.0, kPad = 32.0, kTile = 64.0;
  vec2 cellOrigin = vec2(mod(tileId, 4.0), floor(tileId / 4.0)) * kPitch + kPad;
  albedoUV = (cellOrigin + clamp(local, 0.5/kTile, 1.0 - 0.5/kTile) * kTile) / kAtlasSize;
  albedoGradX *= kTile / kAtlasSize;   // 0.125
  ```
- **不要只改 `mrt.frag`**：`glass.frag` 也按同一套 UV 编码解码图集；漏改时玻璃会采样到
  图集其他区域，表现为整块颜色错误/发白，验证层不会报错。
- **判据**：同机位对比 baseline（256 无 gutter）和修复版，baseline 的规则白点网格消失，
  近景纹理与玻璃仍正常；启动日志应出现 `[Voxel] atlas=512 tile=64 pitch=128 gutter=32`。

## 24. `try_get` 返回的是指针，闭包捕获它会在实体销毁后悬空

- **现象**：撤销"删除实体"之后实体没回来；或回来但数值是垃圾。
  没有任何验证层输出，程序也不一定崩。
- **代码**（错的）：
  ```cpp
  auto transform = registry.try_get<TransformComponent>(e);   // 指针，不是值
  rebuild = [transform](entt::entity created) {
      registry.emplace<TransformComponent>(created, *transform);  // UB
  };
  ```
- **原因**：`entt::registry::try_get<T>()` 返回 `T*`，指向组件存储。
  构造命令时实体还活着，指针有效；`apply()` 一 `destroy()`，指针就悬空。
  `revert()` 在其后才解引用 —— 读到的是已释放的内存。
- **修法**：做**真正的值拷贝**，让快照与实体生命周期解耦：
  ```cpp
  std::optional<TransformComponent> transform;
  if (const auto* p = registry.try_get<TransformComponent>(e)) transform = *p;
  ```
- **为什么这条值得单独记**：这类 bug 在 Debug 下经常"看起来正常"
  （内存还没被复用），只在特定顺序下出错，而且错误信息完全不指向真因。
  本项目是**自检**（断言"撤销后两个实体都回来了、父子关系也恢复了"）抓到的，
  不是人工发现的。
- **副产品**：`CameraComponent` 是空 tag 组件，查存在性要用
  `registry.any_of<T>(e)`；对空类型 `try_get` 取指针不成立。

## 25. ImGui 描述符池只有 1 个 set：注册第二张纹理就 0xC0000005

- **现象**：`ImGui_ImplVulkan_AddTexture()` 之后程序直接崩溃，退出码 `0xC0000005`，
  stderr 全空，验证层一声不吭，日志停在崩溃前最后一句正常输出。
- **原因**：`ImGuiSystem` 初始化时描述符池是
  `maxSets = 1` / `COMBINED_IMAGE_SAMPLER = 1` —— 只够 ImGui 后端自己用。
  `AddTexture` 从**同一个池**里分配描述符集，第二个请求直接失败，
  返回未初始化的句柄；后续绘制解引用它。
- **为什么难查**：`vkAllocateDescriptorSets` 的返回值没被检查（ImGui 后端内部调用），
  失败是静默的。验证层不报"你用了无效描述符集"，因为句柄只是个整数。
- **修法**：池容量按"用户纹理"需求给足。现在是 64：
  ```cpp
  constexpr uint32_t kImGuiTextureCapacity = 64;
  ```
- **判据**：只要崩溃点正好在第一次 `AddTexture` 之后、且没有任何 Vulkan 报错，
  先怀疑描述符池/堆容量，而不是纹理本身。
- **相关**：编辑器离屏视口每帧槽注册一张，见 [editor/viewport](../90-editor/viewport.md)。

## 26. Vulkan 投影矩阵 Y 轴翻转：`projection[1][1]` 是负数

- **现象**：编辑器三轴手柄的三个箭头全部**叠在中心一个点上**，看不到轴线。
  没有任何运行时错误，验证层也不说话。
- **原因**：Vulkan 的 NDC 是 Y 向下（与 OpenGL 相反），所以透视投影矩阵的
  `m[1][1]` 是**负值**。用它反推视锥时：
  ```cpp
  const float tanHalfFov = 1.0f / projection[1][1];   // 得到 -0.577
  const float worldPerPixel = 2.0f * tanHalfFov * distance / viewportHeight;  // 负数
  const float handleLength = std::max(kHandlePixels * worldPerPixel, 1e-4f);  // 被钳成 1e-4
  ```
  手柄长度退化成万分之一世界单位，三个轴缩到同一个像素上。
- **修法**：取绝对值。
  ```cpp
  const float tanHalfFov = std::abs(1.0f / projection[1][1]);
  ```
- **为什么值得记**：这类"数值悄悄变成负的、又被 `std::max` 兜住"的 bug
  不会崩溃、不会报错，只会让画面"看起来不对"。唯一的发现途径是**看截图**。
  定位手段是把手柄长度、相机距离、`tanHalfFov` 打出来 —— 一打就看见负号。
- **通用教训**：凡是"从投影矩阵反推视锥参数"的地方，都要检查符号约定；
  同时不要用 `std::max` 掩盖异常值，必要时加断言。

## 27. 实体级 AABB 拾取在"相机位于物体内部"时完全失效

- **现象**：视口里点任何东西都选中同一个实体（本项目是 Sponza），
  连点在它前面的小球也一样。
- **原因**：Sponza 整栋是**一个实体**，它的世界 AABB 把相机包在里面。
  射线与 AABB 的入射参数 `tMin` 从 0 开始（起点就在盒内），
  于是它的命中距离恒为 0，永远排第一。
- **这不是阈值问题**：调小拾取半径、加距离上限都没用 ——
  AABB 表达不了"这堵墙在 5 米外、那个球在 3 米外"。
- **修法**：AABB 只做粗筛，真正判定用三角形求交（Möller-Trumbore）。
  ```cpp
  // 1. AABB 粗筛：连包围盒都没碰到就不可能命中
  if (!rayAabb(origin, dir, bounds.world, aabbT)) continue;
  if (aabbT > bestDistance) continue;          // 比当前最近还远，跳过
  // 2. 精确求交
  if (!mesh.model->intersectRay(worldTransform->matrix, origin, dir, distance)) continue;
  ```
- **性能取舍**：把**射线**变换到模型局部空间（一次逆矩阵），
  而不是把几十万顶点变换到世界空间。CPU 侧只保留位置 + 索引
  （Sponza 约 10MB），点选是低频操作，暴力遍历 27 万三角形约几毫秒，可接受。
- **通用教训**：任何"用包围盒做精确判定"的地方，都要问一句
  "如果相机在盒子里面会怎样"。拾取、剔除、碰撞都有这个陷阱。

## 28. 屏幕↔NDC 的 Y 方向：投影矩阵已经翻过一次了

- **现象**：编辑器里拖动物体时，**物体和白包围盒朝相反方向移动**；
  更早的表现是"点球却选中了别的东西"。
- **原因**：`VulkanCamera::setPerspectiveProjection` 里有
  ```cpp
  projectionMatrix[1][1] *= -1;   // Vulkan 的 NDC Y 向下
  ```
  所以 NDC 的 +Y 指向**屏幕下方**。而屏幕坐标 → 射线的换算如果还按
  "NDC +Y 向上"写，就多做了一次翻转：
  ```cpp
  // 错的
  ndcY = 1.0f - ((screen.y - rectMin.y) / height) * 2.0f;
  outScreen.y = rectMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * height;
  ```
- **为什么难查**：
  1. 不崩溃、不报错，验证层完全不参与（纯 CPU 数学）；
  2. **投影和反投影一起错时，往返测试仍然通过** —— 两次翻转互相抵消。
     我原来的 `Gizmo::selfTest()` 就是这么"通过"的。
  3. 物体在画面**正中心**时框也是对的（镜像不改变中心点），
     所以只有把物体移开中心才能看出来。
- **修法**：两处都必须直接用 `ndc*0.5+0.5`，不要再翻：
  ```cpp
  ndcY = ((screen.y - rectMin.y) / height) * 2.0f - 1.0f;
  outScreen.y = rectMin.y + (ndc.y * 0.5f + 0.5f) * height;
  ```
- **正确的自检**：不要只做往返，要断言**方向不变量**：
  ```cpp
  // 世界 +Y 更高的点必须投影到更小的屏幕 y（更靠上）
  if (!(higher.y < lower.y)) fail;
  // 世界 +X 更大的点必须投影到更大的屏幕 x
  if (!(right.x > left.x)) fail;
  ```
  并且自检用的投影矩阵必须**和引擎一致**（也做 `[1][1] *= -1`），
  否则测的是一个真实渲染里不存在的约定。
- **旁证**：修之前截图中手柄的**绿色 Y 轴指向屏幕下方** ——
  这是最直观的破绽，只是当时没注意。

## 29. 负的 albedo 经过 ACES 会变成"白色"

- **现象**：程序化棋盘格整片是白格子，只有格子缝偏暗；纯色块、贴图都正常。
  没有验证层报错，也没有任何崩溃。
- **原因**：棋盘格的抗锯齿滤波在"窗口小于格子"时会把 pattern 外插到
  `[-1.5, 2.5]` 而不是 `[0, 1]`。`mix(vec3(0.03), vec3(0.85), -1.5)`
  得到负的 albedo，写进 `R16G16B16A16_SFLOAT` 的 G-Buffer。
  `PresentPass` 的 ACES 形如
  `x*(a*x+b) / (x*(c*x+d)+e)`：负的 x 会让分子分母同时为负，结果变成正值，
  最后 `clamp(0,1)` 成白色。
- **判据**：把 pattern 直接当颜色输出（不经过光照/ACES），能看到黑白格；
  把 albedo 的符号打印出来，黑格是负值。也可以用 `--no-bloom` 排除 Bloom 干扰。
- **修法**：滤波结果必须 `clamp(pattern, 0.0, 1.0)`；
  颜色也不要外插到 [0,1] 之外。
- **通用教训**：任何写进 HDR G-Buffer 的"程序化颜色"都要确认值域。
  验证层只管同步和用法，不管数值；这类 bug 只能靠看截图或输出中间量发现。

## 30. 直接给欧拉分量加拖拽 delta：旋转方向反 + 万向节死锁

- **现象**：拖旋转手柄时，物体会绕一个"混合轴"转；某些角度下拖 X 环，物体
  却朝反方向或斜着转。pitch 接近 ±90° 后继续拖，物体会突然翻转/失去响应。
- **原因**：`TransformComponent` 的矩阵顺序是 `T * Ry * Rx * Rz * S`。
  `rotation.x += delta` 并不等于"绕当前局部 X 轴转 `delta`"——它改的是
  **欧拉分解里的一个分量**，只有其它两个角为零时才等价。到 pitch=±90° 时，
  Y/Z 两个欧拉角描述同一个自由度，这就是万向节死锁。
- **修法**：旋转的真源改成 `glm::quat orientation`，`localMatrix()` 用
  `mat4_cast(orientation)`；拖拽时
  `orientation = normalize(startOrientation * angleAxis(delta, localAxis))`。
  欧拉角只用于 Inspector/JSON 显示，通过 `setEuler()` / `eulerFromQuat()`
  同步。方向和 `buildPlaneBasis` 的右手系一致。
- **回归**：`Scene::selfTest` 断言四元数矩阵与旧 `T*Ry*Rx*Rz*S` 等价、
  pitch=90° 时矩阵有限、单轴旋转反解不会串到其它分量；Gizmo 截图确认
  OBB 与手柄跟着转。
- **通用教训**：欧拉角适合"人读"，不适合"连续复合"。任何会被反复拖拽的
  旋转都必须存在四元数/矩阵里；只在 UI 边界做欧拉转换。

## 31. 把 UV 图集子区间当成 [0,1]：棋盘格变成两三个大块

- **现象**：`Quad-Sphere.obj` 的棋盘格只有两三个巨大格子，且球面有接缝和
  拉伸；`plane.obj`、`debug_cube.obj` 正常。
- **原因**：Quad-Sphere 的 UV 只占 `0.048~0.427`（图集子区间），
  着色器却按 `uv * 8` 采样，实际只覆盖约 3 格；而且它的 UV 在立方体六个面
  之间不连续，任何 UV 平铺都会在面边界出现接缝。三平面投影在球面 45° 处
  又会把黑白混成灰带。
- **修法**：CPU 用 `Model::computeUvBounds()` 记录 UV 范围；只有两个方向都
  接近 `[0,1]` 时才走 UV 棋盘（8 格、减去 uvMin、跨接缝导数折回），
  否则走局部空间 3D 棋盘（连续场，无接缝）。
- **判据**：打印/暂停看 `uvMin/uvMax`；如果范围远小于 1，不要直接乘 tile 数。
  截图里"格子数明显少于预期 + 面边界接缝"就是这个问题。
- **通用教训**：UV 是美术资产的一部分，不能假设所有模型的 UV 都铺满 [0,1]。
  程序化纹理要么归一化，要么用不依赖 UV 的 3D 域。
## 32. 大面积平滑地形上的 SSAO 低频条纹

- **现象**：地形近看是规则的波浪/条纹，像低模法线或纹理接缝；关掉 SSAO 后
  条纹完全消失。改成高分辨率网格只能让条纹变细，不能消除。
- **原因**：SSAO 的 16-tap 半球核在平缓大表面上会产生低频采样干涉；
  相机贴近地面、像素覆盖区域很大时尤其明显。它不是几何法线错误，
  也不是材质 UV 问题。
- **判据**：先跑 `--no-ssao` 对照；如果条纹消失，问题在 SSAO 而不是
  高度场/法线。再检查是否只有地形这种大面积平滑表面出现。
- **修法**：把 G-Buffer Normal 的 `.w` 扩展为 SSAO policy mask：
  普通几何写 `1.0`，地形写 `0.0`；`ssao.frag` 读到 `w < 0.5` 时直接输出
  `vec4(1.0)`。地形仍参与 G-Buffer 法线/深度和光照，只是不参与屏幕空间
  遮蔽。局部接触阴影应由后续地形专用 AO/材质方案提供。
- **回归**：`--empty-scene --terrain-test` 截图同时开/关 SSAO，
  地形表面应不再因 SSAO 产生条纹；普通网格的 SSAO 仍然生效。

## 33. 地形纹理单尺度平铺形成规则网格

- **现象**：替换成真实 albedo/normal 纹理后，远处草地出现规则的点阵/菱形
  重复；纹理分辨率越高，网格只是变细，仍然一眼能看出 tile。
- **原因**：单张 tile 只用 `worldXZ * scale` 一次采样，mip 平均后仍保留周期性；
  同时 normal 强度没有随远处 footprint 足够衰减，光照把重复格点进一步放大。
- **修法**：
  1. 每层 albedo 用两个不同 scale 采样，用低频 patch mask 混合；
  2. 四层各自使用不同世界尺度和相位，并乘 `uvTiling`；
  3. 低频 fBm 做宏观亮度变化，打破均匀色块；
  4. normal 仅做一层采样，按 `fwidth(worldXZ)` 淡出，远处不强调 tile 边界；
  5. 纹理生成端降低草地高频对比，normal strength 按层限制。
- **判据**：近距离应看到细颗粒；中远距离应只剩低频色块，不应有可数的 tile
  网格。若网格仍明显，先检查是否只采样了一次 albedo，再检查 normal 的
  footprint 衰减。

## 34. 流送地形按 chunk 局部高度归一化导致材质断层

- **现象**：流送地形上出现大片米色/白色斑块和笔直的矩形边界，边界沿 chunk
  网格走；关掉 TAA/Bloom/SSAO/阴影后仍存在，看起来像“纹理不无缝”。
- **原因**：片元着色器用 `(fragPosLocal.y - terrainMinY) / terrainHeightRange`
  还原 0..1 高度，再用它决定沙/草/岩/雪权重。若 `terrainMinY/heightRange`
  来自每个 chunk 自己的 `BoundsComponent`，同一世界高度在相邻 chunk 会得到
  不同 `h`，层权重在边界跳变。纹理本身可以完全无缝，问题在材质参数。
- **修法**：所有 chunk 统一使用 `TerrainConfig::baseHeight` 与
  `std::max(TerrainConfig::heightScale, 1e-4f)`；合批实例表和逐 chunk push
  必须共用同一组值。日志同时输出 `heightMin/heightRange/heightMixed`，
  `heightMixed=0` 才说明没有混入 per-chunk 范围。
- **判据**：同一机位下合批与 `--no-terrain-batch` 都不应出现沿 chunk 边界的
  色块；若只有非合批路径出问题，检查 `GeometrySystem` 的 push 是否漏改。

## 35. 角色动画接入：四个看起来无关的坑

- **角色全黑 / 灰白棋盘格**：glTF 材质没有 `baseColorTexture` 时，`materialAlbedoFallback` 必须是 0，否则渲染走"无贴图 fallback"的棋盘格。角色是纯 `baseColorFactor` 材质，最容易踩。
- **动画一个都不切**：Blender/Quaternius 导出的 clip 名常带 `CharacterArmature|` 前缀。导入时只保留最后一个 `|` 之后的部分；否则状态机按 `Idle` 查不到，永远停在绑定姿态或当前 clip。
- **角色不显示 / 位置乱跳**：蒙皮模型 `isDynamic() == true`，不能进静态 GPU 合批，也不能被"合并 buffer"路径缓存。`GeometrySystem` / `SceneGpuBatch` 都要显式跳过它。
- **进入运行模式后角色坠出世界**：出生点不能固定在"相机前方 2m"，要沿视线 ray-march 找地面；相机 yaw/pitch 本来就是弧度，存进 `runtimeYaw/runtimePitch` 后不能再按"度→弧度"转一次，否则 WASD 方向整体反向、角度缩小约 57 倍。
- **判据**：`[Character] model attached | skinned=1 animations=18`、`[Runtime] spawn=... onGround=1`、按 W/Shift+W/Space 时 `[Runtime] clip=Walk/Run/Jump` 依次出现；没有这些日志时先查导入参数，再查状态机阈值和输入分支。
