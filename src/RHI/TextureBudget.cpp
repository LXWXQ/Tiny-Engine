#include "TextureBudget.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace rhi
{
namespace
{
// 降到这个尺寸还放不下就放弃继续降级：再小下去贴图已经没有可读性，
// 不如照常创建并在报告里体现超预算。
constexpr uint32_t kMinBudgetDimension = 64;

std::string formatBytes(uint64_t bytes)
{
    std::ostringstream out;
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    out.setf(std::ios::fixed);
    out.precision(mb < 10.0 ? 2 : 1);
    out << mb << "MB";
    return out.str();
}
} // namespace

TextureBudget& TextureBudget::instance()
{
    static TextureBudget budget;
    return budget;
}

void TextureBudget::registerTexture(uint64_t bytes)
{
    used_ += bytes;
    peak_ = std::max(peak_, used_);
    ++count_;
}

void TextureBudget::unregisterTexture(uint64_t bytes)
{
    used_ = (bytes >= used_) ? 0 : used_ - bytes;
    count_ = (count_ == 0) ? 0 : count_ - 1;
}

uint32_t TextureBudget::mipLevelsFor(uint32_t width, uint32_t height)
{
    const uint32_t longest = std::max(width, height);
    if (longest == 0)
        return 1;
    return static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(longest)))) + 1;
}

uint64_t TextureBudget::mipChainBytes(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                                      uint32_t mipLevels)
{
    uint64_t total = 0;
    for (uint32_t level = 0; level < mipLevels; ++level)
    {
        const uint32_t w = std::max(1u, width >> level);
        const uint32_t h = std::max(1u, height >> level);
        total += static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * bytesPerPixel;
    }
    return total;
}

TextureBudget::MipPlan TextureBudget::planTexture(uint32_t width, uint32_t height,
                                                  uint32_t bytesPerPixel,
                                                  uint32_t requestedMipLevels) const
{
    MipPlan plan;
    plan.width = width;
    plan.height = height;
    plan.mipLevels = requestedMipLevels;
    plan.bytes = mipChainBytes(width, height, bytesPerPixel, requestedMipLevels);

    if (limit_ == 0 || used_ + plan.bytes <= limit_)
        return plan;

    // 第一步：丢掉 mip 链。mip 链总和是 mip0 的 4/3，所以这一步最多省 25%。
    const uint64_t mip0Bytes = mipChainBytes(width, height, bytesPerPixel, 1);
    if (used_ + mip0Bytes <= limit_)
    {
        plan.mipLevels = 1;
        plan.bytes = mip0Bytes;
        return plan;
    }

    // 第二步：逐级减半（每级 4x），mip 链按新尺寸的完整链重新算。
    uint32_t w = width;
    uint32_t h = height;
    while (w > kMinBudgetDimension || h > kMinBudgetDimension)
    {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        const uint32_t levels = mipLevelsFor(w, h);
        const uint64_t bytes = mipChainBytes(w, h, bytesPerPixel, levels);
        if (used_ + bytes <= limit_)
        {
            plan.width = w;
            plan.height = h;
            plan.mipLevels = levels;
            plan.bytes = bytes;
            return plan;
        }
    }

    // 已经降到最小尺寸还超预算：照常创建，但标记 fits=false。
    plan.width = w;
    plan.height = h;
    plan.mipLevels = mipLevelsFor(w, h);
    plan.bytes = mipChainBytes(w, h, bytesPerPixel, plan.mipLevels);
    plan.fits = false;
    return plan;
}

std::string TextureBudget::report() const
{
    std::ostringstream out;
    out << "[Texture] budget used=" << formatBytes(used_)
        << " peak=" << formatBytes(peak_)
        << " limit=" << (limit_ == 0 ? std::string("unlimited") : formatBytes(limit_))
        << " textures=" << count_;
    return out.str();
}

