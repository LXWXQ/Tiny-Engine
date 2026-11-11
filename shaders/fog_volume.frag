#version 450

// 局部体积雾：全屏光线步进若干旋转盒体。
//
// 每条射线先读 G-Buffer 世界坐标，把步进截到最近的不透明表面之前；然后对
// 每个盒体做 slab 相交，在盒内按高度衰减、边缘淡出、3D 噪声和 HG 相函数
// 累积散射。输出预乘 alpha，混合方程见 FogVolumePass::createPipeline。
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

struct FogVolume
{
    mat4 world;
    mat4 inverseWorld;
    vec4 params0;   // x=density y=heightFalloff z=noiseStrength w=noiseScale
    vec4 params1;   // x=centerY y=edgeSoftness z=anisotropy w=ambient
    vec4 color;     // rgb=散射色调 a=steps
    vec4 wind;      // xyz=风速（米/秒）
};

layout(set = 1, binding = 0) uniform FogVolumeBlock
{
    vec4 header;    // x=count
    FogVolume volumes[8];
} fog;

layout(set = 1, binding = 1) uniform sampler3D uNoise;
layout(set = 1, binding = 2) uniform sampler2D inPosition;

layout(push_constant) uniform FogPush
{
    mat4 inverseProjectionView;
    vec4 params;    // x=time y=maxDistance
} push;

const float PI = 3.14159265359;

// 交错梯度噪声：给每个盒体的步进起点一个像素级偏移，避免层状条带。
float interleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float henyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1.0e-4);
    return (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));
}

void main()
{
    int count = int(fog.header.x + 0.5);
    if (count <= 0)
    {
        outColor = vec4(0.0);
        return;
    }

    vec3 camera = ubo.cameraPos.xyz;
    vec3 scenePos = texture(inPosition, inUV).xyz;
    float sceneDistance = max(push.params.y, 1.0);
    vec3 rayDir;

    if (dot(scenePos, scenePos) > 1.0e-6)
    {
        vec3 delta = scenePos - camera;
        float distanceToScene = length(delta);
        rayDir = delta / max(distanceToScene, 1.0e-5);
        sceneDistance = min(sceneDistance, max(distanceToScene - 0.05, 0.0));
    }
    else
    {
        // G-Buffer 没有有效位置（例如空场景/天空）：用逆 VP 还原远平面方向。
        vec2 ndc = inUV * 2.0 - 1.0;
        vec4 farPoint = push.inverseProjectionView * vec4(ndc, 1.0, 1.0);
        rayDir = normalize(farPoint.xyz / farPoint.w - camera);
    }

    if (sceneDistance <= 0.0)
    {
        outColor = vec4(0.0);
        return;
    }

    vec3 sunDir = normalize(-ubo.lightDirection.xyz);
    vec3 sunColor = ubo.lightColor.rgb * ubo.lightColor.w;
    float time = push.params.x;

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;

    for (int v = 0; v < 8; ++v)
    {
        if (v >= count)
            break;

        FogVolume volume = fog.volumes[v];
        float densityScale = volume.params0.x;
        if (densityScale <= 1.0e-5)
            continue;

        // 世界射线变换到盒体局部空间。inverseWorld 含缩放，所以参数 t 仍然
        // 对应世界距离，不需要再归一化方向。
        vec3 localOrigin = (volume.inverseWorld * vec4(camera, 1.0)).xyz;
        vec3 localDir = (volume.inverseWorld * vec4(rayDir, 0.0)).xyz;
        if (abs(localDir.x) < 1.0e-6) localDir.x = 1.0e-6;
        if (abs(localDir.y) < 1.0e-6) localDir.y = 1.0e-6;
        if (abs(localDir.z) < 1.0e-6) localDir.z = 1.0e-6;

        vec3 invDir = 1.0 / localDir;
        vec3 tA = (-0.5 - localOrigin) * invDir;
        vec3 tB = ( 0.5 - localOrigin) * invDir;
        vec3 tMin = min(tA, tB);
        vec3 tMax = max(tA, tB);

        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        tNear = max(tNear, 0.0);
        tFar = min(tFar, sceneDistance);
        if (tFar <= tNear)
            continue;

        int steps = clamp(int(volume.color.a + 0.5), 1, 32);
        float stepSize = (tFar - tNear) / float(steps);

        float jitter = interleavedGradientNoise(gl_FragCoord.xy + vec2(float(v) * 41.0, time * 23.0));
        float t = tNear + stepSize * jitter;

        float phase = mix(0.7, henyeyGreenstein(dot(rayDir, sunDir),
                                                clamp(volume.params1.z, -0.9, 0.9)) * 4.0 * PI, 0.55);
        float ambient = volume.params1.w;
        float edgeSoftness = max(volume.params1.y, 0.001);
        float heightFalloff = max(volume.params0.y, 0.0);
        float centerY = volume.params1.x;
        float noiseScale = max(volume.params0.w, 0.0001);
        vec3 noiseOffset = volume.wind.xyz * time * noiseScale;

        for (int i = 0; i < 32; ++i)
        {
            if (i >= steps || t >= tFar)
                break;

            vec3 p = camera + rayDir * t;
            vec3 localP = (volume.inverseWorld * vec4(p, 1.0)).xyz;

            // 盒体边缘淡出，避免看到硬切的立方体边界。
            float edge = min(min(0.5 - abs(localP.x), 0.5 - abs(localP.y)),
                             0.5 - abs(localP.z));
            if (edge > 0.0)
            {
                float edgeMask = smoothstep(0.0, edgeSoftness, edge);
                float heightMask = exp(-heightFalloff * max(p.y - centerY, 0.0));

                vec4 n = texture(uNoise, p * noiseScale + noiseOffset);
                float nv = n.r * 0.70 + n.g * 0.30;
                float wisps = smoothstep(0.22, 0.78, nv);
                float shape = mix(1.0, 0.15 + 1.35 * wisps, volume.params0.z);
                shape *= mix(1.0, 0.60 + 0.80 * n.b, 0.25);

                float density = densityScale * heightMask * edgeMask * shape;
                float sigma = density * stepSize;
                if (sigma > 1.0e-5)
                {
                    float alpha = 1.0 - exp(-sigma);
                    float localHeight = clamp(localP.y + 0.5, 0.0, 1.0);
                    vec3 ambientColor = mix(vec3(0.20, 0.26, 0.36),
                                            vec3(0.58, 0.67, 0.82), localHeight);
                    vec3 stepColor = volume.color.rgb *
                        (sunColor * phase * 0.55 + ambientColor * ambient * 0.90);

                    scattered += transmittance * alpha * stepColor;
                    transmittance *= 1.0 - alpha;
                    if (transmittance < 0.02)
                        break;
                }
            }

            t += stepSize;
        }

        if (transmittance < 0.02)
            break;
    }

    outColor = vec4(scattered, 1.0 - transmittance);
}
