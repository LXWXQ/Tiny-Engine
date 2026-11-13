#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor; 

struct PointLight 
{
    vec4 position;
    vec4 color;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
    int numLights;
    float exposure;
    PointLight pointLights[100];
    // F6：阴影数据接在 UBO 尾部 —— 顺序必须和 C++ 的 GlobalUbo 一致。
    mat4 previousProjectionView;
    mat4 shadowMatrices[3];
    vec4 shadowCascadeSplits;   // xyz = 每级最远视深度，w = 级联数
    vec4 shadowParams;          // x = 1/分辨率, y = 强度, z = PCF 半径, w = 启用
    vec4 fogParams;             // x = 起始距离, y = 密度, z = 保留, w = 启用
    vec4 shadowFilterParams;    // x = 0 PCF / 1 PCSS, yzw 保留
} ubo;

layout(set = 0, binding = 1) uniform sampler2D environmentMap;

layout(set = 0, binding = 2) uniform sampler2D inPosition;
layout(set = 0, binding = 3) uniform sampler2D inNormal;
layout(set = 0, binding = 4) uniform sampler2D inAlbedo;
layout(set = 0, binding = 5) uniform sampler2D inPbr;
layout(set = 0, binding = 6) uniform sampler2D inSSAO;

// IBL 三件套（由 IBLBaker 在初始化阶段预烘焙）
// F6：3 级联阴影图（每级一个 layer）。
layout(set = 0, binding = 11) uniform sampler2DArray shadowMaps;

layout(set = 0, binding = 7) uniform samplerCube irradianceMap;   // 漫反射：余弦卷积
layout(set = 0, binding = 8) uniform samplerCube prefilteredMap;  // 镜面：GGX 预过滤 + mip 链
layout(set = 0, binding = 9) uniform sampler2D   brdfLut;         // split-sum 的 BRDF 积分表

const float PI = 3.14159265359;
const vec2 invAtan = vec2(0.1591, 0.3183);

vec2 SampleSphericalMap(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(clamp(v.y, -1.0, 1.0)));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

// B：雾色取环境图地平线方向（而不是硬编码颜色），远山淡出后与天空一致。
// 固定 LOD 3 做一次模糊，避免把太阳附近的细节拉成一条亮带。
vec3 SampleFogColor(vec3 V) {
    vec3 dir = normalize(vec3(-V.x, 0.035, -V.z));
    return textureLod(environmentMap, SampleSphericalMap(dir), 3.0).rgb;
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return num / max(denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return num / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 粗糙度感知版 Fresnel：粗糙表面的镜面反射不该有强烈的边缘亮边
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 CalcLightBRDF(vec3 L, vec3 V, vec3 N, vec3 albedo, float roughness, float metallic, vec3 F0, vec3 radiance) {
    vec3 H = normalize(V + L);
    float NDF = DistributionGGX(N, H, roughness);   
    float G   = GeometrySmith(N, V, L, roughness);      
    vec3 F    = fresnelSchlick(max(dot(H, V), 0.0), F0);        
    vec3 numerator    = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular     = numerator / denominator;
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    float NdotL = max(dot(N, L), 0.0);        
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

// 深度比较：返回 1 表示受光，0 表示被遮挡。
float ShadowCompare(vec2 uv, uint cascade, float compare) {
    float stored = texture(shadowMaps, vec3(uv, cascade)).r;
    return compare <= stored ? 1.0 : 0.0;
}

// 固定核 PCF：5x5 双线性采样。radiusTexels 控制核半径，
// 1.5 对应约 3 个纹素的覆盖范围。
float SampleShadowPCF(vec2 uv, uint cascade, float compare, float radiusTexels) {
    float radius = max(radiusTexels, 1.0);
    vec2 texel = vec2(ubo.shadowParams.x) * radius * 0.5;
    float lit = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            lit += ShadowCompare(uv + vec2(x, y) * texel, cascade, compare);
        }
    }
    return lit / 25.0;
}

// PCSS：先在较大范围内找平均遮挡深度，再按「接收者深度 - 遮挡者深度」
// 估计半影宽度，最后用可变半径 PCF 采样。接触点附近 penumbra 很小，
// 阴影边缘硬；离遮挡物越远，核越大、边缘越软。
float SampleShadowPCSS(vec2 uv, uint cascade, float compare, float radiusTexels) {
    const float searchRadius = max(radiusTexels, 1.0) * 2.0;
    const vec2 searchStep = vec2(ubo.shadowParams.x) * searchRadius * 0.5;
    float blockerDepth = 0.0;
    int blockerCount = 0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float stored = texture(shadowMaps,
                vec3(uv + vec2(x, y) * searchStep, cascade)).r;
            if (stored < compare) {
                blockerDepth += stored;
                ++blockerCount;
            }
        }
    }
    if (blockerCount == 0)
        return 1.0;

    const float averageBlocker = blockerDepth / float(blockerCount);
    // 正交阴影图的深度是 0..1；300 把常见的地形/树木深度差映射到 1..12 纹素。
    const float penumbra = clamp((compare - averageBlocker) * 300.0, 1.0, 12.0);
    return SampleShadowPCF(uv, cascade, compare, penumbra);
}

