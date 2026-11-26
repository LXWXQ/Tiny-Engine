#pragma once

#include "RHI/RenderGraph.h"

#include <glm/glm.hpp>

#include <cstdint>

// 时域上采样 / 时域抗锯齿的统一接缝（P5-5）。
//
// 这一层只定义契约，不实现神经网络或空间放大：当前的实现是 TAA（`TAAPass`），
// 未来的 FSR2 / DLSS 式实现只要满足同一组契约就能替换它，而 `Application` 里
// 只认 `ITemporalUpscaler*`，不需要知道底下是哪一种。
//
// 契约有三条：
//   1. 抖动由实现给出（`prepareFrame` 返回像素偏移），调用方把它加到投影矩阵上；
//      实现必须自己保存上一帧的抖动，因为历史 UV 需要减掉它。
//   2. 输出是一张 `RenderGraph` 管理的纹理句柄（`getOutput`），Bloom/Present 直接读；
//      即使实现被禁用，也要照常写，避免下游读到没人写的图。
//   3. 输入由 `getInputs()` 声明（颜色/世界位置/历史/速度/深度），调用方只在真正
//      被声明的输入上建立图依赖 —— 现在的 TAA 要颜色+世界位置+历史+速度。
struct TemporalUpscalerInputs
{
    bool color = true;
    bool worldPosition = false;
    bool history = false;
    bool velocity = false;
    bool depth = false;
};

class ITemporalUpscaler
{
public:
    virtual ~ITemporalUpscaler() = default;

    // 日志/调试面板用的短名（"taa"）。
    virtual const char* upscalerName() const = 0;
    // 是否消费历史帧：TAA 为真，纯空间上采样为假。
    virtual bool isTemporal() const = 0;
    // 本实现需要哪些输入。
    virtual TemporalUpscalerInputs getInputs() const = 0;

    // 写 UBO 之前调用。返回本帧的亚像素抖动（像素单位）。
    // 实现要把上一次的抖动翻到「上一帧」，addToGraph 才能拿到正确历史偏移。
    virtual glm::vec2 prepareFrame(uint64_t frameNumber, VkExtent2D renderExtent,
                                   bool enabled, float jitterScale) = 0;

    // 下游（Bloom/Present）要读的输出图。
    virtual RenderGraph::TextureHandle getOutput(uint32_t frameInFlightIndex) const = 0;

    // 分辨率变化。
    virtual void resize(VkExtent2D extent) = 0;
};