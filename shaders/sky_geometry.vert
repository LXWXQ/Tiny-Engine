#version 450

// ---------------------------------------------------------------------------
// 3D 天空盒的顶点着色器。
//
// 与 skybox_mrt.vert 的区别：这里的 worldPos 完全由 push.modelMatrix 决定 ——
// 相机锁定与 1/16 放大已经在 CPU 侧算进 modelMatrix（scene::skyEntityMatrix），
// 着色器不再自己加 ubo.cameraPos。这样天空几何可以像普通实体一样被摆进场景、
// 参与插值，同时仍然是"无限远"的。
//
// 深度：gl_Position.z = w * 0.999999，等价于把整块天空压到远平面之前一点点，
// 既保证它盖住所有普通几何，又不会和远平面的清屏值打架。
// ---------------------------------------------------------------------------

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 uv;

layout(location = 0) out vec3 fragPosWorld;
layout(location = 1) out vec3 fragNormalWorld;
layout(location = 2) out vec3 fragColor;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
} ubo;

layout(push_constant) uniform Push {
    mat4 modelMatrix;
    mat4 normalMatrix;
} push;

void main() {
    vec4 world = push.modelMatrix * vec4(position, 1.0);
    fragPosWorld = world.xyz;
    fragNormalWorld = normalize(mat3(push.normalMatrix) * normal);
    fragColor = color;

    vec4 clip = ubo.projectionView * world;
    clip.z = clip.w * 0.999999;
    gl_Position = clip;
}
