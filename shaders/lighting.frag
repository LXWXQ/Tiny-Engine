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
} ubo;

layout(set = 0, binding = 1) uniform sampler2D environmentMap;

layout(set = 0, binding = 2) uniform sampler2D inPosition;
layout(set = 0, binding = 3) uniform sampler2D inNormal;
layout(set = 0, binding = 4) uniform sampler2D inAlbedo;
layout(set = 0, binding = 5) uniform sampler2D inPbr;
layout(set = 0, binding = 6) uniform sampler2D inSSAO;

// IBL 三件套（由 IBLBaker 在初始化阶段预烘焙）
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

vec3 CalcDirectionalLight(vec3 N, vec3 V, vec3 albedo, float roughness, float metallic, vec3 F0) {
    vec3 L = normalize(-ubo.lightDirection.xyz);
    vec3 radiance = ubo.lightColor.xyz * ubo.lightColor.w;
    return CalcLightBRDF(L, V, N, albedo, roughness, metallic, F0, radiance);
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
    Lo += CalcDirectionalLight(N, V, albedo, roughness, metallic, F0);

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
