#version 450

// SSAO is generated from a tiled 4x4 noise texture. This small Gaussian
// post-filter removes the visible screen-space tiling/noise before Lighting
// consumes the ambient-occlusion mask.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D aoTexture;

void main()
{
    const float weights[5] = float[](1.0, 4.0, 6.0, 4.0, 1.0);
    vec2 texelSize = 1.0 / vec2(textureSize(aoTexture, 0));

    float sum = 0.0;
    float totalWeight = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float weight = weights[x + 2] * weights[y + 2];
            vec2 uv = inUV + vec2(float(x), float(y)) * texelSize;
            sum += texture(aoTexture, uv).r * weight;
            totalWeight += weight;
        }
    }

    float ao = sum / totalWeight;
    outFragColor = vec4(ao, ao, ao, 1.0);
}
