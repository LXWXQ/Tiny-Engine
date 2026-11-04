#version 450

// V-01：玻璃前向通道。
// 顶点池在 GlassPass 里已经变换到世界空间（见 rebuildVertexPool），
// 所以这里没有 model 矩阵，也没有 push constant —— 只有投影。
layout(location = 0) in vec3 inPosition;
// 顶点色在体素网格里编码的是「面朝向明暗 x AO」，和不透明路径一致地乘到 albedo 上，
// 否则玻璃的转角会比旁边的石头亮一截。
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec3 fragPosWorld;
layout(location = 1) out vec3 fragNormalWorld;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec3 fragColor;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

void main()
{
    fragPosWorld = inPosition;
    fragNormalWorld = inNormal;
    fragUV = inUV;
    fragColor = inColor;
    gl_Position = ubo.projectionView * vec4(inPosition, 1.0);
}
