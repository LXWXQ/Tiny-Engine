#version 450

// 水面像素着色：大波解析轮廓 + 水波法线贴图细节 + Fresnel + 环境反射 + 厚度吸收 + 岸边泡沫。
//
// 不做折射（需要先把 hdrScene 复制成可采样纹理，RenderGraph 目前没有 image copy），
// 但水后面不透明几何的世界坐标可以从 G-Buffer 的 inPosition 拿到，用它算
// 视线穿过水的厚度：薄的地方透出浅色，厚的地方变深，岸边自动出现泡沫带。
layout(location = 0) in vec3 fragWorldPos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D environmentMap;
layout(set = 0, binding = 2) uniform sampler2D inPosition;
layout(set = 0, binding = 12) uniform sampler2D reflectionMap;
layout(set = 0, binding = 13) uniform sampler2D ssrMap;
// R-06：无缝水波法线贴图。世界 XZ 平铺，两层不同尺度/流速叠加，
// 替代原来 3 个高频正弦波 —— 后者在远距离采样不足时会形成规则黑白格。
layout(set = 0, binding = 14) uniform sampler2D waterNormalMap;

layout(push_constant) uniform WaterPush
{
    vec4 params;   // time, level, waveStrength, waveSpeed
    vec4 params2;  // farRadius, foamStrength, fogStart, fogDensity
    vec4 params3;  // x = planar reflection enabled, y = SSR enabled
} push;

const vec4 WAVES[4] = vec4[4](
    vec4( 0.9239,  0.3827, 23.0, 0.180),
    vec4(-0.5736,  0.8192, 14.0, 0.100),
    vec4( 0.7071, -0.7071,  9.0, 0.055),
    vec4( 0.2588,  0.9659,  5.5, 0.025)
);

const vec2 kInvAtan = vec2(0.1591, 0.3183);

float bigWaveHeight(vec2 p, float t, float strength)
{
    float h = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float k = 6.28318530718 / WAVES[i].z;
        float omega = sqrt(9.81 * k) * 0.65;
        float phase = dot(WAVES[i].xy, p) * k - t * omega + float(i) * 1.7;
        h += WAVES[i].w * sin(phase);
    }
    return h * strength;
}

// 一个波在当前像素足迹下还能保留多少振幅。phaseStep 是相邻像素之间的
// 相位差：超过约 1 rad 后继续按几何法线采样只会产生随机方向，必须淡出。
// 淡出的能量不会丢，后面会转成粗糙度，让高光变宽而不是消失。
float waveResolve(vec2 dir, float wavelength, vec2 footprintX, vec2 footprintY)
{
    float k = 6.28318530718 / wavelength;
    float footprint = abs(dot(dir, footprintX)) + abs(dot(dir, footprintY));
    return 1.0 - smoothstep(0.5, 1.5, k * footprint);
}

vec2 filteredBigWaveGradient(vec2 p, float t, float strength,
                             vec2 footprintX, vec2 footprintY, out float slopeVariance)
{
    vec2 gradient = vec2(0.0);
    slopeVariance = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float k = 6.28318530718 / WAVES[i].z;
        float omega = sqrt(9.81 * k) * 0.65;
        float phase = dot(WAVES[i].xy, p) * k - t * omega + float(i) * 1.7;
        float resolve = waveResolve(WAVES[i].xy, WAVES[i].z, footprintX, footprintY);
        float slope = WAVES[i].w * strength * k;
        gradient += WAVES[i].xy * slope * cos(phase) * resolve;
        slopeVariance += 0.5 * slope * slope * (1.0 - resolve * resolve);
    }
    return gradient;
}

// 两层法线贴图：一层约 18m 周期（细浪），一层约 42m 周期（较长波纹）。
// 用 textureGrad 显式传入世界 XZ 的像素足迹，掠射角下才会正确选择 mip /
// 各向异性过滤；这正是纯解析小波做不到、会在远处变成黑白格的地方。
// 贴图法线是 (nx, ny, nz)，高度梯度 gx = -nx, gy = -ny。
vec2 waterDetailGradient(vec2 p, float t, float strength, float fade,
                         vec2 footprintX, vec2 footprintY, out float slopeVariance)
{
    const float kScaleA = 1.0 / 18.0;
    const float kScaleB = 1.0 / 42.0;
    vec2 uvA = p * kScaleA + t * vec2( 0.020,  0.012);
    vec2 uvB = p * kScaleB + t * vec2(-0.013,  0.017);

    vec3 nA = textureGrad(waterNormalMap, uvA, footprintX * kScaleA, footprintY * kScaleA).xyz * 2.0 - 1.0;
    vec3 nB = textureGrad(waterNormalMap, uvB, footprintX * kScaleB, footprintY * kScaleB).xyz * 2.0 - 1.0;

    // 两层斜率直接相加；0.55 / 0.35 是相对幅度，保持大波轮廓仍占主导。
    vec2 slope = -(nA.xy * 0.55 + nB.xy * 0.35) * (strength * fade);

    // 贴图 mip 过滤会抹掉未解析的高频斜率，但能量不能凭空消失：
    // 按最高 LOD 估一个很小的方差，交给粗糙度把高光展宽。
    float lodA = textureQueryLod(waterNormalMap, uvA).y;
    float lodB = textureQueryLod(waterNormalMap, uvB).y;
    float lost = smoothstep(1.0, 6.0, max(lodA, lodB));
    slopeVariance = lost * lost * 0.008;
    return slope;
}

