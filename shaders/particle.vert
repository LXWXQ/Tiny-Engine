#version 450

// F5：粒子 billboard。唯一顶点缓冲是每粒子实例数据（instance rate），
// 6 个顶点由 gl_VertexIndex 生成，不占几何缓冲。
layout(location = 0) in vec4 inPositionSize;   // xyz = 世界位置, w = 直径
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(push_constant) uniform ParticlePushConstant
{
    vec4 cameraRight;
    vec4 cameraUp;
} push;

void main()
{
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

    const vec2 corner = corners[gl_VertexIndex % 6];
    const vec3 world = inPositionSize.xyz +
        (push.cameraRight.xyz * corner.x + push.cameraUp.xyz * corner.y) *
        (inPositionSize.w * 0.5);

    gl_Position = ubo.projectionView * vec4(world, 1.0);
    fragUV = corner;
    fragColor = inColor;
}
