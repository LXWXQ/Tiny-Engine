#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 uv;

// 输出给片元着色器的 3D 向量
layout(location = 0) out vec3 outUVW;

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
    // 1. 直接将局部坐标作为采样向量传递
    outUVW = position;
    
    // 2. 取出 modelMatrix 的旋转和缩放，计算局部坐标
    mat3 modelRotScale = mat3(push.modelMatrix); 
    vec3 localPos = modelRotScale * position;
    
    // 3. 加上相机的世界坐标，实现“永远跟随摄像机”
    vec3 worldPos = localPos + ubo.cameraPos.xyz;
    vec4 pos = ubo.projectionView * vec4(worldPos, 1.0);
    
    // 4. Z = W 深度欺骗
    gl_Position = pos.xyww; 
}