#pragma once

#include <cstdint>
#include <string>

namespace rhi
{

// ---------------------------------------------------------------------------
// M1-B5: 纹理显存预算
//
// 为什么需要它：Sponza 这类场景一次载入几十张 2048² 贴图，而且每张都带着完整
// mip 链常驻显存。以前显存吃紧只能整体换低分辨率资源，引擎里没有任何"降级"
// 手段，也没有地方能回答"这些贴图一共吃了多少显存"。
//
// 这里做两件事：
//   1) 把每张纹理的驻留字节记到一处，能报 used / peak / count；
//   2) 预算超限时按固定顺序降级：先丢 mip 链（省 25%），再逐级减半分辨率
//      （每级省 75%）。降级只发生在"这张纹理放不下"的时候，不超限时行为与
//      以前完全一致。
//
// 只做主线程记账：纹理都在主线程创建/销毁，加锁会把一次纹理上传变成锁竞争。
// ---------------------------------------------------------------------------
class TextureBudget
{
public:
    // 一张纹理最终的驻留方案。
    struct MipPlan
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
        uint64_t bytes = 0;
        bool fits = true;   // false 表示已经降到最小尺寸仍超预算（照常创建，但记一笔）
    };

    // 进程级实例（纹理加载点分散在 RHI/Scene，和 assets::mounts() 同样的理由）。
    static TextureBudget& instance();

    // 0 = 不限制（默认值，保持现有画面与显存占用不变）。
    void setLimitBytes(uint64_t bytes) { limit_ = bytes; }
    uint64_t limitBytes() const { return limit_; }

    uint64_t usedBytes() const { return used_; }
    uint64_t peakBytes() const { return peak_; }
    uint32_t textureCount() const { return count_; }

    void registerTexture(uint64_t bytes);
    void unregisterTexture(uint64_t bytes);

    // 按当前 used_ 与 limit_ 决定这张纹理怎么放。
    MipPlan planTexture(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                        uint32_t requestedMipLevels) const;

    static uint32_t mipLevelsFor(uint32_t width, uint32_t height);
    static uint64_t mipChainBytes(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                                  uint32_t mipLevels);

    // 一行状态："[Texture] budget used=... peak=... limit=... textures=N"。
    std::string report() const;

private:
    uint64_t limit_ = 0;
    uint64_t used_ = 0;
    uint64_t peak_ = 0;
    uint32_t count_ = 0;
};

// 降级决策的契约自检（不需要 Vulkan / 窗口 / 文件系统）。
bool textureBudgetSelfTest();

} // namespace rhi
