#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragTint;
layout(location = 4) in float fragGradient;
layout(location = 5) in float fragPhase;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outPBR;

layout(push_constant) uniform Push {
    vec4 timeWind;
    vec4 grassColor;
    vec4 treeColor;
    vec4 trunkColor;
} push;

float hash11(float n)
{
    return fract(sin(n * 127.1 + 311.7) * 43758.5453);
}

void main()
{
    vec3 albedo;
    vec3 normal;

    if (push.timeWind.z < 0.5)
    {
        // 草：每张卡片横向切成 5 根叶片，按高度收窄、顶端随风弯曲。
        float cell = floor(fragUV.x * 5.0);
        float local = fract(fragUV.x * 5.0) - 0.5;
        float rnd = hash11(cell + floor(fragPhase * 91.0) * 7.0 + 1.0);
        float bladeHeight = 0.52 + 0.44 * rnd;
        float bend = (fragUV.y * fragUV.y) * (rnd - 0.5) * 0.55;
        float width = mix(0.24, 0.075, fragUV.y);
        float distanceToStem = abs(local + bend);
        float blade = 1.0 - smoothstep(width * 0.55, width, distanceToStem);
        blade *= step(fragUV.y, bladeHeight);

        // 叶片之间的空隙用 alpha test 丢掉，避免半透明排序。
        if (blade < 0.42)
            discard;

        float gradient = 0.52 + 0.56 * fragUV.y;
        albedo = fragTint * gradient * (0.88 + 0.22 * rnd);
        normal = normalize(mix(fragNormal, vec3(0.0, 1.0, 0.0), 0.62));
    }
    else
    {
        // 树：顶点色的 r 通道是树冠遮罩（0 树干，1 树冠）。
        if (fragGradient > 0.5)
        {
            albedo = fragTint * (0.78 + 0.26 * clamp(fragNormal.y, 0.0, 1.0));
            normal = normalize(mix(fragNormal, vec3(0.0, 1.0, 0.0), 0.18));
        }
        else
        {
            albedo = push.trunkColor.rgb * (0.74 + 0.26 * fragUV.y);
            normal = normalize(fragNormal);
        }
    }

    outPosition = vec4(fragWorldPos, 1.0);
    outNormal = vec4(normal, 1.0);
    outAlbedo = vec4(albedo, 1.0);

    float roughness = push.timeWind.z < 0.5 ? 0.88 : 0.82;
    outPBR = vec4(0.0, roughness, 0.0, 1.0);
}
