#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 uv;
// 只要位置就够了，UV我们在片段着色器里用世界坐标动态算！

layout(location = 0) out vec3 outWorldPos;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec3 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(push_constant) uniform Push {
    mat4 modelMatrix;
    mat4 normalMatrix;
} push;

void main() {
    vec4 worldPos = push.modelMatrix * vec4(position, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = ubo.projectionView * worldPos;
}