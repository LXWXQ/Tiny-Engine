#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;

// 实例缓冲（binding 1）：3 个 vec4。
layout(location = 4) in vec4 instancePositionScale;
layout(location = 5) in vec4 instanceRotationTint;
layout(location = 6) in vec4 instanceColor;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec3 fragTint;
layout(location = 4) out float fragGradient;
layout(location = 5) out float fragPhase;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(push_constant) uniform Push {
    vec4 timeWind;   // x=time, y=wind strength, z=mode (0 grass, 1 tree)
    vec4 grassColor;
    vec4 treeColor;
    vec4 trunkColor;
} push;

void main()
{
    // 每实例：positionScale.xyz 是世界基点，.w 是高度缩放；
    // rotationTint.x 是偏航，.y 是宽度缩放，.z 是风相位，.w 是颜色抖动。
    float yaw = instanceRotationTint.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = inPosition * vec3(instanceRotationTint.y, 1.0, instanceRotationTint.y) *
                 instancePositionScale.w;

    // 只旋转 XZ，Y 保持竖直。
    vec3 rotated;
    rotated.x = c * local.x - s * local.z;
    rotated.y = local.y;
    rotated.z = s * local.x + c * local.z;

    vec3 world = rotated + instancePositionScale.xyz;

    // 草整簇摆动；树只有树冠摆，树干几乎不动。
    float heightT = clamp(inPosition.y, 0.0, 1.0);
    float weight = heightT * heightT;
    if (push.timeWind.z > 0.5)
        weight *= max(inColor.r, 0.15);
    float phase = instanceRotationTint.z;
    float sway = sin(push.timeWind.x * 1.7 + phase * 6.2831853 +
                     world.x * 0.35 + world.z * 0.27);
    world.x += sway * push.timeWind.y * weight * 0.18 *
               (push.timeWind.z > 0.5 ? 1.6 : 1.0);
    world.z += cos(push.timeWind.x * 1.3 + phase * 4.7 + world.x * 0.21) *
               push.timeWind.y * weight * 0.08;

    vec3 n;
    n.x = c * inNormal.x - s * inNormal.z;
    n.y = inNormal.y;
    n.z = s * inNormal.x + c * inNormal.z;

    fragWorldPos = world;
    fragNormal = normalize(n);
    fragUV = inUV;
    fragTint = instanceColor.rgb;
    fragGradient = inColor.r;
    fragPhase = phase;

    gl_Position = ubo.projectionView * vec4(world, 1.0);
}
