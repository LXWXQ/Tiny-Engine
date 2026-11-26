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
    float reprojectionMode;   // 0 = 速度图，1 = 上一帧矩阵（A/B 与回退）
    float probeEnabled;       // 1 = 把两种重投影的像素差攒进 probe 缓冲
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
        bool hasVelocity = pc.reprojectionMode < 0.5 && dot(velocity, velocity) > 1e-12;
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
                // 3x3 邻域两种夹法取交集：AABB 挡遮挡边缘把背景拖进前景，
                // YCoCg 的均值±1.25σ 挡「颜色不同但亮度接近」的细节 ——
                // 只夹 AABB 会让细纹理在反复积累里慢慢糊平。
                ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
                vec3 minY = vec3(1e9);
                vec3 maxY = vec3(-1e9);
                vec3 sumY = vec3(0.0);
                vec3 sumSqY = vec3(0.0);
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
                    }
                }
                const vec3 meanY = sumY / 9.0;
                const vec3 sigmaY = sqrt(max(vec3(0.0), sumSqY / 9.0 - meanY * meanY));
                const float kSigmaScale = 1.25;
                minY = max(minY, meanY - sigmaY * kSigmaScale);
                maxY = min(maxY, meanY + sigmaY * kSigmaScale);
                maxY = max(maxY, minY + vec3(1.0 / 255.0));

                vec3 historyY = rgbToYCoCg(texture(historyColor, previousUv).rgb);
                historyY = clamp(historyY, minY, maxY);

                result = mix(current, yCoCgToRgb(historyY),
                             clamp(pc.historyWeight, 0.0, 0.98));
            }
        }
    }

    // 探针按 8x8 抽一格：整屏 60M 像素的定点和会溢出 32 位，
    // 抽样后约 1M 个点，每点以 1/32 像素为单位累加也不会溢出。
    if (pc.probeEnabled > 0.5 && (int(gl_FragCoord.x) & 7) == 0 && (int(gl_FragCoord.y) & 7) == 0)
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