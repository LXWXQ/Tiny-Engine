#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube envCubeMap;

layout(push_constant) uniform Push {
    int faceIndex;
    float roughness;
    int sampleCount;
    float sourceFaceSize;
} push;

const float PI = 3.14159265359;

vec3 cubeFaceDirection(int face, vec2 ndc)
{
    float up = -ndc.y;
    if (face == 0) return vec3( 1.0, up,  -ndc.x);
    if (face == 1) return vec3(-1.0, up,   ndc.x);
    if (face == 2) return vec3(ndc.x, 1.0, up);
    if (face == 3) return vec3(ndc.x, -1.0, -up);
    if (face == 4) return vec3(ndc.x, up,  1.0);
    return               vec3(-ndc.x, up, -1.0);
}

// 把半球上的入射辐照度做余弦加权积分（Lambert 漫反射的 irradiance 项）。
// 结果等价于对天空做一次非常粗糙的模糊，直接作为漫反射 IBL 使用。
void main()
{
    vec2 ndc = inUV * 2.0 - 1.0;
    vec3 N = normalize(cubeFaceDirection(push.faceIndex, ndc));

    // 以 N 为轴构造切空间
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    vec3 irradiance = vec3(0.0);
    float sampleCount = 0.0;
    const float sampleDelta = 0.025;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            vec3 tangentSample = vec3(sin(theta) * cos(phi),
                                      sin(theta) * sin(phi),
                                      cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += texture(envCubeMap, sampleVec).rgb * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }

    irradiance = PI * irradiance / sampleCount;
    outColor = vec4(irradiance, 1.0);
}