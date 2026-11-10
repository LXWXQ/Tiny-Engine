#version 450

// F6-06：太阳盘 + 镜头光晕（全屏后效）。
//
// 日盘由世界空间射线与 sunDir 的夹角决定，因此不随屏幕纵横比拉伸；
// 鬼影是屏幕空间伪影，沿“太阳 -> 中心”的连线镜像，不参与逐像素遮挡判定，
// 只受太阳整体可见度调制。所有输出都是加色，混合方程见 SunFlarePass。
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

layout(set = 1, binding = 0) uniform sampler2D uDepth;

layout(push_constant) uniform SunPush
{
    mat4 inverseProjectionView;
    vec4 sunDirectionRadius; // xyz=指向太阳 w=角半径(弧度)
    vec4 params0;            // x=intensity y=glow z=flareIntensity w=occlusion
    vec4 params1;            // x=ghostCount y=aspect z=sunUV.x w=sunUV.y
    vec4 color;              // rgb=色调 a=太阳在相机前方
} push;

const float kSkyDepth = 0.9999;

float isSky(vec2 uv)
{
    if (push.params0.w < 0.5)
        return 1.0;
    return step(kSkyDepth, texture(uDepth, clamp(uv, vec2(0.0), vec2(1.0))).r);
}

// 太阳中心附近的整体可见度。日盘逐像素用 isSky 判定；鬼影是镜头伪影，
// 这里用 5 个点判断“太阳是不是被山/建筑挡住了”，避免山后还在闪鬼影。
float sunVisibility(vec2 uv)
{
    if (push.params0.w < 0.5)
        return 1.0;
    if (any(lessThan(uv, vec2(0.001))) || any(greaterThan(uv, vec2(0.999))))
        return 1.0;

    const vec2 o = vec2(0.006, 0.004);
    float sum = isSky(uv);
    sum += isSky(uv + vec2(o.x, 0.0));
    sum += isSky(uv - vec2(o.x, 0.0));
    sum += isSky(uv + vec2(0.0, o.y));
    sum += isSky(uv - vec2(0.0, o.y));
    return sum * 0.2;
}

// 近似六边形距离：|x|*cos30 + |y|*0.5 与 |y| 的较大值，边缘略带圆角，
// 比真正的 SDF 便宜，远看就是镜头鬼影的六边形光斑。
float hexDistance(vec2 p)
{
    return max(abs(p.x) * 0.8660254 + abs(p.y) * 0.5, abs(p.y));
}

void main()
{
    if (push.color.a < 0.5)
    {
        outColor = vec4(0.0);
        return;
    }

    vec2 ndc = inUV * 2.0 - 1.0;
    vec4 nearPoint = push.inverseProjectionView * vec4(ndc, 0.0, 1.0);
    vec4 farPoint = push.inverseProjectionView * vec4(ndc, 1.0, 1.0);
    vec3 rayOrigin = nearPoint.xyz / nearPoint.w;
    vec3 rayDir = normalize(farPoint.xyz / farPoint.w - rayOrigin);

    vec3 sunDir = normalize(push.sunDirectionRadius.xyz);
    float angularRadius = max(push.sunDirectionRadius.w, 0.0001);
    float cosAngle = clamp(dot(rayDir, sunDir), -1.0, 1.0);

    // 用角度而不是 dot 做衰减：日盘角半径只有零点几度，dot 域里几乎全是
    // 0.9999，直接 pow 会让光晕铺满整个天空。
    float theta = acos(cosAngle);

    float disc = 1.0 - smoothstep(angularRadius * 0.86, angularRadius * 1.06, theta);

    // 两段日冕：紧贴盘面的亮边 + 更宽的大气散射。
    float glow = push.params0.y *
        (0.65 * exp(-max(theta - angularRadius, 0.0) / (angularRadius * 0.85)) +
         0.35 * exp(-theta / (angularRadius * 5.0)));

    float sky = isSky(inUV);
    vec3 radiance = push.color.rgb * push.params0.x * (disc * 18.0 + glow * 0.8) * sky;

    // ---- 镜头光晕（屏幕空间）----
    float flare = push.params0.z;
    if (flare > 1.0e-4)
    {
        vec2 sunUV = vec2(push.params1.z, push.params1.w);
        float visibility = sunVisibility(sunUV);
        float aspect = max(push.params1.y, 0.1);
        int ghosts = clamp(int(push.params1.x), 0, 8);

        vec3 ghostColor = vec3(0.0);
        for (int i = 0; i < 8; ++i)
        {
            if (i >= ghosts)
                break;

            // 0.35 起步，逐个向屏幕中心镜像并穿过中心；k>1 的鬼影落在
            // 太阳对面，正是广角镜头里最常见的那条光斑链。
            float k = 0.35 + 0.42 * float(i);
            vec2 center = sunUV + (vec2(0.5) - sunUV) * k;
            vec2 p = inUV - center;
            p.x *= aspect;

            float radius = 0.016 + 0.010 * float(i % 3);
            float hex = hexDistance(p);
            // 略微填充中心、边缘再提亮：纯边缘环看起来像 UI 描边，
            // 实心 + 亮边的光斑更接近镀膜镜头的散景。
            float body = 1.0 - smoothstep(radius * 0.60, radius, hex);
            float rim = mix(0.45, 1.0, smoothstep(radius * 0.25, radius * 0.90, hex));

            // 轻微色散：奇偶鬼影在冷/暖之间摆动，模拟镀膜镜头的偏色。
            float hue = fract(float(i) * 0.37);
            vec3 tint = mix(vec3(0.55, 0.78, 1.0), vec3(1.0, 0.62, 0.35), hue);
            vec3 tinted = mix(push.color.rgb, tint, 0.55);
            ghostColor += tinted * body * rim;
        }

        // 横向光条：只在太阳所在高度附近，向两侧快速衰减。
        float streak = exp(-abs(inUV.y - vec2(push.params1.z, push.params1.w).y) * 260.0) *
                       exp(-abs(inUV.x - vec2(push.params1.z, push.params1.w).x) * 3.0);
        vec3 streakColor = mix(push.color.rgb, vec3(0.65, 0.82, 1.0), 0.25);

        // 日盘周围的彩色细环，强度低，只用来破掉“纯白圆点”的塑料感。
        float ring = exp(-abs(theta - angularRadius * 2.6) / max(angularRadius * 0.35, 0.0001));

        radiance += push.color.rgb * push.params0.x * flare * 0.05 * ring * sky;
        radiance += ghostColor * flare * visibility * 0.22;
        radiance += streakColor * flare * visibility * 0.14 * streak;
    }

    outColor = vec4(radiance, 1.0);
}
