#version 450

// 水面像素着色：解析波面法线 + Fresnel + 环境反射 + 厚度吸收 + 岸边泡沫。
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

layout(push_constant) uniform WaterPush
{
    vec4 params;   // time, level, waveStrength, waveSpeed
    vec4 params2;  // farRadius, foamStrength, fogStart, fogDensity
    vec4 params3;  // x = planar reflection enabled
} push;

const vec4 WAVES[4] = vec4[4](
    vec4( 0.9239,  0.3827, 23.0, 0.180),
    vec4(-0.5736,  0.8192, 14.0, 0.100),
    vec4( 0.7071, -0.7071,  9.0, 0.055),
    vec4( 0.2588,  0.9659,  5.5, 0.025)
);

// 三个高频小波只贡献法线细节，不做顶点位移。波长 1.7 / 1.1 / 0.75m。
const vec4 RIPPLES[3] = vec4[3](
    vec4( 0.8660,  0.5000, 1.70, 0.010),
    vec4(-0.5000,  0.8660, 1.10, 0.006),
    vec4( 0.9659, -0.2588, 0.75, 0.004)
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

vec2 filteredRippleGradient(vec2 p, float t, float fade,
                            vec2 footprintX, vec2 footprintY, out float slopeVariance)
{
    vec2 gradient = vec2(0.0);
    slopeVariance = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        float k = 6.28318530718 / RIPPLES[i].z;
        float omega = sqrt(9.81 * k) * 1.35;
        float phase = dot(RIPPLES[i].xy, p) * k - t * omega * 1.3 + float(i) * 2.1;
        float resolve = waveResolve(RIPPLES[i].xy, RIPPLES[i].z, footprintX, footprintY);
        float slope = RIPPLES[i].w * fade * k;
        gradient += RIPPLES[i].xy * slope * cos(phase) * resolve;
        slopeVariance += 0.5 * slope * slope * (1.0 - resolve * resolve);
    }
    return gradient;
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

    // 大波决定轮廓，小波只在大约 220m 内贡献法线细节；远处直接淡出，
    // 否则细密波纹在低采样率下会变成闪烁的点阵。
    const float distanceToCamera = length(p - ubo.cameraPos.xz);
    const float rippleFade = 1.0 - smoothstep(60.0, 220.0, distanceToCamera);

    // 像素足迹：dFdx/dFdy 给出世界 XZ 每像素的跨度。用它逐波判断是否
    // 还能被当前采样率解析，而不是只看相机距离 —— 掠射角下远处的足迹
    // 会急剧变大，这正是之前条带/闪烁最明显的地方。
    vec2 footprintX = dFdx(p);
    vec2 footprintY = dFdy(p);
    float bigSlopeVariance = 0.0;
    float rippleSlopeVariance = 0.0;
    vec2 gradient = filteredBigWaveGradient(p, t, strength, footprintX, footprintY, bigSlopeVariance);
    gradient += filteredRippleGradient(p, t, rippleFade, footprintX, footprintY, rippleSlopeVariance);

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
    float roughness = clamp(sqrt(bigSlopeVariance + rippleSlopeVariance) * 7.0 +
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

    // 水后不透明几何的世界坐标 -> 沿视线方向的厚度。天空写入的是约 10000m，
    // 于是"没有底"的地方自动变成深海。
    vec2 screenUV = gl_FragCoord.xy / vec2(textureSize(inPosition, 0));

    // C1：半分辨率平面反射。镜像相机把水面以上的真实几何重画一遍，
    // 远水因此能反射对岸/山体，而不是只剩天空环境贴图。
    // 反射纹理是半分辨率，UV 直接沿用屏幕坐标并按法线做轻微扰动；
    // 关掉开关时 params3.x = 0，保持原来的环境贴图反射。
    if (push.params3.x > 0.5)
    {
        vec2 reflectionUV = clamp(screenUV + N.xz * (0.0015 + roughness * 0.006),
                                  vec2(0.001), vec2(0.999));
        reflection = texture(reflectionMap, reflectionUV).rgb;
    }

    vec3 scenePos = texture(inPosition, screenUV).xyz;
    const float sceneDistance = length(scenePos - ubo.cameraPos.xyz);
    const float waterDistance = length(fragWorldPos - ubo.cameraPos.xyz);
    const float thickness = max(sceneDistance - waterDistance, 0.0);
    const float depthFactor = 1.0 - exp(-thickness * 0.22);

    const vec3 shallowColor = vec3(0.045, 0.150, 0.160);
    const vec3 deepColor = vec3(0.008, 0.035, 0.055);
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

    // 岸边泡沫：水薄 + 波峰 + 两点交叉的正弦噪声。不做纹理采样，
    // 第一版保持无资源依赖；以后要更细可以换成图集里的泡沫 tile。
    const float shore = 1.0 - smoothstep(0.0, 1.6, thickness);
    const float crest = smoothstep(0.08, 0.26, bigWaveHeight(p, t, strength));
    const float foamPattern = 0.55 + 0.45 * sin(p.x * 1.7 + t * 0.6)
                                          * sin(p.y * 2.1 - t * 0.45);
    const float foam = clamp((shore * 0.90 + crest * 0.25) * foamPattern, 0.0, 1.0);
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

    float alpha = mix(0.18, 0.92, depthFactor);
    alpha = clamp(alpha + fresnel * 0.55, 0.0, 0.985);
    alpha = max(alpha, foam * 0.90);

    outColor = vec4(color, alpha);
}