vec2 sampleSphericalMap(vec3 v)
{
    return vec2(atan(v.z, v.x) * kInvAtan.x + 0.5,
                0.5 - asin(clamp(v.y, -1.0, 1.0)) * kInvAtan.y);
}

void main()
{
    const float t = push.params.x * push.params.w;
    const float strength = max(push.params.z, 0.0);
    const vec2 p = fragWorldPos.xz;

    // 大波决定轮廓，法线贴图补充细浪。贴图自带完整 mip 链，远处按 LOD
    // 自然淡出；额外再按距离做一次淡出，避免极远处只剩纹理重复。
    const float distanceToCamera = length(p - ubo.cameraPos.xz);
    const float detailFade = 1.0 - smoothstep(150.0, 700.0, distanceToCamera);

    // 像素足迹：dFdx/dFdy 给出世界 XZ 每像素的跨度。大波逐波判断是否
    // 还能被当前采样率解析；法线贴图则把同一足迹交给 textureGrad。
    vec2 footprintX = dFdx(p);
    vec2 footprintY = dFdy(p);
    float bigSlopeVariance = 0.0;
    float detailSlopeVariance = 0.0;
    vec2 gradient = filteredBigWaveGradient(p, t, strength, footprintX, footprintY, bigSlopeVariance);
    gradient += waterDetailGradient(p, t, strength, detailFade,
                                    footprintX, footprintY, detailSlopeVariance);

    vec3 N = normalize(vec3(-gradient.x, 1.0, -gradient.y));
    vec3 V = normalize(ubo.cameraPos.xyz - fragWorldPos);

    // 双面：从水下往上看时法线翻转，避免整片水面变成黑色。
    float NdotV = dot(N, V);
    if (NdotV < 0.0)
    {
        N = -N;
        NdotV = -NdotV;
    }
    NdotV = clamp(NdotV, 0.0, 1.0);

    // 环境反射（等距柱状 HDR）。接缝处把跨 ±PI 的导数折回来，
    // 否则那一列会退到最低 mip，出现一条亮线（和 skybox 用同样的修法）。
    // 未被解析的波能量 -> 像素级粗糙度。远处水面因此从「锐利镜面」变成
    // 略微模糊的反射；高光也随之变宽，不会在一条线上闪。
    float distanceRoughness = smoothstep(120.0, 3000.0, distanceToCamera) * 0.30;
    float roughness = clamp(sqrt(bigSlopeVariance + detailSlopeVariance) * 7.0 +
                            distanceRoughness, 0.02, 0.65);
    float roughNorm = smoothstep(0.03, 0.55, roughness);

    vec3 R = reflect(-V, N);
    vec2 envUV = sampleSphericalMap(R);
    vec2 dx = dFdx(envUV);
    vec2 dy = dFdy(envUV);
    dx.x -= round(dx.x);
    dy.x -= round(dy.x);
    float reflectionBlur = 1.0 + roughness * 12.0;
    vec3 reflection = textureGrad(environmentMap, envUV,
                                  dx * reflectionBlur, dy * reflectionBlur).rgb;
    const vec3 environmentReflection = reflection;

    // 水后不透明几何的世界坐标 -> 沿视线方向的厚度。天空写入的是约 10000m，
    // 于是"没有底"的地方自动变成深海。
    vec2 screenUV = gl_FragCoord.xy / vec2(textureSize(inPosition, 0));

    // 半分辨率的反射纹理都沿用屏幕坐标，按法线做轻微扰动。
    vec2 reflectionUV = clamp(screenUV + N.xz * (0.0015 + roughness * 0.006),
                              vec2(0.001), vec2(0.999));

    // C1：半分辨率平面反射。镜像相机把水面以上的真实几何重画一遍，
    // 远水因此能反射对岸/山体，而不是只剩天空环境贴图。
    // 关掉开关时 params3.x = 0，保持原来的环境贴图反射。
    if (push.params3.x > 0.5)
        reflection = texture(reflectionMap, reflectionUV).rgb;

    // F6-01：近场屏幕空间反射。alpha 是 SSR 自己的置信度（没命中=0），
    // 再用粗糙度和距离衰减：粗糙水面本来就看不清镜像，远处交给平面反射/IBL。
    if (push.params3.y > 0.5)
    {
        vec4 ssr = texture(ssrMap, reflectionUV);
        float ssrWeight = ssr.a
            * (1.0 - smoothstep(0.20, 0.55, roughness))
            * (1.0 - smoothstep(120.0, 220.0, distanceToCamera));
        reflection = mix(reflection, ssr.rgb, clamp(ssrWeight, 0.0, 1.0));
    }

    // 平面反射会完整覆盖环境反射，掠射角下容易把对岸深色地形整片铺在水面上。
    // 只对“暗反射”补天空底色：亮的水面/天空倒影保持原来的对比度，
    // 被深色地形压黑的部分才提亮，避免整片水变成均匀的乳白蓝。
    const float reflectionLuma = dot(reflection, vec3(0.2126, 0.7152, 0.0722));
    const float darkReflection = 1.0 - smoothstep(0.04, 0.22, reflectionLuma);
    reflection = mix(reflection, environmentReflection, 0.34 * darkReflection);

    vec3 scenePos = texture(inPosition, screenUV).xyz;
    const float sceneDistance = length(scenePos - ubo.cameraPos.xyz);
    const float waterDistance = length(fragWorldPos - ubo.cameraPos.xyz);
    const float thickness = max(sceneDistance - waterDistance, 0.0);
    const float depthFactor = 1.0 - exp(-thickness * 0.22);

    const vec3 shallowColor = vec3(0.050, 0.160, 0.167);
    const vec3 deepColor = vec3(0.016, 0.058, 0.075);
    vec3 body = mix(shallowColor, deepColor, depthFactor);

    const vec3 L = normalize(-ubo.lightDirection.xyz);
    const vec3 radiance = ubo.lightColor.rgb * ubo.lightColor.w;
    const float sunDiffuse = max(dot(N, L), 0.0);
    body *= 0.55 + 0.45 * sunDiffuse;

    const float fresnel = clamp(0.02 + 0.98 * pow(1.0 - NdotV, 5.0), 0.0, 1.0);
    vec3 color = mix(body, reflection, fresnel);

    // 太阳高光：近处保持锐利，远处按粗糙度展宽并降低峰值（specular AA）。
    const vec3 H = normalize(L + V);
    const float NdotH = max(dot(N, H), 0.0);
    float specPower = mix(900.0, 24.0, roughNorm);
    float specular = pow(NdotH, specPower) * mix(3.0, 0.55, roughNorm)
                   + pow(NdotH, mix(120.0, 8.0, roughNorm)) * mix(0.10, 0.18, roughNorm);
    color += radiance * specular;

    // 岸边泡沫：只贴着水陆交界出现。旧版把波峰项铺满整个水面，并且用
    // sin(x)*sin(y) 生成泡沫，俯视时整片水会出现规则菱形棋盘格。
    // 现在泡沫由水波法线贴图的低频噪声打散，且波峰项也乘 shore，
    // 远离岸边的深水不再产生任何泡沫。
    const float shore = 1.0 - smoothstep(0.0, 1.8, thickness);
    const float crest = smoothstep(0.10, 0.30, bigWaveHeight(p, t, strength));
    const vec2 foamUV = p * (1.0 / 7.0) + t * vec2(0.011, -0.008);
    const float foamNoise = textureLod(waterNormalMap, foamUV, 1.5).x;
    const float foamMask = smoothstep(0.34, 0.74, foamNoise);
    const float foam = clamp(shore * foamMask * (0.75 + 0.25 * crest), 0.0, 1.0);
    color = mix(color, vec3(0.82, 0.90, 0.94), foam * 0.55);

    // 全场景距离雾（B）。水体用真实的视线距离，而不是相机 XZ 距离；
    // 雾色取环境图地平线方向，远景才能无缝淡出到天空而不是一条硬边。
    float fogDensity = push.params2.w;
    if (fogDensity > 0.0)
    {
        float fogAmount = 1.0 - exp(-fogDensity * max(waterDistance - push.params2.z, 0.0));
        vec3 fogColor = textureLod(environmentMap,
            sampleSphericalMap(normalize(vec3(-V.x, 0.035, -V.z))), 3.0).rgb;
        color = mix(color, fogColor, clamp(fogAmount, 0.0, 1.0));
    }

    float alpha = mix(0.18, 0.89, depthFactor);
    alpha = clamp(alpha + fresnel * 0.55, 0.0, 0.985);
    alpha = max(alpha, foam * 0.90);

    outColor = vec4(color, alpha);
}
