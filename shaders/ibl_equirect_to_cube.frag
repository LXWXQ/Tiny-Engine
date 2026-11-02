#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D equirectMap;

layout(push_constant) uniform Push {
    int faceIndex;
    float roughness;
    int sampleCount;
    float sourceFaceSize;
} push;

const float INV_TWO_PI = 0.15915494309;   // 1 / (2*PI)
const float INV_PI     = 0.31830988618;   // 1 / PI

vec2 sampleSphericalMap(vec3 v)
{
    // 等距柱状投影。本工程的 HDR 顶部是天顶（已用脚本验证：图像上三分之一最亮），
    // 所以 v.y = +1 必须映射到 uv.y = 0（图像顶行）。
    return vec2(atan(v.z, v.x) * INV_TWO_PI + 0.5,
                0.5 - asin(clamp(v.y, -1.0, 1.0)) * INV_PI);
}

vec3 cubeFaceDirection(int face, vec2 ndc)
{
    // ndc.x: -1(左) .. +1(右)
    // ndc.y: -1(上) .. +1(下)   —— Vulkan 的 NDC 是 y 向下的
    float up = -ndc.y;   // 转成「向上为正」，与立方体贴图约定对齐
    if (face == 0) return vec3( 1.0, up,  -ndc.x);  // +X
    if (face == 1) return vec3(-1.0, up,   ndc.x);  // -X
    if (face == 2) return vec3(ndc.x, 1.0, up);     // +Y
    if (face == 3) return vec3(ndc.x, -1.0, -up);   // -Y
    if (face == 4) return vec3(ndc.x, up,  1.0);    // +Z
    return               vec3(-ndc.x, up, -1.0);    // -Z
}

void main()
{
    vec2 ndc = inUV * 2.0 - 1.0;
    vec3 dir = normalize(cubeFaceDirection(push.faceIndex, ndc));
    outColor = vec4(texture(equirectMap, sampleSphericalMap(dir)).rgb, 1.0);
}