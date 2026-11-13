#include "Terrain/TerrainSculptDelta.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace terrain
{
namespace
{
bool expect(bool condition, const char* what)
{
    if (!condition)
        std::cerr << "[Terrain] sculptDeltaSelfTest FAIL: " << what << "\n";
    return condition;
}

HeightField makeField(uint32_t width, uint32_t height)
{
    HeightField field;
    field.width = width;
    field.height = height;
    field.sizeX = static_cast<float>(width - 1u);
    field.sizeZ = static_cast<float>(height - 1u);
    field.baseHeight = 0.0f;
    field.heightScale = 10.0f;
    field.heights.resize(static_cast<std::size_t>(width) * height);
    for (uint32_t z = 0; z < height; ++z)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            field.heights[static_cast<std::size_t>(z) * width + x] =
                static_cast<float>(x + z) / static_cast<float>(width + height);
        }
    }
    field.refreshMinMax();
    return field;
}
} // namespace

bool collectSculptDelta(const HeightField& before, const HeightField& after,
                        TerrainSculptDelta& out, std::string& errorMessage)
{
    out.indices.clear();
    out.heights.clear();
    errorMessage.clear();

    if (before.width != after.width || before.height != after.height)
    {
        errorMessage = "heightfield size mismatch";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(before.width) * before.height;
    if (before.heights.size() != expected || after.heights.size() != expected)
    {
        errorMessage = "heightfield height count mismatch";
        return false;
    }

    for (std::size_t i = 0; i < expected; ++i)
    {
        if (before.heights[i] == after.heights[i])
            continue;
        out.indices.push_back(static_cast<uint32_t>(i));
        out.heights.push_back(after.heights[i]);
    }
    return true;
}

bool applySculptDelta(HeightField& field, const TerrainSculptDelta& delta,
                      std::string& errorMessage)
{
    errorMessage.clear();

    if (delta.indices.size() != delta.heights.size())
    {
        errorMessage = "sculpt delta index/height count mismatch";
        return false;
    }

    const std::size_t cellCount = static_cast<std::size_t>(field.width) * field.height;
    if (cellCount == 0u || field.heights.size() != cellCount)
    {
        errorMessage = "heightfield is empty";
        return false;
    }

    for (std::size_t i = 0; i < delta.indices.size(); ++i)
    {
        const uint32_t index = delta.indices[i];
        if (index >= cellCount)
        {
            errorMessage = "sculpt delta index out of range";
            return false;
        }
        if (i > 0 && delta.indices[i - 1] >= index)
        {
            errorMessage = "sculpt delta indices must be strictly increasing";
            return false;
        }
        if (!std::isfinite(delta.heights[i]))
        {
            errorMessage = "sculpt delta height is not finite";
            return false;
        }
    }

    for (std::size_t i = 0; i < delta.indices.size(); ++i)
        field.heights[delta.indices[i]] = std::clamp(delta.heights[i], 0.0f, 1.0f);

    field.refreshMinMax();
    return true;
}

bool terrainSculptDeltaSelfTest()
{
    bool ok = true;

    // 用例 1：只有改过的格子进差分，且按升序排列。
    {
        const HeightField before = makeField(8u, 6u);
        HeightField after = before;
        after.heights[3] = 0.9f;
        after.heights[17] = 0.1f;
        after.heights[40] = 0.5f;

        TerrainSculptDelta delta;
        std::string error;
        ok &= expect(collectSculptDelta(before, after, delta, error), "collect failed");
        ok &= expect(delta.size() == 3u, "collect must keep only modified cells");
        ok &= expect(delta.indices.size() == 3u && delta.indices[0] == 3u &&
                         delta.indices[1] == 17u && delta.indices[2] == 40u,
                     "collect must produce ascending indices");
        ok &= expect(delta.heights[1] == 0.1f && delta.heights[2] == 0.5f,
                     "collect must keep modified heights");
    }

    // 用例 2：没改过的两块场差分是空的。
    {
        const HeightField before = makeField(8u, 6u);
        const HeightField after = makeField(8u, 6u);
        TerrainSculptDelta delta;
        std::string error;
        ok &= expect(collectSculptDelta(before, after, delta, error), "collect identical failed");
        ok &= expect(delta.empty(), "identical fields must produce an empty delta");
    }

    // 用例 3：把差分盖回去，逐格等于修改后的高度场，min/max 也刷新。
    {
        const HeightField before = makeField(8u, 6u);
        HeightField after = before;
        after.heights[3] = 0.9f;
        after.heights[40] = 0.5f;
        after.refreshMinMax();

        TerrainSculptDelta delta;
        std::string error;
        collectSculptDelta(before, after, delta, error);

        HeightField restored = before;
        ok &= expect(applySculptDelta(restored, delta, error), "apply failed");
        bool same = true;
        for (std::size_t i = 0; i < after.heights.size(); ++i)
            same = same && restored.heights[i] == after.heights[i];
        ok &= expect(same, "apply must reproduce the sculpted heights");
        ok &= expect(restored.minValue == after.minValue && restored.maxValue == after.maxValue,
                     "apply must refresh min/max");
    }

    // 用例 4：尺寸不一致的场不能求差分。
    {
        const HeightField before = makeField(8u, 6u);
        const HeightField after = makeField(8u, 5u);
        TerrainSculptDelta delta;
        std::string error;
        ok &= expect(!collectSculptDelta(before, after, delta, error), "size mismatch must fail");
        ok &= expect(!error.empty(), "size mismatch must report an error");
    }

    // 用例 5：下标越界 / 长度不等 / 下标非递增都要拒绝。
    {
        HeightField field = makeField(8u, 6u);
        std::string error;

        TerrainSculptDelta outOfRange;
        outOfRange.indices = { 9999u };
        outOfRange.heights = { 0.5f };
        ok &= expect(!applySculptDelta(field, outOfRange, error), "out of range index must fail");

        TerrainSculptDelta lengthMismatch;
        lengthMismatch.indices = { 1u, 2u };
        lengthMismatch.heights = { 0.5f };
        ok &= expect(!applySculptDelta(field, lengthMismatch, error), "length mismatch must fail");

        TerrainSculptDelta unordered;
        unordered.indices = { 5u, 4u };
        unordered.heights = { 0.5f, 0.6f };
        ok &= expect(!applySculptDelta(field, unordered, error), "non-increasing indices must fail");

        TerrainSculptDelta notFinite;
        notFinite.indices = { 1u };
        notFinite.heights = { std::nanf("") };
        ok &= expect(!applySculptDelta(field, notFinite, error), "non-finite height must fail");
    }

    if (ok)
        std::cout << "[Terrain] sculptDeltaSelfTest PASS | cases=5 ascending=ok apply=ok guards=ok\n";
    return ok;
}

} // namespace terrain
