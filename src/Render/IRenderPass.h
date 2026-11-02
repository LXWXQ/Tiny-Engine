#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <string>
#include "Core/FrameInfo.h"

class RenderGraph;

// 渲染通道抽象。
//
// 从 D-1(b) 起，Pass 不再自己持有 VkRenderPass / VkFramebuffer，
// 也不再直接录制命令 —— 而是"把自己的工作声明进渲染图"：
//
//   void GeometryPass::addToGraph(RenderGraph& graph, FrameInfo& frameInfo)
//   {
//       auto& p = graph.addGraphicsPass("Geometry");
//       p.writeColor(m_position, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
//       p.writeDepth(m_depth, VK_ATTACHMENT_LOAD_OP_CLEAR);
//       p.execute([this, &frameInfo](VkCommandBuffer cmd) { ... });
//   }
//
// 屏障、布局转换、vkCmdBeginRendering / EndRendering、viewport/scissor
// 全部由渲染图负责。Pass 里不会再出现一行同步代码。
class IRenderPass
{
public:
    virtual ~IRenderPass() = default;

    // 初始化资源：从渲染图 create/import 纹理、创建管线
    virtual void init() = 0;

    // 把本 Pass 的渲染工作注册进渲染图
    virtual void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) = 0;

    // TD-03: resize extent-dependent render targets in place. Pipelines use
    // dynamic viewport/scissor, so most passes do not need pipeline rebuilds.
    virtual void resize(VkExtent2D /*extent*/) {}

    virtual std::string getName() const = 0;
};