bool textureBudgetSelfTest()
{
    auto expect = [](bool condition, const char* what)
    {
        if (!condition)
            std::cerr << "[Texture] budgetSelfTest FAIL: " << what << "\n";
        return condition;
    };

    bool ok = true;

    // 2048² RGBA8 的完整 mip 链：4/3 * 16MB ≈ 22.37MB。
    const uint64_t mip0 = TextureBudget::mipChainBytes(2048, 2048, 4, 1);
    const uint64_t full = TextureBudget::mipChainBytes(2048, 2048, 4, 12);
    ok &= expect(mip0 == 16ull * 1024 * 1024, "2048^2 RGBA8 mip0 must be 16MB");
    ok &= expect(full == 22369620ull, "2048^2 RGBA8 full mip chain must be 22369620 bytes");
    ok &= expect(TextureBudget::mipLevelsFor(2048, 2048) == 12, "2048^2 has 12 mip levels");
    ok &= expect(TextureBudget::mipLevelsFor(1024, 512) == 11, "1024x512 has 11 mip levels");

    // 丢 mip 链省 25%，减半省 75%：两个数字必须和迁移计划的口径一致。
    const double mipSaving = 1.0 - static_cast<double>(mip0) / static_cast<double>(full);
    ok &= expect(mipSaving > 0.249 && mipSaving < 0.251, "dropping the mip chain saves 25%");

    // 不限制：原样放行。
    {
        TextureBudget budget;
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.width == 2048 && plan.height == 2048 && plan.mipLevels == 12 &&
                         plan.bytes == full && plan.fits,
                     "unlimited budget must keep the full mip chain");
    }

    // 预算够：原样放行（32MB > 22.37MB）。
    {
        TextureBudget budget;
        budget.setLimitBytes(32ull * 1024 * 1024);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.mipLevels == 12 && plan.bytes == full, "budget above the chain must not downgrade");
    }

    // 20MB：整链放不下、mip0 放得下 → 只留 mip0。
    {
        TextureBudget budget;
        budget.setLimitBytes(20ull * 1024 * 1024);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.mipLevels == 1 && plan.bytes == mip0 && plan.fits,
                     "20MB budget must fall back to mip0 only");
    }

    // 8MB：mip0 也放不下 → 减半到 1024²，完整链 5.59MB。
    {
        TextureBudget budget;
        budget.setLimitBytes(8ull * 1024 * 1024);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        const uint64_t halfFull = TextureBudget::mipChainBytes(1024, 1024, 4, 11);
        ok &= expect(plan.width == 1024 && plan.height == 1024 && plan.mipLevels == 11 &&
                         plan.bytes == halfFull && plan.fits,
                     "8MB budget must halve the resolution");
        ok &= expect(static_cast<double>(plan.bytes) / static_cast<double>(full) < 0.26,
                     "halving must cut resident bytes by at least 74%");
    }

    // 1MB：继续减半到 256²。
    {
        TextureBudget budget;
        budget.setLimitBytes(1ull * 1024 * 1024);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.width == 256 && plan.height == 256 && plan.fits,
                     "1MB budget must keep halving down to 256^2");
    }

    // 已有用量也参与判断：8MB 已用 + 24MB 上限，2048² 整链(22.37MB)放不下，
    // 但 mip0(16MB) 正好装进剩余 16MB —— 边界必须走"只留 mip0"。
    {
        TextureBudget budget;
        budget.setLimitBytes(24ull * 1024 * 1024);
        budget.registerTexture(8ull * 1024 * 1024);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.mipLevels == 1 && plan.bytes == 16ull * 1024 * 1024 && plan.fits,
                     "existing usage must be taken into account (exact boundary keeps mip0)");
    }

    // 再紧 1MB：mip0 也放不下 → 减半到 1024²。
    {
        TextureBudget budget;
        budget.setLimitBytes(23ull * 1024 * 1024);
        budget.registerTexture(8ull * 1024 * 1024);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.width == 1024 && plan.mipLevels == 11 && plan.fits,
                     "existing usage must push the texture down to half resolution");
    }

    // 极小预算：降到 64 仍放不下 → fits=false，但不崩、不返回 0 尺寸。
    {
        TextureBudget budget;
        budget.setLimitBytes(4096);
        const auto plan = budget.planTexture(2048, 2048, 4, 12);
        ok &= expect(plan.width == 64 && plan.height == 64 && !plan.fits,
                     "tiny budget must clamp at 64 and report fits=false");
    }

    // 记账：注册/注销与峰值。
    {
        TextureBudget budget;
        budget.registerTexture(1000);
        budget.registerTexture(2000);
        ok &= expect(budget.usedBytes() == 3000 && budget.peakBytes() == 3000 &&
                         budget.textureCount() == 2,
                     "register must accumulate and track the peak");
        budget.unregisterTexture(1000);
        ok &= expect(budget.usedBytes() == 2000 && budget.peakBytes() == 3000 &&
                         budget.textureCount() == 1,
                     "unregister must lower usage but keep the peak");
        budget.unregisterTexture(5000);
        ok &= expect(budget.usedBytes() == 0 && budget.textureCount() == 0,
                     "unregister below zero must clamp");
        ok &= expect(budget.report().find("textures=0") != std::string::npos,
                     "report must expose the texture count");
    }

    if (ok)
        std::cout << "[Texture] budgetSelfTest PASS | cases=11 mip-chain=22369620 halving=75% guards=ok\n";
    return ok;
}

} // namespace rhi
