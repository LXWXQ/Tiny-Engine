#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube envCubeMap;

layout(push_constant) uniform Push {
    int faceIndex;
    float roughness;
    int sampleCount;
    float sourceFaceSize;   // 源立方体贴图单面边长，用于估计采样立体角
} push;

const float PI = 3.14159265359;

vec3 cubeFaceDirection(int face, vec2 ndc)
{
    float up = -ndc.y;
    if (face == 0) return vec3( 1.0, up,  -ndc.x);
    if (face == 1) return vec3(-1.0, up,   ndc.x);
    if (face == 2) return vec3(ndc.x, 1.0, up);
    if (face == 3) return vec3(ndc.x, -1.0, -up);
    if (face == 4) return vec3(ndc.x, up,  1.0);
    return               vec3(-ndc.x, up, -1.0);
}

// Hammersley 低差异序列：比纯随机收敛快，同样样本数下噪点明显更少
float radicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;   // / 2^32
}

vec2 hammersley(uint i, uint n)
{
    return vec2(float(i) / float(n), radicalInverseVdC(i));
}

vec3 importanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float distributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0000001);
}

vec3 prefilterEnvMap(float roughness, vec3 R)
{
    vec3 N = R;
    vec3 V = R;
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;

    uint sampleCount = uint(push.sampleCount);
    for (uint i = 0u; i < sampleCount; ++i)
    {
        vec2 Xi = hammersley(i, sampleCount);
        vec3 H = importanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);

            // 按「采样立体角 vs 源纹素立体角」估算该用哪一级 mip。
            // 不做这一步的话，高粗糙度（低分辨率 mip）会因为采样过密而闪烁。
            float D = distributionGGX(NdotH, roughness);
            float pdf = (D * NdotH / (4.0 * VdotH)) + 0.0001;

            float saTexel  = 4.0 * PI / (6.0 * push.sourceFaceSize * push.sourceFaceSize);
            float saSample = 1.0 / (float(sampleCount) * pdf + 0.0001);
            float mipLevel = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += textureLod(envCubeMap, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    return prefilteredColor / max(totalWeight, 0.0001);
}

void main()
{
    vec2 ndc = inUV * 2.0 - 1.0;
    vec3 N = normalize(cubeFaceDirection(push.faceIndex, ndc));
    outColor = vec4(prefilterEnvMap(push.roughness, N), 1.0);
}