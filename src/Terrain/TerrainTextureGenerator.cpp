#include "Terrain/TerrainTextureGenerator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace terrain
{

namespace
{
constexpr float kPi = 3.14159265358979323846f;

uint32_t mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

uint32_t hash2D(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(x) * 0x9e3779b1u;
    h = mix32(h);
    h ^= static_cast<uint32_t>(y) * 0x85ebca77u;
    return mix32(h);
}

float fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float periodicValueNoise(float u, float v, uint32_t periodX, uint32_t periodY, uint32_t seed)
{
    u -= std::floor(u);
    v -= std::floor(v);

    const float x = u * static_cast<float>(periodX);
    const float y = v * static_cast<float>(periodY);
    const int32_t x0Raw = static_cast<int32_t>(std::floor(x));
    const int32_t y0Raw = static_cast<int32_t>(std::floor(y));
    const float tx = fade(x - static_cast<float>(x0Raw));
    const float ty = fade(y - static_cast<float>(y0Raw));

    const auto wrap = [](int32_t value, uint32_t period) -> int32_t
    {
        int32_t wrapped = value % static_cast<int32_t>(period);
        if (wrapped < 0)
            wrapped += static_cast<int32_t>(period);
        return wrapped;
    };

    const int32_t x0 = wrap(x0Raw, periodX);
    const int32_t y0 = wrap(y0Raw, periodY);
    const int32_t x1 = wrap(x0Raw + 1, periodX);
    const int32_t y1 = wrap(y0Raw + 1, periodY);

    const auto sample = [seed](int32_t ix, int32_t iy) -> float
    {
        return static_cast<float>(hash2D(ix, iy, seed) & 0x00ffffffu) / 16777215.0f;
    };

    const float a = sample(x0, y0);
    const float b = sample(x1, y0);
    const float c = sample(x0, y1);
    const float d = sample(x1, y1);
    const float nx0 = a + tx * (b - a);
    const float nx1 = c + tx * (d - c);
    return nx0 + ty * (nx1 - nx0);
}

float fbm(float u, float v, uint32_t periodX, uint32_t periodY,
          uint32_t octaves, uint32_t seed)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float amplitudeSum = 0.0f;

    for (uint32_t octave = 0; octave < octaves; ++octave)
    {
        sum += amplitude * periodicValueNoise(
            u, v, periodX << octave, periodY << octave,
            mix32(seed ^ (0x9e3779b9u * (octave + 1u))));
        amplitudeSum += amplitude;
        amplitude *= 0.5f;
    }

    return sum / std::max(amplitudeSum, 1e-8f);
}

float smoothstep01(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Color
{
    float r;
    float g;
    float b;
};

Color mixColor(const Color& a, const Color& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t
    };
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void writePixel(std::vector<uint8_t>& pixels, uint32_t index,
                const Color& color, float alpha = 1.0f)
{
    const size_t offset = static_cast<size_t>(index) * 4u;
    pixels[offset + 0u] = toByte(color.r);
    pixels[offset + 1u] = toByte(color.g);
    pixels[offset + 2u] = toByte(color.b);
    pixels[offset + 3u] = toByte(alpha);
}

struct LayerArt
{
    Color sandDark{ 0.55f, 0.46f, 0.32f };
    Color sandLight{ 0.83f, 0.73f, 0.55f };
    Color grassDark{ 0.17f, 0.29f, 0.10f };
    Color grassMid{ 0.31f, 0.45f, 0.17f };
    Color grassDry{ 0.47f, 0.44f, 0.23f };
    Color rockDark{ 0.19f, 0.18f, 0.16f };
    Color rockLight{ 0.55f, 0.52f, 0.47f };
    Color snowShade{ 0.82f, 0.87f, 0.92f };
    Color snowLight{ 0.96f, 0.98f, 1.00f };
};

void generateSand(float u, float v, float macro, float grain,
                  const LayerArt& art, Color& color, float& height)
{
    const float ripple = 0.5f + 0.5f *
        std::sin(2.0f * kPi * (u * 14.0f + v * 5.0f) + macro * 8.0f);
    height = std::clamp(0.55f * ripple + 0.30f * macro + 0.15f * grain, 0.0f, 1.0f);
    color = mixColor(art.sandDark, art.sandLight,
                     std::clamp(0.20f * grain + 0.45f * ripple + 0.35f * macro, 0.0f, 1.0f));
    const float highlight = smoothstep01(0.62f, 1.0f, ripple) * 0.10f;
    color = mixColor(color, { 0.92f, 0.86f, 0.70f }, highlight);
}

void generateGrass(float u, float v, float macro, float grain,
                   const LayerArt& art, Color& color, float& height)
{
    const float broad = fbm(u, v, 8u, 8u, 4u, 0x19a24b71u);
    const float medium = fbm(u, v, 32u, 32u, 4u, 0x4f7a3au);
    const float blade = fbm(u, v, 64u, 24u, 3u, 0x7b1d43u);
    const float dry = smoothstep01(0.54f, 0.80f,
        fbm(u, v, 4u, 4u, 2u, 0x846ca68bu));

    height = std::clamp(0.40f * broad + 0.35f * medium +
                        0.18f * grain + 0.07f * blade, 0.0f, 1.0f);
    Color base = mixColor(art.grassDark, art.grassMid,
                          std::clamp(0.45f * broad + 0.35f * medium + 0.20f * grain,
                                     0.0f, 1.0f));
    base = mixColor(base, art.grassDry, dry * 0.18f);

    const float bladeLight = smoothstep01(0.62f, 0.94f, blade) * 0.10f;
    color = mixColor(base, { 0.39f, 0.51f, 0.22f }, bladeLight);
}

void generateRock(float u, float v, float macro, float grain,
                  const LayerArt& art, Color& color, float& height)
{
    const float medium = fbm(u, v, 16u, 16u, 4u, 0x6b6560u);
    const float fine = fbm(u, v, 64u, 64u, 3u, 0x391b9b2du);
    const float fissure = std::pow(1.0f - std::fabs(2.0f * medium - 1.0f), 6.0f);
    height = std::clamp(0.40f * macro + 0.42f * medium + 0.18f * fine, 0.0f, 1.0f);
    color = mixColor(art.rockDark, art.rockLight,
                     std::clamp(0.55f * macro + 0.30f * fine + 0.15f * grain, 0.0f, 1.0f));
    color = mixColor(color, { 0.08f, 0.07f, 0.06f }, fissure * 0.38f);
}

void generateSnow(float u, float v, float macro, float grain,
                  const LayerArt& art, Color& color, float& height)
{
    const float fine = fbm(u, v, 64u, 64u, 3u, 0xe8edf2u);
    const float drift = 0.5f + 0.5f *
        std::sin(2.0f * kPi * (u * 6.0f + v * 3.0f) + macro * 5.0f);
    height = std::clamp(0.35f * macro + 0.45f * fine + 0.20f * drift, 0.0f, 1.0f);
    color = mixColor(art.snowShade, art.snowLight,
                     std::clamp(0.55f * fine + 0.30f * drift + 0.15f * grain, 0.0f, 1.0f));
}
} // namespace

bool generateTerrainTextureSet(TerrainTextureSet& outTextures,
                               uint32_t resolution,
                               std::string& errorMessage)
{
    if (resolution < 16u || resolution > 1024u ||
        (resolution & (resolution - 1u)) != 0u)
    {
        errorMessage = "terrain texture resolution must be a power of two in [16, 1024]";
        return false;
    }

    outTextures.width = resolution;
    outTextures.height = resolution;
    const size_t pixelCount = static_cast<size_t>(resolution) * resolution;
    std::array<std::vector<float>, TerrainTextureSet::kLayerCount> heights;
    for (uint32_t layer = 0; layer < TerrainTextureSet::kLayerCount; ++layer)
    {
        outTextures.albedoRGBA[layer].assign(pixelCount * 4u, 255u);
        outTextures.normalRGBA[layer].assign(pixelCount * 4u, 255u);
        outTextures.heightRGBA[layer].assign(pixelCount * 4u, 255u);
        outTextures.roughnessRGBA[layer].assign(pixelCount * 4u, 255u);
        heights[layer].assign(pixelCount, 0.0f);
    }

    const LayerArt art;
    const uint32_t layerSeeds[4] = { 0xc2b280u, 0x4f7a3au, 0x6b6560u, 0xe8edf2u };
    const float normalStrength[4] = { 2.2f, 2.8f, 5.5f, 0.8f };
    const float roughnessBase[4] = { 0.95f, 0.89f, 0.80f, 0.68f };

    // Generate color and height tiles. All noise periods are integers, so the
    // texture repeats cleanly along both axes.
    for (uint32_t layer = 0; layer < TerrainTextureSet::kLayerCount; ++layer)
    {
        for (uint32_t y = 0; y < resolution; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution);
            for (uint32_t x = 0; x < resolution; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(resolution);
                const uint32_t index = y * resolution + x;
                const uint32_t seed = layerSeeds[layer];

                const float macro = fbm(u, v, 4u, 4u, 5u, seed ^ 0xa511e9b3u);
                const float grain = fbm(u, v, 128u, 128u, 2u, seed ^ 0x63d83595u);
                Color color{};
                float height = 0.0f;

                switch (layer)
                {
                case 0: generateSand(u, v, macro, grain, art, color, height); break;
                case 1: generateGrass(u, v, macro, grain, art, color, height); break;
                case 2: generateRock(u, v, macro, grain, art, color, height); break;
                default: generateSnow(u, v, macro, grain, art, color, height); break;
                }

                heights[layer][index] = std::clamp(height, 0.0f, 1.0f);
                writePixel(outTextures.albedoRGBA[layer], index, color);
            }
        }
    }

    // Tileable tangent-space normals from wrapped central differences.
    for (uint32_t layer = 0; layer < TerrainTextureSet::kLayerCount; ++layer)
    {
        for (uint32_t y = 0; y < resolution; ++y)
        {
            const uint32_t y0 = (y + resolution - 1u) % resolution;
            const uint32_t y1 = (y + 1u) % resolution;
            for (uint32_t x = 0; x < resolution; ++x)
            {
                const uint32_t x0 = (x + resolution - 1u) % resolution;
                const uint32_t x1 = (x + 1u) % resolution;
                const uint32_t index = y * resolution + x;

                const float dx = (heights[layer][y * resolution + x1] -
                                  heights[layer][y * resolution + x0]) * 0.5f;
                const float dz = (heights[layer][y1 * resolution + x] -
                                  heights[layer][y0 * resolution + x]) * 0.5f;

                float nx = -dx * normalStrength[layer];
                float ny = -dz * normalStrength[layer];
                float nz = 1.0f;
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length < 1e-8f)
                    nz = 1.0f;
                else
                {
                    nx /= length;
                    ny /= length;
                    nz /= length;
                }

                writePixel(outTextures.normalRGBA[layer], index,
                           { nx * 0.5f + 0.5f, ny * 0.5f + 0.5f, nz * 0.5f + 0.5f });

                const float height = heights[layer][index];
                writePixel(outTextures.heightRGBA[layer], index,
                           { height, height, height });
                const float roughness = std::clamp(
                    roughnessBase[layer] + (height - 0.5f) * 0.10f, 0.0f, 1.0f);
                writePixel(outTextures.roughnessRGBA[layer], index,
                           { roughness, roughness, roughness });
            }
        }
    }

    errorMessage.clear();
    return true;
}

} // namespace terrain