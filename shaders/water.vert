#version 450

// 水面顶点着色：相机中心的径向网格 + 4 个大波位移。
// 顶点坐标是相对相机的局部 XZ，世界位置在 shader 里加 cameraPos.xz，
// 这样网格永远铺满相机周围直到地平线，不需要每帧重建缓冲。
layout(binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(push_constant) uniform WaterPush
{
    vec4 params;   // time, level, waveStrength, waveSpeed
    vec4 params2;  // farRadius, foamStrength, reserved, reserved
} push;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 fragWorldPos;

const vec4 WAVES[4] = vec4[4](
    vec4( 0.9239,  0.3827, 23.0, 0.180),
    vec4(-0.5736,  0.8192, 14.0, 0.100),
    vec4( 0.7071, -0.7071,  9.0, 0.055),
    vec4( 0.2588,  0.9659,  5.5, 0.025)
);

float bigWaveHeight(vec2 p, float t, float strength)
{
    float h = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float k = 6.28318530718 / WAVES[i].z;
        float omega = sqrt(9.81 * k) * 0.65;
        float phase = dot(WAVES[i].xy, p) * k - t * omega + float(i) * 1.7;
        h += WAVES[i].w * sin(phase);
    }
    return h * strength;
}

void main()
{
    const float t = push.params.x * push.params.w;
    const vec2 p = inPosition.xz + ubo.cameraPos.xz;
    const float h = bigWaveHeight(p, t, push.params.z);

    vec3 world = vec3(inPosition.x + ubo.cameraPos.x,
                      push.params.y + h,
                      inPosition.z + ubo.cameraPos.z);
    fragWorldPos = world;
    gl_Position = ubo.projectionView * vec4(world, 1.0);
}