// 选级 + 阴影滤波。自己比较深度而不是用比较采样器：比较采样器会让 PCF 的
// 软化程度无法和强度参数解耦，而且还要多一套 sampler 状态。
float SampleShadow(vec3 fragPosWorld, float viewDepth, vec3 N, float NdotL) {
    if (ubo.shadowParams.w < 0.5)
        return 1.0;

    uint cascade = 0u;
    if (viewDepth <= ubo.shadowCascadeSplits.x)
        cascade = 0u;
    else if (viewDepth <= ubo.shadowCascadeSplits.y)
        cascade = 1u;
    else if (viewDepth <= ubo.shadowCascadeSplits.z)
        cascade = 2u;
    else
        return 1.0;   // 超出最后一级：不投影，直接受光

    // 法线偏移（世界空间）：沿法线抬起半个纹素对应的世界距离，压住自遮蔽。
    vec3 samplePos = fragPosWorld + N * 0.05;
    vec4 lightSpace = ubo.shadowMatrices[cascade] * vec4(samplePos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w;
    proj.xy = proj.xy * 0.5 + 0.5;   // Vulkan 的 z 已经是 [0,1]，只有 xy 要重映射
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return 1.0;

    // 斜射面需要更大的偏移：N·L 越小，同一深度差对应的世界距离越长。
    // 斜率放大 + 常量兜底：正交阴影图覆盖 2r 的深度范围，2048 纹素下
    // 1 纹素≈0.015 世界单位，bias 太小会让大面积平面自己遮自己。
    float bias = max(0.0008, 0.0030 * (1.0 - NdotL));
    float compare = proj.z - bias;

    if (ubo.shadowFilterParams.x > 0.5)
        return SampleShadowPCSS(proj.xy, cascade, compare, ubo.shadowParams.z);
    return SampleShadowPCF(proj.xy, cascade, compare, ubo.shadowParams.z);
}

vec3 CalcDirectionalLight(vec3 N, vec3 V, vec3 albedo, float roughness, float metallic, vec3 F0,
                          vec3 fragPosWorld, float viewDepth) {
    vec3 L = normalize(-ubo.lightDirection.xyz);
    vec3 radiance = ubo.lightColor.xyz * ubo.lightColor.w;
    vec3 direct = CalcLightBRDF(L, V, N, albedo, roughness, metallic, F0, radiance);
    float NdotL = max(dot(N, L), 0.0);
    float shadow = SampleShadow(fragPosWorld, viewDepth, N, NdotL);
    // 强度做 mix 而不是乘法：0 强度等于完全不加暗，而不是把方向光一起关掉。
    return direct * mix(1.0, shadow, clamp(ubo.shadowParams.y, 0.0, 1.0));
}

vec3 CalcPointLight(PointLight light, vec3 N, vec3 fragPos, vec3 V, vec3 albedo, float roughness, float metallic, vec3 F0) {
    vec3 lightPos = light.position.xyz;
    float lightRadius = 30.0;
    vec3 lightColor = light.color.xyz;
    float lightIntensity = light.color.w;
    vec3 L = lightPos - fragPos;
    float distance = length(L);
    if (distance >= lightRadius) return vec3(0.0);
    L = normalize(L);
    float distSquared = distance * distance;
    float radiusSquared = lightRadius * lightRadius;
    float attenuation = pow(clamp(1.0 - pow(distSquared / radiusSquared, 2.0), 0.0, 1.0), 2.0);
    attenuation /= (distSquared + 1.0); 
    vec3 radiance = lightColor * lightIntensity * attenuation;
    return CalcLightBRDF(L, V, N, albedo, roughness, metallic, F0, radiance);
}

void main() {
    vec3 fragPosWorld = texture(inPosition, inUV).xyz;
    vec3 N            = texture(inNormal, inUV).xyz;
    vec3 albedo       = texture(inAlbedo, inUV).rgb;
    vec4 pbr          = texture(inPbr, inUV); 
    float ssao        = texture(inSSAO, inUV).r;
    float metallic  = pbr.r;
    float roughness = pbr.g;
    roughness = max(roughness, 0.04);
    
    if (length(N) < 0.1) {
        outColor = vec4(albedo, 1.0);
        return;
    }

    vec3 V = normalize(ubo.cameraPos.xyz - fragPosWorld);
    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, metallic);

    vec3 Lo = vec3(0.0);
    // 用视空间深度选级：世界距离在透视下和屏幕上的密度对不上。
    // 透视投影下 clip.w 恰好等于 -z_view，正好就是要的值，省一次矩阵乘法。
    float viewDepth = (ubo.projectionView * vec4(fragPosWorld, 1.0)).w;
    Lo += CalcDirectionalLight(N, V, albedo, roughness, metallic, F0, fragPosWorld, viewDepth);

    for (int i = 0; i < ubo.numLights; i++)
        Lo += CalcPointLight(ubo.pointLights[i], N, fragPosWorld, V, albedo, roughness, metallic, F0);

    // ---------------- IBL（split-sum 近似）----------------
    // 漫反射：直接用预烘焙的 irradiance cubemap（已做余弦卷积）
    // 镜面  ：prefiltered cubemap（按粗糙度选 mip）x BRDF 积分表
    // 原来那句 envSpecular *= (1.0 - roughness) 是能量不守恒的粗糙近似，已替换。
    float NdotV = max(dot(N, V), 0.0);
    vec3 F_ambient = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD_ambient = (vec3(1.0) - F_ambient) * (1.0 - metallic);

    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    // prefiltered cubemap 从 128 起共 5 级 mip，所以最大 LOD = 4
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilteredMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 brdf = texture(brdfLut, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefiltered * (F_ambient * brdf.x + brdf.y);

    // Fresnel approaches 1.0 at grazing angles. With a bright environment
    // that turns every silhouette into a hard white rim. Keep some grazing
    // response, but suppress the saturated outline.
    float grazingSpecular = mix(0.12, 1.0, smoothstep(0.0, 0.25, NdotV));
    specularIBL *= grazingSpecular;

    vec3 ambient = kD_ambient * diffuseIBL + specularIBL;

    // SSAO 近似的是「间接光」的可见性，不能去压暗方向光/点光的直接照明。
    // 因此只调制环境项，并且要在色调映射之前应用。
    ambient *= ssao;

    vec3 color = ambient + Lo;

    // B：距离雾。只作用于有几何的法线像素；天空在下面提前返回，保持原样。
    // 雾色来自同一张环境图，所以远处地形、水面、玻璃会一起淡出到地平线。
    if (ubo.fogParams.w > 0.5) {
        float fogDistance = length(ubo.cameraPos.xyz - fragPosWorld);
        float fogAmount = 1.0 - exp(-ubo.fogParams.y * max(fogDistance - ubo.fogParams.x, 0.0));
        color = mix(color, SampleFogColor(V), clamp(fogAmount, 0.0, 1.0));
    }

    // =====================================================================
    // D-2：这里只输出「线性 HDR 辐照度」，不再做曝光 / ACES 色调映射 / gamma。
    //
    // 原因：Bloom 是「对亮部做模糊再叠加」的后效，它必须作用在色调映射**之前**
    // 的线性 HDR 上 —— 一旦提前 clamp 到 [0,1]（ACES 的最后一步就是 clamp），
    // 高光源信息就永久丢失，Bloom 只能把已经归一化的白色再涂白一遍，
    // 得到的是"整体发灰"而不是"光溢出"。
    //
    // 曝光也一并挪到 PresentPass：曝光本质上是色调映射的输入缩放，
    // 放在这里会让 Bloom 的阈值随曝光联动，调曝光时辉光强度会跟着乱变。
    // =====================================================================
    outColor = vec4(color, 1.0);
}
