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

layout(set = 0, binding = 1) uniform sampler2D environmentMap;

layout(set = 0, binding = 10) readonly buffer MaterialFactorBlock
{
    vec4 factors[];
} materialFactors;

layout(set = 1, binding = 0) uniform sampler2D albedoMaps[];

// 与 C++ 侧 GlassPushConstantData 精确对应（32 字节）。
layout(push_constant) uniform GlassPushConstant
{
    uint materialId;
    float alpha;
    vec4 fogParams;   // x = 起始距离, y = 密度, z = 保留, w = 启用
} push;

const vec2 kInvAtan = vec2(0.1591, 0.3183);

vec2 sampleSphericalMap(vec3 v)
{
    return vec2(atan(v.z, v.x) * kInvAtan.x + 0.5,
                0.5 - asin(clamp(v.y, -1.0, 1.0)) * kInvAtan.y);
}

void main()
{
    const uint matId = push.materialId;

    // 与 mrt.frag 相同的体素图集解码：uv.x 里编码 tile 号（tile * 64 + 面内跨度）。
    const bool voxelAtlas = materialFactors.factors[matId * 2u + 1u].z > 0.5;
    vec2 albedoUV = fragUV;
    vec2 albedoGradX = dFdx(fragUV);
    vec2 albedoGradY = dFdy(fragUV);
    if (voxelAtlas)
    {
        // 4x4 格，每格 64px 内容 + 32px edge-clamp gutter，128px 间距，512 总尺寸。
        const float kAtlasSize = 512.0;
        const float kPitch = 128.0;
        const float kPad = 32.0;
        const float kTile = 64.0;
        const float tileId = floor(fragUV.x / kTile);
        const vec2 local = fract(vec2(fragUV.x - tileId * kTile, fragUV.y));
        const vec2 cellOrigin = vec2(mod(tileId, 4.0), floor(tileId / 4.0)) * kPitch + kPad;
        albedoUV = (cellOrigin + clamp(local, 0.5 / kTile, 1.0 - 0.5 / kTile) * kTile) / kAtlasSize;
        // 同 mrt.frag：fract() 的导数在格边界跳变，显式梯度避免误选最粗 mip；
        // gutter 保证高 mip 只采到本格边缘，不会渗到相邻格。
        albedoGradX *= kTile / kAtlasSize;
        albedoGradY *= kTile / kAtlasSize;
    }

    vec3 albedo = materialFactors.factors[matId * 2u].rgb *
                  textureGrad(albedoMaps[nonuniformEXT(matId)], albedoUV, albedoGradX, albedoGradY).rgb * fragColor;
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

    vec3 color = albedo * (ambient + radiance * NdotL * 0.6) + radiance * specular;

    // B：和地形/水面一致的距离雾。玻璃本身是半透明，这里只淡出它的
    // 着色颜色；alpha 不改，避免远处玻璃突然变成一块实心雾墙。
    if (push.fogParams.w > 0.5)
    {
        float fogDistance = length(ubo.cameraPos.xyz - fragPosWorld);
        float fogAmount = 1.0 - exp(-push.fogParams.y * max(fogDistance - push.fogParams.x, 0.0));
        vec3 fogColor = textureLod(environmentMap,
            sampleSphericalMap(normalize(vec3(-V.x, 0.035, -V.z))), 3.0).rgb;
        color = mix(color, fogColor, clamp(fogAmount, 0.0, 1.0));
    }

    // 菲涅尔：正对时更透（能看到后面的东西），掠射角更实（玻璃边缘发亮）。
    const float fresnel = pow(1.0 - NdotV, 3.0);
    const float alpha = clamp(push.alpha + fresnel * 0.35 + specular * 0.5, 0.0, 0.95);

    outColor = vec4(color, alpha);
}
