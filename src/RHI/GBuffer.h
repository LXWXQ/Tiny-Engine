#pragma once

#include "Device.h"
#include <vulkan/vulkan.h>
#include <stdexcept>
// G-Buffer 各附件的格式。
// 这些格式必须和 GeometryPass::init() 里 importTexture 时填的 desc.format、
// 以及各管线 config.colorAttachmentFormats 完全一致 —— 动态渲染下没有
// Framebuffer 帮忙兜底，格式对不上会直接被验证层拦下。
namespace GBufferFormat
{
    // Position 必须是 32F：这里存的是绝对世界坐标，离原点越远 half 的量化步长越大。
    // 例如 |x|≈144 时 16F 的 ULP 已经是 0.125，而 SSAO 的 bias 只有 0.047、
    // radius 只有 0.33 —— 同一块表面会被深度比较判成“互相遮挡”，角色身上出现
    // 大片黑色花瓣。世界位置不适合用 half 存，16F 只适合方向/颜色这类小范围数据。
    // G-Buffer 采样器是 NEAREST、附件也不混合，因此不需要 16F 的过滤/带宽优势；
    // 代价是每像素比 RGBA16F 多 8 字节。改回 16F 前先看
    // docs/60-debug-profiling/pitfalls.md 坑 38。
    constexpr VkFormat Position = VK_FORMAT_R32G32B32A32_SFLOAT;
    constexpr VkFormat Normal   = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Albedo 必须是浮点：天空盒把「线性 HDR 辐照度」直接写进这个附件
    //（见 skybox_mrt.frag），由 PresentPass 统一做色调映射。
    // 8bit UNORM/SRGB 会把 >1.0 的高光在写入时就截断，天空盒的太阳/亮斑
    // 就再也进不了 Bloom —— 而且截断后的 1.0 恰好又超过 Bloom 阈值，
    // 结果整块天空都在起辉光，变成一层假雾。
    constexpr VkFormat Albedo   = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat Pbr      = VK_FORMAT_R8G8B8A8_UNORM;

    static_assert(Position == VK_FORMAT_R32G32B32A32_SFLOAT,
                  "G-Buffer Position must stay 32F: 16F world positions break SSAO "
                  "far from the origin (see pitfalls #38).");
}

// 将原本的附件结构体移到这里
struct FrameBufferAttachment 
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format;
};

class GBuffer 
{
public:
    // 构造时需要 Device 和当前的屏幕分辨率(Extent)
    GBuffer(Device& device, VkExtent2D extent);
    
    // RAII 核心：自动销毁所有 GBuffer 附件
    ~GBuffer();

    // 禁用拷贝
    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    // TD-03: recreate attachments at a new extent. Caller must make the GPU idle.
    void resize(VkExtent2D extent);

    // Getters：供 GeometryPass 导入渲染图、以及全局描述符集使用
    const FrameBufferAttachment& getPosition() const { return position; }
    const FrameBufferAttachment& getNormal() const { return normal; }
    const FrameBufferAttachment& getAlbedo() const { return albedo; }
    const FrameBufferAttachment& getPbr() const { return pbr; }
    const FrameBufferAttachment& getDepth() const { return depth; }
    VkSampler getSampler() const { return sampler; }
    VkExtent2D getExtent() const { return currentExtent; }

private:
    void createAttachment(VkFormat format, VkImageUsageFlags usage, FrameBufferAttachment* attachment);
    void destroyAttachment(FrameBufferAttachment& attachment);

    Device& device;
    VkExtent2D currentExtent;

    FrameBufferAttachment position;
    FrameBufferAttachment normal;
    FrameBufferAttachment albedo;
    FrameBufferAttachment pbr;
    FrameBufferAttachment depth;
    VkSampler sampler = VK_NULL_HANDLE;
};