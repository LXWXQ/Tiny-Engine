#version 450

// Depth-only vegetation shadow vertex shader. It uses the same instance
// transform and wind formula as vegetation.vert, but writes only position into
// the cascade shadow map. The tree trunk mask (color.r) keeps trunks rigid.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 4) in vec4 instancePositionScale;
layout(location = 5) in vec4 instanceRotationTint;

layout(push_constant) uniform Push
{
    mat4 lightViewProjection;
    vec4 timeWind;   // x=time, y=wind strength
} push;

void main()
{
    float yaw = instanceRotationTint.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = inPosition * vec3(instanceRotationTint.y, 1.0, instanceRotationTint.y) *
                 instancePositionScale.w;

    vec3 rotated;
    rotated.x = c * local.x - s * local.z;
    rotated.y = local.y;
    rotated.z = s * local.x + c * local.z;

    vec3 world = rotated + instancePositionScale.xyz;

    float heightT = clamp(inPosition.y, 0.0, 1.0);
    float weight = heightT * heightT * max(inColor.r, 0.0);
    float phase = instanceRotationTint.z;
    float sway = sin(push.timeWind.x * 1.7 + phase * 6.2831853 +
                     world.x * 0.35 + world.z * 0.27);
    world.x += sway * push.timeWind.y * weight * 0.18 * 1.45;
    world.z += cos(push.timeWind.x * 1.3 + phase * 4.7 + world.x * 0.21) *
               push.timeWind.y * weight * 0.08;

    gl_Position = push.lightViewProjection * vec4(world, 1.0);
}
