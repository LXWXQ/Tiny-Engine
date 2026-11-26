#version 450

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outAux;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
    int numLights;
    float exposure;
    mat4 previousProjectionView;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D currentColor;
layout(set = 1, binding = 1) uniform sampler2D worldPosTex;
layout(set = 1, binding = 2) uniform sampler2D historyColor;
layout(set = 1, binding = 3) uniform sampler2D velocityTex;

// 探针：把「速度图重投影」与「上一帧矩阵重投影」的差值攒起来，CPU 每 30 帧读一次。
layout(set = 1, binding = 4) buffer ProbeBuffer {
    uint probePixelCount;
    uint probeBigCount;
    uint probeSumUnits;   // 1/32 像素为单位
    uint probeMaxMilli;
} probe;

layout(push_constant) uniform Push {
    vec2 previousJitterUv;
    vec2 texelSize;
    float historyWeight;
    float historyValid;
    // 位标记：1 = 强制矩阵重投影（A/B 与回退），2 = 速度探针，4 = 经典 3x3 邻域（P5-3 回退）
    uint taaFlags;
    uint padFlags;
} pc;

vec3 rgbToYCoCg(vec3 c)
{
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)),
                dot(c, vec3(0.5, 0.0, -0.5)),
                dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 yCoCgToRgb(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

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
        // 上一帧 UV 优先由速度图给出（单位就是 UV 增量）。旧实现只用
        // 「上一帧矩阵 × 当前世界位置」反推：静止几何是对的，运动的物体算出来的
        // 还是当前位置，历史颜色被拉到错的地方 —— 这正是移动物体拖影的来源。
        // 天空盒/网格/植被的速度是 0（它们不产生屏幕空间运动），这时仍走矩阵，
        // 否则相机动起来天空会被抹开。
        vec2 velocity = texture(velocityTex, uv).xy;
        bool hasVelocity = (pc.taaFlags & 1u) == 0u && dot(velocity, velocity) > 1e-12;
        vec2 previousUv;
        bool reprojectable = true;
        if (hasVelocity)
        {
            previousUv = uv - velocity + pc.previousJitterUv;
        }
        else
        {
            vec4 previousClip = ubo.previousProjectionView * vec4(worldPos, 1.0);
            reprojectable = previousClip.w > 0.0001;
            previousUv = reprojectable
                ? previousClip.xy / previousClip.w * 0.5 + 0.5 + pc.previousJitterUv
                : uv;
        }

        if (reprojectable &&
            all(greaterThanEqual(previousUv, vec2(0.0))) &&
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
                // 邻域统计。经典版是 3x3 全收（P5-3 的回退，`--taa-classic-kernel`）；
                // 默认版是 5x5 十字的 13 个抽头，且**只收世界半径落在中心容差内的抽头** ——
                // 薄几何（草叶/栏杆）与遮挡边缘上，背景抽头会把背景颜色带进前景的夹制盒，
                // 历史被一个错误的盒夹住之后仍旧拖影。收不到的抽头一律丢弃。
                ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
                const bool classic = (pc.taaFlags & 4u) != 0u;

                vec3 minY = vec3(1e9);
                vec3 maxY = vec3(-1e9);
                vec3 sumY = vec3(0.0);
                vec3 sumSqY = vec3(0.0);
                int taps = 0;
                int validTaps = 0;

                if (classic)
                {
                    for (int y = -1; y <= 1; ++y)
                    {
                        for (int x = -1; x <= 1; ++x)
                        {
                            ivec2 samplePixel = clamp(pixel + ivec2(x, y),
                                                      ivec2(0), size - 1);
                            vec3 c = rgbToYCoCg(texelFetch(currentColor, samplePixel, 0).rgb);
                            minY = min(minY, c);
                            maxY = max(maxY, c);
                            sumY += c;
                            sumSqY += c * c;
                            ++taps;
                        }
                    }
                    validTaps = taps;
                }
                else
                {
                    for (int i = -2; i <= 2; ++i)
                    {
                        for (int j = -2; j <= 2; ++j)
                        {
                            if (abs(i) + abs(j) > 2)
                                continue;
                            ivec2 samplePixel = clamp(pixel + ivec2(i, j),
                                                      ivec2(0), size - 1);
                            const float sampleRadius =
                                length(texelFetch(worldPosTex, samplePixel, 0).xyz);
                            ++taps;
                            if (abs(sampleRadius - worldRadius) > tolerance)
                                continue;
                            ++validTaps;
                            vec3 c = rgbToYCoCg(texelFetch(currentColor, samplePixel, 0).rgb);
                            minY = min(minY, c);
                            maxY = max(maxY, c);
                            sumY += c;
                            sumSqY += c * c;
                        }
                    }
                    if (validTaps == 0)
                    {
                        const vec3 centerY = rgbToYCoCg(current);
                        minY = centerY;
                        maxY = centerY;
                        sumY = centerY;
                        sumSqY = centerY * centerY;
                        validTaps = 1;
                    }
                }

                const float invCount = 1.0 / float(validTaps);
                const vec3 meanY = sumY * invCount;
                const vec3 sigmaY = sqrt(max(vec3(0.0), sumSqY * invCount - meanY * meanY));
                const float kSigmaScale = classic ? 1.25 : 1.0;
                minY = max(minY, meanY - sigmaY * kSigmaScale);
                maxY = min(maxY, meanY + sigmaY * kSigmaScale);
                maxY = max(maxY, minY + vec3(1.0 / 255.0));

                vec3 historyY = rgbToYCoCg(texture(historyColor, previousUv).rgb);
                historyY = clamp(historyY, minY, maxY);

                // 有效抽头占比就是「这块像素有多薄/多靠边」的度量：占比低说明邻域里
                // 一半以上是别的东西（去遮挡、薄几何、轮廓），越不该信历史。
                const float coverage = classic ? 1.0 : float(validTaps) / float(taps);
                const float weightScale = mix(0.45, 1.0, coverage);

                result = mix(current, yCoCgToRgb(historyY),
                             clamp(pc.historyWeight, 0.0, 0.98) * weightScale);
            }
        }
    }

    // 探针按 8x8 抽一格：整屏 60M 像素的定点和会溢出 32 位，
    // 抽样后约 1M 个点，每点以 1/32 像素为单位累加也不会溢出。
    if ((pc.taaFlags & 2u) != 0u && (int(gl_FragCoord.x) & 7) == 0 && (int(gl_FragCoord.y) & 7) == 0)
    {
        vec2 velocity = texture(velocityTex, uv).xy;
        vec4 currClip = ubo.projectionView * vec4(worldPos, 1.0);
        vec4 prevClip = ubo.previousProjectionView * vec4(worldPos, 1.0);
        vec2 matrixVelocity = (currClip.w > 1e-4 && prevClip.w > 1e-4)
            ? (currClip.xy / currClip.w - prevClip.xy / prevClip.w) * 0.5
            : vec2(0.0);
        vec2 diffPx = abs(velocity - matrixVelocity) * vec2(size);
        uint units = uint(min((diffPx.x + diffPx.y) * 32.0, 65000.0));
        uint milliMax = uint(min(max(diffPx.x, diffPx.y) * 1000.0, 65000.0));
        atomicAdd(probe.probePixelCount, 1u);
        if (max(diffPx.x, diffPx.y) > 0.5)
            atomicAdd(probe.probeBigCount, 1u);
        atomicAdd(probe.probeSumUnits, units);
        atomicMax(probe.probeMaxMilli, milliMax);
    }

    outColor = vec4(result, worldRadius);
    outAux = vec4(result, worldRadius);
}