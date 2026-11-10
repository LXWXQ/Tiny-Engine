#version 450

// F6-05：体积云（全屏光线步进）。
//
// 只读 G-Buffer 世界坐标来确定“最近的遮挡表面”，云层本身既不写深度也不进
// G-Buffer。输出预乘 alpha 的颜色，混合方程见 CloudPass::createPipeline。
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D inPosition;
layout(set = 1, binding = 1) uniform sampler3D uNoise;

layout(push_constant) uniform CloudPush
{
    mat4 inverseProjectionView;
    vec4 params0;   // x=coverage y=density z=baseHeight w=thickness
    vec4 params1;   // x=windSpeed y=absorption z=steps w=lightSteps
    vec4 params2;   // x=time y=maxDistance z=ambient w=enabled
    vec4 color;     // rgb=云体散射色调
} push;

// 交错梯度噪声：给步进起点一个像素级偏移，避免薄云层出现同心圆条带。
float interleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float heightProfile(vec3 p)
{
    float h = clamp((p.y - push.params0.z) / max(push.params0.w, 1.0), 0.0, 1.0);
    // 顶端淡出用 1-smoothstep 而非反向 edge：GLSL 对 edge0 >= edge1 的结果未定义。
    return smoothstep(0.0, 0.10, h) * (1.0 - smoothstep(0.60, 1.0, h));
}

// 云层密度。3D 纹理是 64^3 可平铺 RGBA：
//   R = 低频形状（Perlin + Worley），G = Worley 侵蚀，B = 高频细节，A = 大尺度天气。
float sampleDensity(vec3 p)
{
    if (p.y < push.params0.z || p.y > push.params0.z + push.params0.w)
        return 0.0;

    float speed = push.params1.x;
    vec3 wind = vec3(push.params2.x * speed, 0.0, push.params2.x * speed * 0.35) * 0.00055;
    vec4 n = texture(uNoise, p * 0.00055 + wind);

    float coverage = clamp(push.params0.x * mix(0.62, 1.22, n.a), 0.0, 1.0);
    float threshold = 1.0 - coverage;
    float shape = n.r * 0.85 + n.b * 0.15;
    float density = smoothstep(threshold, threshold + 0.22, shape);
    density *= mix(0.45, 1.0, n.g);
    density *= heightProfile(p);
    return density * push.params0.y;
}

void main()
{
    vec3 camera = ubo.cameraPos.xyz;
    float maxDistance = max(push.params2.y, 1.0);

    vec2 ndc = inUV * 2.0 - 1.0;
    vec4 nearPoint = push.inverseProjectionView * vec4(ndc, 0.0, 1.0);
    vec4 farPoint = push.inverseProjectionView * vec4(ndc, 1.0, 1.0);
    vec3 rayOrigin = nearPoint.xyz / nearPoint.w;
    vec3 farWorld = farPoint.xyz / farPoint.w;
    vec3 rayDir = normalize(farWorld - rayOrigin);

    // 先截到最近的不透明表面。天空的 G-Buffer 位置是 camera + dir * 10000，
    // 因此这里也会自然得到“天空不截断”的行为。
    float sceneDistance = maxDistance;
    vec3 scenePos = texture(inPosition, inUV).xyz;
    if (dot(scenePos, scenePos) > 1.0e-6)
        sceneDistance = min(sceneDistance, length(scenePos - camera) - 0.5);

    const float base = push.params0.z;
    const float top = base + push.params0.w;
    float t0 = 0.0;
    float t1 = min(maxDistance, sceneDistance);

    if (abs(rayDir.y) < 1.0e-5)
    {
        if (rayOrigin.y < base || rayOrigin.y > top)
        {
            outColor = vec4(0.0);
            return;
        }
    }
    else
    {
        float ta = (base - rayOrigin.y) / rayDir.y;
        float tb = (top - rayOrigin.y) / rayDir.y;
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }

    if (t1 <= t0)
    {
        outColor = vec4(0.0);
        return;
    }

    int steps = clamp(int(push.params1.z), 1, 128);
    int lightSteps = clamp(int(push.params1.w), 1, 16);
    float stepSize = (t1 - t0) / float(steps);

    float jitter = interleavedGradientNoise(gl_FragCoord.xy + vec2(push.params2.x * 17.0, 0.0));
    float t = t0 + stepSize * jitter;

    vec3 sunDir = normalize(-ubo.lightDirection.xyz);
    vec3 sunColor = ubo.lightColor.rgb * ubo.lightColor.w;
    vec3 tint = push.color.rgb;
    float ambient = push.params2.z;
    float absorption = max(push.params1.y, 0.0);

    float phase = 0.55 + 0.45 * pow(max(dot(rayDir, sunDir), 0.0), 4.0);
    float lightStep = push.params0.w / float(lightSteps);

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < steps; ++i)
    {
        if (t >= t1)
            break;

        vec3 p = rayOrigin + rayDir * t;
        float density = sampleDensity(p);
        if (density > 1.0e-4)
        {
            // 朝太阳方向再做一次短步进，近似自阴影；被上方云挡住的底部会变暗。
            float opticalDepth = 0.0;
            for (int j = 0; j < lightSteps; ++j)
            {
                vec3 lp = p + sunDir * (lightStep * (float(j) + 0.5));
                opticalDepth += sampleDensity(lp) * lightStep;
            }
            float sunTransmittance = exp(-opticalDepth * absorption * 0.045);

            float h = clamp((p.y - base) / max(push.params0.w, 1.0), 0.0, 1.0);
            vec3 ambientColor = mix(vec3(0.30, 0.38, 0.55), vec3(0.72, 0.80, 0.92), h);
            vec3 stepColor = tint * (sunColor * sunTransmittance * phase * 0.85 +
                                     ambientColor * ambient * 1.1);
            float sigma = density * 0.045 * stepSize;
            float alpha = 1.0 - exp(-sigma);

            scattered += transmittance * alpha * stepColor;
            transmittance *= 1.0 - alpha;
            if (transmittance < 0.01)
                break;
        }

        t += stepSize;
    }

    // 预乘 alpha：RGB 已经乘过 transmittance，混合时不再乘 SRC_ALPHA。
    outColor = vec4(scattered, 1.0 - transmittance);
}
