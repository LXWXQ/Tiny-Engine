#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    mat4 modelView;
    mat4 projection;
    vec4 baseColor;
    vec4 lightDirView;
} push;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec2 fragUV;

void main()
{
    // 位置直接来自相机空间的偏移矩阵：viewmodel 不参与世界变换，
    // 所以贴墙时不会被墙体的深度裁掉。
    gl_Position = push.projection * push.modelView * vec4(inPosition, 1.0);
    fragNormal = normalize(mat3(push.modelView) * inNormal);
    fragColor = inColor * push.baseColor.rgb;
    fragUV = inUV;
}