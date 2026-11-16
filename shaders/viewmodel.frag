#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 modelView;
    mat4 projection;
    vec4 baseColor;
    vec4 lightDirView;
} push;

void main()
{
    const vec3 n = normalize(fragNormal);
    const vec3 lightDir = normalize(push.lightDirView.xyz);
    const float ndl = max(dot(n, lightDir), 0.0);

    // 固定的环境项 + 跟随场景平行光的漫反射：手持物不需要 PBR，
    // 但也不能是完全平的一片颜色，否则贴墙时看不出朝向。
    const vec3 ambient = vec3(0.26, 0.28, 0.34);
    const vec3 diffuse = vec3(1.0, 0.96, 0.90) * ndl * push.lightDirView.w;
    vec3 color = fragColor * (ambient + diffuse);

    // 轻微边缘压暗，让方块在强光下也有轮廓。
    const float rim = smoothstep(0.0, 1.0, 1.0 - abs(n.z));
    color *= 1.0 - 0.15 * rim;

    outColor = vec4(color, 1.0);
}