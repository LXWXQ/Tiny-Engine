#pragma once

#include "RHI/RenderGraph.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <array>
#include "Core/FrameInFlight.h"

// D-2：光照通道的输出格式（线性 HDR 中间目标）。
// 用 16F 而不是 11-11-10 或 R8：Bloom 要在 tonal 映射之前看到 >1 的高光，
// 而 10 位整数格式虽然也能表示 >1（在 UNORM 下不能，在 FLOAT 下能），
// 但 16F 的精度足够做多级降采样而不会在粗级积累出明显的阶梯。
inline constexpr VkFormat kHdrSceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// 帧内渲染资源的集中登记处。
//
// 各 Pass 通过它交换产物：几何通道写 G-Buffer、SSAO 采样 G-Buffer、
// 光照通道写 HDR、Bloom 读写各级辉光、Present 读 HDR + Bloom 写交换链。
// 渲染图正是依靠这些「谁读了谁写了」的声明，才能自动推导出布局转换和屏障 ——
// 所以这个结构同时也是同步关系的唯一事实来源。
struct RenderResources
{
    // ---- D-4：以下瞬时资源全部按 frame-in-flight 槽位各自一份 ----
    // 两帧同时在飞时，第 N+1 帧录制/提交不能读写第 N 帧仍在使用的 G-Buffer、
    // SSAO、HDR 或 Bloom 中间图。共享它们会造成跨帧数据竞争，验证层不一定能报。
    std::array<RenderGraph::TextureHandle, kFramesInFlight> gbPosition;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> gbNormal;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> gbAlbedo;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> gbPbr;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> gbDepth;

    std::array<RenderGraph::TextureHandle, kFramesInFlight> ssaoRaw;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> ssao;

    // 光照终点：每帧一份线性 HDR 中间目标。
    std::array<RenderGraph::TextureHandle, kFramesInFlight> hdrScene;

    // TAA：每帧解析后的抗锯齿 HDR 输出。Bloom 与 Present 固定读它，
    // 因此即使 taaEnabled=false，TAA Pass 也要照常运行并写这张图。
    std::array<RenderGraph::TextureHandle, kFramesInFlight> taaOutput;

    // TAA 历史：两张常驻图 ping-pong。alpha 通道存世界半径，供重投影时
    // 判断历史像素是否来自同一块几何（静态场景里相机平移不会改变它）。
    std::array<RenderGraph::TextureHandle, 2> taaHistory;

    // 每帧一棵 Bloom 金字塔；每棵 4 级。
    std::array<std::vector<RenderGraph::TextureHandle>, kFramesInFlight> bloomLevels;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> bloomResult;

    // PresentPass 用的采样器不属于渲染图（渲染图只管图像），由使用方持有
    VkSampler hdrSampler = VK_NULL_HANDLE;

    // ---- 跨帧常驻（Application 里导入一次，之后只读）----
    RenderGraph::TextureHandle environment;   // 等距柱状 HDR（天空盒）
    RenderGraph::TextureHandle irradiance;    // IBL 漫反射
    RenderGraph::TextureHandle prefiltered;   // IBL 镜面
    RenderGraph::TextureHandle brdfLut;       // IBL BRDF 积分表

    // ---- 交换链（每帧按 imageIndex 选一个）----
    std::vector<RenderGraph::TextureHandle> swapchainImages;

    // ---- 编辑器离屏视口（TD-09）----
    //
    // 编辑器模式下 PresentPass 不再直接写交换链，而是把 tonemap 之后的 LDR 结果
    // 写进这张图；随后由 UiCompositePass 把这张图当作 ImGui 的一张图片画到交换链上，
    // 面板就真正"围绕"视口而不是"浮在"3D 画面上面。
    //
    // 为什么按 frame slot 各一份：和 hdrScene 同理 —— 两帧在飞时，
    // 第 N+1 帧写这张图会覆盖 GPU 仍在采样的第 N 帧内容。
    //
    // 非编辑器模式下这些 handle 保持 invalid()，PresentPass 走原来的直写交换链路径。
    std::array<RenderGraph::TextureHandle, kFramesInFlight> editorViewport;
    VkExtent2D editorViewportExtent{ 0, 0 };
    bool editorViewportEnabled = false;

    // 编辑器叠加层（包围盒线框）建管线时要用的附件格式。
    // 由 Application 在创建离屏视口时填好；未启用编辑器时保持 UNDEFINED，
    // EditorOverlayPass 看到 UNDEFINED 就不建管线（零开销）。
    VkFormat editorViewportFormatHint = VK_FORMAT_UNDEFINED;
    VkFormat depthFormatHint = VK_FORMAT_UNDEFINED;
};
