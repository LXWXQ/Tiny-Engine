#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragPosWorld;
layout(location = 2) out vec3 fragNormalWorld;
layout(location = 3) out vec2 fragUV;
// 局部空间坐标：占位材质的棋盘格用它，保证六个面尺度一致、且跟着物体旋转。
// 用局部坐标而不是世界坐标，是因为世界坐标的格子会在物体移动时"滑动"。
layout(location = 4) out vec3 fragPosLocal;
// 局部空间法线：占位棋盘格的三平面投影按它选轴/混合。
// 必须用 local normal（不是 world normal）：物体旋转时棋盘格应该"贴在物体上"，
// 而不是跟着世界轴换投影方向。
layout(location = 5) out vec3 fragNormalLocal;
// B-03: GPU-selected indirect draws encode per-command LOD blend/mode in
// firstInstance; pass it flat to the fragment dither path.
layout(location = 6) flat out float fragTerrainLodBlend;
layout(location = 7) flat out uint fragTerrainLodBlendMode;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec3 lightDirection;
    vec4 lightColor;
    vec4 cameraPos; 
} ubo;

layout(push_constant) uniform Push 
{
    mat4 modelMatrix;
    mat4 normalMatrix;
    // D-3 bindless：材质下标 + 有效性。与 C++ 侧 SimplePushConstantData 的
    // 尾部两个 uint32 精确对应（std430 下 uvec2 = 8 字节、对齐 8）。
    // 顶点着色器不读它，但必须在这里声明出来 —— push_constant 块的大小是
    // 由所有 stage 里最大的那个声明决定的；少声明会导致片元这边读越界。
    uvec2 material;
    float alphaCutoff;
    uint alphaMasked;
    uint albedoFallback;
    float checkerScale;
    vec4 checkerUvTransform;
    uint terrainMode;
    float terrainMinY;
    float terrainHeightRange;
    float terrainDetailStrength;
    float terrainUvTiling;
    uint terrainMaterialStyle;
    float terrainLodBlend;
    uint terrainLodBlendMode;
} push;

void main() 
{
    vec4 positionWorld = push.modelMatrix * vec4(inPosition, 1.0);
    gl_Position = ubo.projectionView * positionWorld;
    fragNormalWorld = normalize(mat3(push.normalMatrix) * inNormal);
    fragPosWorld = positionWorld.xyz;
    fragColor = inColor;
    fragUV = inUV;
    fragPosLocal = inPosition;
    fragNormalLocal = inNormal;
}