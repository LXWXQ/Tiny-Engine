#pragma once

#include "IRenderPass.h"
#include "RHI/Device.h"
#include "RHI/RenderGraph.h"
#include <array>
#include <memory>

// TD-05 子资源语义校验的 GPU/回归探针。
//
// Positive 模式：只对 (mip0, layer0) 和 (mip1, layer1) 做写后读，
// 用来确认 barrier 只覆盖声明的子资源、正常路径验证层仍为 0 字节。
// SemanticError 模式：写 mip0/layer0，却读 mip1/layer0，RenderGraph 必须
// 在提交前给出带 mip/layer 的语义错误。
class SubresourceProbePass : public IRenderPass
{
public:
    enum class Mode
    {
        Disabled,
        Positive,
        SemanticError
    };

    SubresourceProbePass(Device& device, RenderGraph& renderGraph);

    void init() override;
    void addToGraph(RenderGraph& graph, FrameInfo& frameInfo) override;
    std::string getName() const override { return "SubresourceProbe"; }

    void setMode(Mode mode) { currentMode = mode; }
    Mode getMode() const { return currentMode; }

private:
    Device& device;
    RenderGraph& renderGraph;
    std::array<RenderGraph::TextureHandle, kFramesInFlight> testImages{};
    std::array<RenderGraph::BufferHandle, kFramesInFlight> testBuffers{};
    Mode currentMode = Mode::Disabled;
    bool reportedPositive = false;
};