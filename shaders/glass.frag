#version 450
#extension GL_EXT_nonuniform_qualifier : require

// V-01：玻璃前向着色。
// 这里刻意不复用 lighting.frag 的完整 PBR/IBL 路径：那条路径依赖 G-Buffer
// 和光照 pass 的局部描述符（irradiance/prefiltered/BRDF LUT），玻璃只绑了
// 全局 set 与 bindless 材质 set。玻璃是"薄、亮、看穿"的观感，简化模型
// 足够，且不会与延迟光照的阴影/SSAO 结果打架。
layout(location = 0) in vec3 fragPosWorld;
layout(location = 1) in vec3 fragNormalWorld;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(set = 0, binding = 10) readonly buffer MaterialFactorBlock
{
    vec4 factors[];
} materialFactors;

layout(set = 1, binding = 0) uniform sampler2D albedoMaps[];

// 与 C++ 侧 GlassPushConstantData 精确对应（8 字节）。
layout(push_constant) uniform GlassPushConstant
{
    uint materialId;
    float alpha;
} push;

void main()
{
    const uint matId = push.materialId;

    // 与 mrt.frag 相同的体素图集解码：uv.x 里编码 tile 号（tile * 64 + 面内跨度）。
    const bool voxelAtlas = materialFactors.factors[matId * 2u + 1u].z > 0.5;
    vec2 albedoUV = fragUV;
    if (voxelAtlas)
    {
        const float tileId = floor(fragUV.x / 64.0);
        const vec2 local = fract(vec2(fragUV.x - tileId * 64.0, fragUV.y));
        albedoUV = (vec2(mod(tileId, 4.0), floor(tileId / 4.0)) + clamp(local, 0.02, 0.98)) * 0.25;
    }

    vec3 albedo = materialFactors.factors[matId * 2u].rgb *
                  texture(albedoMaps[nonuniformEXT(matId)], albedoUV).rgb * fragColor;
    // 玻璃本身是浅蓝白，再叠一层冷色 tint，避免和透明水晶混在一起看不出材质。
    albedo *= vec3(0.72, 0.88, 0.95);

    vec3 N = normalize(fragNormalWorld);
    vec3 V = normalize(ubo.cameraPos.xyz - fragPosWorld);
    // 玻璃双面可见：背面法线翻向观察者，否则背面会全黑。
    if (dot(N, V) < 0.0)
        N = -N;

    const vec3 L = normalize(-ubo.lightDirection.xyz);
    const vec3 radiance = ubo.lightColor.rgb * ubo.lightColor.w;
    const float NdotL = max(dot(N, L), 0.0);
    const float NdotV = clamp(dot(N, V), 0.0, 1.0);

    const vec3 H = normalize(L + V);
    const float specular = pow(max(dot(N, H), 0.0), 96.0) * 0.5;
    // 常量环境项：GlobalUbo.ambientLightColor 目前没有写入端（恒为 0），
    // 不能指望它；给一个很小的人工环境项，保证背光面不会纯黑。
    const vec3 ambient = vec3(0.14, 0.17, 0.20);

    const vec3 color = albedo * (ambient + radiance * NdotL * 0.6) + radiance * specular;

    // 菲涅尔：正对时更透（能看到后面的东西），掠射角更实（玻璃边缘发亮）。
    const float fresnel = pow(1.0 - NdotV, 3.0);
    const float alpha = clamp(push.alpha + fresnel * 0.35 + specular * 0.5, 0.0, 0.95);

    outColor = vec4(color, alpha);
}
