#version 450

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outAux;

struct PointLight
{
    vec4 position;
    vec4 color;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
    int numLights;
    float exposure;
    PointLight pointLights[100];
    mat4 previousProjectionView;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D currentColor;
layout(set = 1, binding = 1) uniform sampler2D worldPosTex;
layout(set = 1, binding = 2) uniform sampler2D historyColor;

layout(push_constant) uniform Push {
    vec2 previousJitterUv;
    vec2 texelSize;
    float historyWeight;
    float historyValid;
    float pad0;
    float pad1;
} pc;

void main()
{
    vec2 uv = inUV;
    vec3 current = texture(currentColor, uv).rgb;
    vec3 worldPos = texture(worldPosTex, uv).xyz;
    float worldRadius = length(worldPos);

    vec3 result = current;
    ivec2 size = textureSize(currentColor, 0);

    if (pc.historyValid > 0.5 && pc.historyWeight > 0.0001)
    {
        vec4 previousClip = ubo.previousProjectionView * vec4(worldPos, 1.0);
        if (previousClip.w > 0.0001)
        {
            vec2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
            previousUv += pc.previousJitterUv;

            if (all(greaterThanEqual(previousUv, vec2(0.0))) &&
                all(lessThanEqual(previousUv, vec2(1.0))))
            {
                ivec2 historyPixel = clamp(ivec2(previousUv * vec2(size)),
                                           ivec2(0), size - 1);
                float historyRadius = texelFetch(historyColor, historyPixel, 0).a;

                // 世界半径在同一次相机平移中保持不变，是比深度更稳的
                // 几何一致性判据；天空的世界位置在很远处，阈值随之放大。
                float tolerance = max(0.05, worldRadius * 0.0025);
                if (abs(worldRadius - historyRadius) <= tolerance)
                {
                    vec3 history = texture(historyColor, previousUv).rgb;

                    // 3x3 邻域 AABB 钳制：抑制遮挡边缘把背景颜色拖进前景。
                    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
                    vec3 minColor = current;
                    vec3 maxColor = current;
                    for (int y = -1; y <= 1; ++y)
                    {
                        for (int x = -1; x <= 1; ++x)
                        {
                            ivec2 samplePixel = clamp(pixel + ivec2(x, y),
                                                      ivec2(0), size - 1);
                            vec3 sampleColor = texelFetch(currentColor, samplePixel, 0).rgb;
                            minColor = min(minColor, sampleColor);
                            maxColor = max(maxColor, sampleColor);
                        }
                    }

                    history = clamp(history, minColor, maxColor);
                    result = mix(current, history, clamp(pc.historyWeight, 0.0, 0.98));
                }
            }
        }
    }

    outColor = vec4(result, worldRadius);
    outAux = vec4(result, worldRadius);
}
