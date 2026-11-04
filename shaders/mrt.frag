#version 450
#extension GL_EXT_nonuniform_qualifier : require   // D-3 bindless 的数组索引

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragPosWorld;
layout(location = 2) in vec3 fragNormalWorld;
layout(location = 3) in vec2 fragUV;
layout(location = 4) in vec3 fragPosLocal;
layout(location = 5) in vec3 fragNormalLocal;
layout(location = 6) flat in float fragTerrainLodBlend;
layout(location = 7) flat in uint fragTerrainLodBlendMode;
// F1：GPU 驱动提交下，材质参数来自实例缓冲而不是 push constant。
layout(location = 8) flat in uint fragInstanceIndex;

// 向 4 个 G-Buffer 颜色附件输出。
// 编号是「输出槽位」而不是附件序号 —— 动态渲染下附件集合由 GeometryPass
// 在渲染图里声明（Position/Normal/Albedo/Pbr/Depth），这里只管按槽位写。
layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outPBR;

// ---------------------------------------------------------------------------
// D-3 bindless：材质纹理不再是"每材质一个描述符集"，而是 4 个纹理数组，
// 用同一个材质下标去索引。
//
// 为什么要 nonuniformEXT：同一个 draw call 里，不同像素可能属于不同子网格、
// 用到不同材质下标。没有这个限定符，GLSL 假定数组索引在一次 draw 内是
// "uniform"（所有调用一致），驱动就可以只取一次描述符 —— 结果就是整片
// 用一个材质画出来。加上它等于告诉编译器"索引可能逐像素不同"。
//
// 数组长度是运行期决定的（VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT），
// 所以这里写 [] 而不是写死 256。
// ---------------------------------------------------------------------------
layout(set = 1, binding = 0) uniform sampler2D albedoMaps[];
layout(set = 1, binding = 1) uniform sampler2D normalMaps[];
layout(set = 1, binding = 2) uniform sampler2D metallicMaps[];
layout(set = 1, binding = 3) uniform sampler2D roughnessMaps[];

// E3: glTF 材质 factor（baseColor / metallic / roughness），按材质槽位索引。
// 放在全局 set（set 0, binding 10）：bindless 材质 set 只有 4 个纹理数组，
// 而 factor 是"每材质一份的标量"，用 SSBO 最省事，也同时适用于
// CPU 路径和 GPU 驱动提交（两者都绑 set 0）。
// 每材质两个 vec4（baseColor、metalRough）在缓冲里平铺成 vec4 数组，
// 下标 = matId * 2 与 matId * 2 + 1，对应 C++ 侧 32 字节的
// MaterialFactorRecord。扁平数组的 stride 显式可验证（16 字节）。
layout(set = 0, binding = 10) readonly buffer MaterialFactorBlock {
    vec4 factors[];
} materialFactors;

// Terrain stage 6: four fixed material layers. Layer order is sand, grass,
// rock, snow and matches TerrainTextureGenerator / TerrainMaterial.
layout(set = 2, binding = 0) uniform sampler2D terrainAlbedoSand;
layout(set = 2, binding = 1) uniform sampler2D terrainAlbedoGrass;
layout(set = 2, binding = 2) uniform sampler2D terrainAlbedoRock;
layout(set = 2, binding = 3) uniform sampler2D terrainAlbedoSnow;
layout(set = 2, binding = 4) uniform sampler2D terrainNormalSand;
layout(set = 2, binding = 5) uniform sampler2D terrainNormalGrass;
layout(set = 2, binding = 6) uniform sampler2D terrainNormalRock;
layout(set = 2, binding = 7) uniform sampler2D terrainNormalSnow;
// r = height, g = roughness (b/a reserved). Packing avoids four extra
// texture fetches per terrain pixel.
layout(set = 2, binding = 8) uniform sampler2D terrainHeightRoughnessSand;
layout(set = 2, binding = 9) uniform sampler2D terrainHeightRoughnessGrass;
layout(set = 2, binding = 10) uniform sampler2D terrainHeightRoughnessRock;
layout(set = 2, binding = 11) uniform sampler2D terrainHeightRoughnessSnow;

// 材质下标通过 push constant 传进来（见 SimplePushConstantData::materialId）
layout(push_constant) uniform Push {
    mat4 modelMatrix;
    mat4 normalMatrix;
    // x = 材质槽位下标，y = 是否有效（0 = 天空盒/网格这类不需要材质的物体）
    uvec2 material;
    float alphaCutoff;
    uint alphaMasked;
    // 1 = 材质没有 albedo 贴图，用程序化棋盘格代替
    uint albedoFallback;
    // 棋盘格密度（由 CPU 按模型包围盒算好）
    float checkerScale;
    // 有 UV 时的归一化：(uv - zw) * xy
    vec4 checkerUvTransform;
    uint terrainMode;
    float terrainMinY;
    float terrainHeightRange;
    float terrainDetailStrength;
    float terrainUvTiling;
    uint terrainMaterialStyle;
    float terrainLodBlend;
    uint terrainLodBlendMode;
    uint gpuScene;
    uint terrainBatched;
} push;

// F1：与 shaders/scene_cull.comp / mrt.vert 的 InstanceRecord 严格一致。
struct InstanceRecord
{
    mat4 model;
    mat4 normal;
    uvec4 flags;
    vec4 misc;
    vec4 uvTransform;
    vec4 boundsMin;
    vec4 boundsMax;
    uvec4 draw;
};
layout(set = 3, binding = 0) readonly buffer InstanceBuffer { InstanceRecord instances[]; };

// GPU 驱动提交时材质参数在实例缓冲里；统一复制到这份可写副本，
// 主流程与棋盘格函数都读它，避免在几十处分支里分别判断来源。
// 注意：push_constant 块不能再声明额外实例，所以这里用只含材质字段的副本结构。
struct MaterialParams
{
    uvec2 material;
    float alphaCutoff;
    uint alphaMasked;
    uint albedoFallback;
    float checkerScale;
    vec4 checkerUvTransform;
};
MaterialParams gMaterial;

// 程序化棋盘格。
//
// 用途：没有有效材质槽位 / 材质没有 albedo 贴图时，用它代替"默认白纹理"。
// 纯白渲染会让物体变成一团没有细节的白块 —— 看不出形状、朝向、有没有在动。
// 棋盘格能同时暴露：轮廓、UV 方向、物体的旋转。
//
// 【为什么不用 UV】
//   1. 不同模型/不同面的 UV 尺度可能完全不一致 —— 同一个立方体六个面会显示
//      出密度不同的格子，看起来像"贴图错了"；
//   2. 还有些模型（debug_cube.obj、plane.obj）根本没有有效 UV；
//   3. UV 频率一旦过高就会产生摩尔纹。
// 改用**局部空间坐标 + 局部法线**做三平面投影：任何多面体、球体、
// 规则/不规则平面都能出格子，且格子跟着物体旋转、不会随世界坐标滑动。
//
// 【为什么必须 clamp】
//   下面的盒式滤波是对"方波"的近似。滤波窗口比格子小时，表达式会外插到
//   [-1.5, 2.5] 而不是 [0,1]。负的 albedo 会写进 G-Buffer，而 PresentPass
//   的 ACES 对负输入会 clamp 到**白色**（分子分母同时为负 -> 正值），
//   表现就是"本该是黑格子的地方全变成白的，只剩格子缝是暗的"。
//   所以这里必须把 pattern 夹到 [0,1]。
float checkerFiltered(vec2 p, vec2 dpdx, vec2 dpdy)
{
    // 盒式滤波：w = 一个像素在 p 空间覆盖的宽度。
    // 格子周期是 2，所以半宽正好覆盖"半格"的过渡带。
    vec2 w = (abs(dpdx) + abs(dpdy)) * 0.5 + 1e-5;
    vec2 i = 2.0 * (abs(fract((p - w) * 0.5) - 0.5)
                  - abs(fract((p + w) * 0.5) - 0.5)) / w;
    return clamp(0.5 - 0.5 * i.x * i.y, 0.0, 1.0);
}

// 有完整 UV 的网格用 2D 棋盘按 UV 包在表面上（平面、teapot）。
// 2D 棋盘按 UV 包在表面上，比三平面混合更接近"贴了一张棋盘贴图"，
// 球面上也不会出现三平面在 45° 过渡带把黑白混成一片灰的抹痕。
//
// u 在 1 -> 0 处环绕：fragUV 跳变会让 dFdx 变成约 8（8 格）。
// 直接用它做滤波会把接缝那一列糊成灰色；把跨接缝的导数折回 [-0.5,0.5]
// 就恢复了正常频率（和 skybox 的 textureGrad 处理同一个道理）。
float checkerUv(vec2 uv)
{
    const float kTiles = 8.0;             // 参考你给的黑白格：约 8 格
    vec2 normalized = (uv - gMaterial.checkerUvTransform.zw) * gMaterial.checkerUvTransform.xy;
    vec2 p = normalized * kTiles;
    vec2 dpdx = dFdx(p);
    vec2 dpdy = dFdy(p);
    dpdx.x -= round(dpdx.x);
    dpdy.x -= round(dpdy.x);
    return checkerFiltered(p, dpdx, dpdy);
}

// 3D 棋盘格：在局部空间里按单位立方体做黑白格。
// 对任何表面（球、多面体、不规则平面）都连续，没有 UV 接缝，也不会像
// 三平面那样在 45° 过渡带把黑白混成一片灰。
float checker3D(vec3 p)
{
    vec3 q = p + vec3(0.317f, 0.619f, 0.733f); // 避免平面正好落在格子边界上（整面变灰）
    vec3 f = fract(q);
    vec3 toBoundary = min(f, vec3(1.0) - f); // 0 = 边界，0.5 = 格子中心
    vec3 w = (abs(dFdx(q)) + abs(dFdy(q))) * 0.5 + 1e-5;
    vec3 inside = smoothstep(vec3(0.0), w, toBoundary);

    float parity = mod(floor(q.x) + floor(q.y) + floor(q.z), 2.0);
    return mix(0.5, parity, min(min(inside.x, inside.y), inside.z));
}

// 无纹理网格的最终入口：
//   * UV 覆盖完整（接近 [0,1]）-> 2D 棋盘按 UV 包在表面上（平面/teapot）；
//   * UV 是图集子区间或没有 UV（Quad-Sphere / debug_cube）-> 局部空间 3D 棋盘。
// 3D 棋盘在球面上不会出现三平面的灰带或 UV 图集的接缝。
vec3 checkerboardFallback(vec3 localPos)
{
    if (gMaterial.checkerUvTransform.x > 0.0f)
        return mix(vec3(0.03), vec3(0.85), checkerUv(fragUV));

    const vec3 p = localPos * gMaterial.checkerScale;
    return mix(vec3(0.03), vec3(0.85), checker3D(p));
}

// ---------------------------------------------------------------------------
// Terrain stage 6: shared material weights + low-frequency macro modulation.
// Per-layer albedo/normal textures live in the dedicated terrain set (set 2).
// ---------------------------------------------------------------------------
float terrainHash21(vec2 p)
{
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p.yx + 34.23);
    return fract(p.x * p.y);
}

float terrainValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    // Quintic fade gives a continuous second derivative, so finite-difference
    // bump lighting does not expose square lattice cells.
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = terrainHash21(i);
    float b = terrainHash21(i + vec2(1.0, 0.0));
    float c = terrainHash21(i + vec2(0.0, 1.0));
    float d = terrainHash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float terrainFbm(vec2 p)
{
    const vec2 r1 = vec2(0.80, 0.60);
    const vec2 r2 = vec2(-0.60, 0.80);
    vec2 p1 = vec2(dot(p, r1), dot(p, vec2(-r1.y, r1.x)));
    vec2 p2 = vec2(dot(p, r2), dot(p, vec2(-r2.y, r2.x)));
    return 0.50 * terrainValueNoise(p) +
           0.30 * terrainValueNoise(p1 * 2.03 + vec2(17.1, 9.2)) +
           0.20 * terrainValueNoise(p2 * 5.11 + vec2(-7.7, 23.4));
}

// 你的偏导数法线计算魔法保留在这里！
vec3 getNormalFromMap(uint matId) {
    vec3 tangentNormal = texture(normalMaps[nonuniformEXT(matId)], fragUV).xyz * 2.0 - 1.0;

    vec3 Q1  = dFdx(fragPosWorld);
    vec3 Q2  = dFdy(fragPosWorld);
    vec2 st1 = dFdx(fragUV);
    vec2 st2 = dFdy(fragUV);

    vec3 N   = normalize(fragNormalWorld);

    // UV 接缝拦截：跨越 0/1 边界时退回几何法线。
    if (abs(st1.s) > 0.5 || abs(st1.t) > 0.5 ||
        abs(st2.s) > 0.5 || abs(st2.t) > 0.5) {
        return N;
    }

    // 防御极点退化
    float det = st1.s * st2.t - st2.s * st1.t;
    if (abs(det) < 0.00001) {
        return N;
    }
    
    float invDet = 1.0 / det;
    vec3 T = (Q1 * st2.t - Q2 * st1.t) * invDet;
    
    T = normalize(T - dot(T, N) * N);
    vec3 B = -normalize(cross(N, T)); // 保持你原本的负号
    
    mat3 TBN = mat3(T, B, N);
    return normalize(TBN * tangentNormal);
}

float terrainBayer4(ivec2 pixel)
{
    // Interleaved gradient noise: deterministic per pixel, less structured than
    // a 4x4 Bayer matrix when motion blur smears the transition.
    const vec2 p = vec2(pixel) + vec2(0.5);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    gMaterial.material = push.material;
    gMaterial.alphaCutoff = push.alphaCutoff;
    gMaterial.alphaMasked = push.alphaMasked;
    gMaterial.albedoFallback = push.albedoFallback;
    gMaterial.checkerScale = push.checkerScale;
    gMaterial.checkerUvTransform = push.checkerUvTransform;
    if (push.gpuScene != 0u)
    {
        InstanceRecord inst = instances[fragInstanceIndex];
        gMaterial.material = inst.flags.xy;
        gMaterial.alphaMasked = inst.flags.z;
        gMaterial.albedoFallback = inst.flags.w;
        gMaterial.alphaCutoff = inst.misc.x;
        gMaterial.checkerScale = inst.misc.y;
        gMaterial.checkerUvTransform = inst.uvTransform;
    }

    if (fragTerrainLodBlendMode != 0u)
    {
        const float dither = terrainBayer4(ivec2(gl_FragCoord.xy));
        if (fragTerrainLodBlendMode == 1u && dither >= fragTerrainLodBlend)
            discard;
        if (fragTerrainLodBlendMode == 2u && dither < fragTerrainLodBlend)
            discard;
    }

    // 1. 位置 (世界坐标)
    outPosition = vec4(fragPosWorld, 1.0);

    // 2. 顶点法线（世界空间）
    // 注意：切线空间法线贴图目前是「有意关闭」的 —— getNormalFromMap() 保留待用，
    // 等材质系统完善、模型能提供可靠切线之后再启用。
    //
    // 没有法线的模型（debug_cube.obj 这类 OBJ）顶点法线是零。
    // 直接 normalize(0) 会得到 NaN，进而污染整个 G-Buffer；这里用世界位置的
    // 屏幕导数重建几何法线（立方体/平面正好是面法线，光照也一起修好）。
    vec3 worldNormal = fragNormalWorld;
    float worldNormal2 = dot(worldNormal, worldNormal);
    if (any(isnan(worldNormal)) || worldNormal2 < 1e-8)
    {
        vec3 gn = cross(dFdx(fragPosWorld), dFdy(fragPosWorld));
        float g2 = dot(gn, gn);
        worldNormal = (g2 > 1e-12) ? gn * inversesqrt(g2) : vec3(0.0, 1.0, 0.0);
    }
    else
    {
        worldNormal *= inversesqrt(worldNormal2);
    }
    outNormal = vec4(worldNormal, 1.0);



    // Terrain stage 6: four material layers blended by height, slope and
    // layer-local height maps. Texture sampling supplies albedo, normal,
    // height and roughness detail; Surface Style biases the layer weights.
    // F2-b: the batched terrain path draws many chunks with one push constant, so
    // each chunk carries its own height range, tiling and surface style in the
    // set 3 instance table instead.
    uint tMode = push.terrainMode;
    float tMinY = push.terrainMinY;
    float tHeightRange = push.terrainHeightRange;
    float tDetailStrength = push.terrainDetailStrength;
    float tUvTiling = push.terrainUvTiling;
    uint tMaterialStyle = push.terrainMaterialStyle;
    if (push.terrainBatched != 0u) {
        InstanceRecord terrainInst = instances[fragInstanceIndex];
        tMode = uint(terrainInst.misc.z + 0.5);
        tMinY = terrainInst.uvTransform.x;
        tHeightRange = terrainInst.uvTransform.y;
        tDetailStrength = terrainInst.uvTransform.z;
        tUvTiling = terrainInst.uvTransform.w;
        tMaterialStyle = uint(terrainInst.misc.w + 0.5);
    }
    if (tMode != 0u) {
        const float h = clamp((fragPosLocal.y - tMinY) /
                              max(tHeightRange, 1e-4), 0.0, 1.0);
        const float slope = clamp(1.0 - worldNormal.y, 0.0, 1.0);

        const float sandW = 1.0 - smoothstep(0.08, 0.22, h);
        const float slopeRockW = smoothstep(0.24, 0.60, slope);
        const float highRockW = smoothstep(0.62, 0.84, h) * 0.55;
        const float rockW = max(slopeRockW, highRockW);
        const float snowW = smoothstep(0.80, 0.94, h) * (1.0 - 0.6 * rockW);
        const float grassW = clamp(1.0 - sandW - rockW - snowW, 0.0, 1.0);

        vec4 layerWeight = vec4(sandW, grassW, rockW, snowW);
        vec4 styleBias = vec4(1.0);
        vec3 styleTint = vec3(1.0);
        float styleRoughness = 0.0;
        float styleNormalScale = 1.0;
        if (tMaterialStyle == 1u) {          // Arid
            styleBias = vec4(1.35, 0.80, 1.05, 0.45);
            styleTint = vec3(1.06, 1.01, 0.90);
            styleRoughness = 0.035;
        } else if (tMaterialStyle == 2u) {   // Alpine
            styleBias = vec4(0.55, 0.75, 1.25, 1.30);
            styleTint = vec3(0.94, 0.98, 1.06);
            styleRoughness = -0.025;
            styleNormalScale = 1.15;
        } else if (tMaterialStyle == 3u) {   // Lush
            styleBias = vec4(0.55, 1.40, 0.75, 0.55);
            styleTint = vec3(0.94, 1.06, 0.90);
            styleRoughness = 0.02;
            styleNormalScale = 0.90;
        }
        layerWeight *= styleBias;

        const vec2 worldXZ = fragPosWorld.xz;
        const float uvTiling = clamp(tUvTiling, 0.01, 100.0);
        const vec2 sandUv = worldXZ * (0.65 * uvTiling) + vec2(0.11, 0.37);
        const vec2 grassUv = worldXZ * (0.85 * uvTiling) + vec2(0.53, 0.17);
        const vec2 rockUv = worldXZ * (0.48 * uvTiling) + vec2(0.23, 0.71);
        const vec2 snowUv = worldXZ * (0.36 * uvTiling) + vec2(0.61, 0.43);

        const vec4 packedSand = texture(terrainHeightRoughnessSand, sandUv);
        const vec4 packedGrass = texture(terrainHeightRoughnessGrass, grassUv);
        const vec4 packedRock = texture(terrainHeightRoughnessRock, rockUv);
        const vec4 packedSnow = texture(terrainHeightRoughnessSnow, snowUv);
        const vec4 layerHeight = vec4(packedSand.r, packedGrass.r,
                                      packedRock.r, packedSnow.r);
        layerWeight *= mix(vec4(0.72), vec4(1.28), layerHeight);
        const vec4 layerW = layerWeight / max(dot(layerWeight, vec4(1.0)), 1e-4);
        const float sandN = layerW.x;
        const float grassN = layerW.y;
        const float rockN = layerW.z;
        const float snowN = layerW.w;

        const float patchMask = terrainFbm(worldXZ * 0.19 + vec2(3.7, 8.1));
        const vec3 sandAlbedo = mix(
            texture(terrainAlbedoSand, sandUv).rgb,
            texture(terrainAlbedoSand, sandUv * 0.37 + vec2(0.43, 0.19)).rgb,
            patchMask);
        const vec3 grassAlbedo = mix(
            texture(terrainAlbedoGrass, grassUv).rgb,
            texture(terrainAlbedoGrass, grassUv * 0.39 + vec2(0.17, 0.63)).rgb,
            patchMask);
        const vec3 rockAlbedo = mix(
            texture(terrainAlbedoRock, rockUv).rgb,
            texture(terrainAlbedoRock, rockUv * 0.43 + vec2(0.71, 0.29)).rgb,
            patchMask);
        const vec3 snowAlbedo = mix(
            texture(terrainAlbedoSnow, snowUv).rgb,
            texture(terrainAlbedoSnow, snowUv * 0.41 + vec2(0.37, 0.81)).rgb,
            patchMask);
        vec3 albedo = sandN * sandAlbedo + grassN * grassAlbedo +
                      rockN * rockAlbedo + snowN * snowAlbedo;
        albedo *= styleTint;

        const float macro = terrainFbm(worldXZ * 0.50 + vec2(1.3, -2.1));
        const float materialStrength = clamp(tDetailStrength, 0.0, 1.0);
        albedo *= mix(1.0, mix(0.82, 1.18, macro), 0.35 + materialStrength * 0.35);

        const vec3 sandNormal = texture(terrainNormalSand, sandUv).xyz * 2.0 - 1.0;
        const vec3 grassNormal = texture(terrainNormalGrass, grassUv).xyz * 2.0 - 1.0;
        const vec3 rockNormal = texture(terrainNormalRock, rockUv).xyz * 2.0 - 1.0;
        const vec3 snowNormal = texture(terrainNormalSnow, snowUv).xyz * 2.0 - 1.0;
        const vec3 layerNormal = normalize(sandN * sandNormal + grassN * grassNormal +
                                           rockN * rockNormal + snowN * snowNormal);

        const vec2 footprintVec = fwidth(worldXZ);
        const float footprint = max(footprintVec.x, footprintVec.y);
        const float detailFade = 1.0 - smoothstep(0.04, 0.25, footprint);
        const float normalStrength = materialStrength * mix(0.25, 1.0, detailFade) *
                                     mix(0.75, 1.35, rockN) * styleNormalScale;
        const vec3 tangentX = normalize(vec3(1.0, 0.0, 0.0) -
                                        worldNormal * worldNormal.x + vec3(1e-5, 0.0, 0.0));
        const vec3 tangentZ = normalize(cross(tangentX, worldNormal));
        const vec3 disturbed = normalize(vec3(layerNormal.xy * normalStrength, 1.0));
        worldNormal = normalize(tangentX * disturbed.x +
                                tangentZ * disturbed.y +
                                worldNormal * disturbed.z);
        outNormal = vec4(worldNormal, 0.0);   // terrain opts out of SSAO

        const vec4 layerRoughness = vec4(packedSand.g, packedGrass.g,
                                         packedRock.g, packedSnow.g);
        float roughness = clamp(dot(layerW, layerRoughness) +
                                styleRoughness + (macro - 0.5) * 0.04, 0.45, 1.0);

        outAlbedo = vec4(albedo, 1.0);
        outPBR = vec4(0.0, roughness, 0.0, 1.0);
        return;
    }

    // 材质下标。y == 0 表示这个物体没有可用的材质槽位（天空盒 / 程序化网格 /
    // 编辑器里还没分配材质的网格），直接画棋盘格占位 —— 因为此时数组里那个
    // 下标可能压根没绑定内容（descriptorBindingPartiallyBound 允许数组尾部未绑定，
    // 采它就是未定义行为）。
    const uint matId = gMaterial.material.x;
    if (gMaterial.material.y == 0u) {
        // 不乘顶点色：占位着色的目的就是让任何模型（含顶点色非白的）
        // 都能看到清晰的黑白格。
        outAlbedo = vec4(checkerboardFallback(fragPosLocal), 1.0);
        outPBR = vec4(0.0, 1.0, 0.0, 1.0);   // 金属度 0、粗糙度 1
        return;
    }

    // 3. 基础颜色
    // albedo 贴图以 SRGB 格式采样，硬件解码后拿到的已经是线性值。
    // G-Buffer 的 albedo 附件自 D-2 起是 R16G16B16A16_SFLOAT（线性、不编码），
    // 所以写入端**什么都不用做** —— 既不要 pow(2.2)（会双重伽马），
    // 也不要指望硬件帮你编码（浮点格式不做 sRGB 转换）。
    // 之所以换成浮点：天空盒借道这个通道写 >1.0 的线性 HDR，8bit 会在写入时截断。
    // 材质没有 albedo 贴图时，采样到的是"默认白纹理" —— 渲染出来是一团白。
    // 改画棋盘格，让占位材质一眼可辨。
    // E3: factor 乘到采样结果上（glTF 规范：factor 与贴图相乘）。
    const vec4 baseColorFactor = materialFactors.factors[matId * 2u];
    // F3-f：体素图集 —— 顶点 uv.x = tile * 64 + 面内跨度，uv.y = 面内跨度；
    // 材质的 metalRough.z == 1.0 表示要走这层解码。
    const bool voxelAtlas = materialFactors.factors[matId * 2u + 1u].z > 0.5;
    vec2 albedoUV = fragUV;
    // 体素图集：fract() 在每格边界产生导数尖峰，隐式 LOD 会被推到最粗 mip，
    // 采样到整张图集的平均值（其中包含未使用的白色 tile）→ 方块边缘白线/闪烁。
    // 用显式梯度绕开 fract 的导数不连续：面内 uv 本身是线性的，直接缩放 0.25 即可。
    vec2 albedoGradX = dFdx(fragUV);
    vec2 albedoGradY = dFdy(fragUV);
    if (voxelAtlas)
    {
        const float tileId = floor(fragUV.x / 64.0);
        const vec2 local = fract(vec2(fragUV.x - tileId * 64.0, fragUV.y));
        albedoUV = (vec2(mod(tileId, 4.0), floor(tileId / 4.0)) + clamp(local, 0.5 / 64.0, 1.0 - 0.5 / 64.0)) * 0.25;
        albedoGradX *= 0.25;
        albedoGradY *= 0.25;
    }
    vec4 albedoColor = (gMaterial.albedoFallback != 0u)
        ? vec4(checkerboardFallback(fragPosLocal), 1.0)
        : textureGrad(albedoMaps[nonuniformEXT(matId)], albedoUV, albedoGradX, albedoGradY) * baseColorFactor;
    if (gMaterial.alphaMasked != 0u && albedoColor.a < gMaterial.alphaCutoff)
        discard;

    // 占位棋盘格不乘顶点色（理由同上：要保证任何模型都能看出黑白格）；
    // 真正有 albedo 贴图的走原来的 tint 行为。
    vec3 albedo = albedoColor.rgb;
    if (gMaterial.albedoFallback == 0u)
        albedo *= fragColor;
    outAlbedo = vec4(albedo, albedoColor.a);

    // 4. PBR 参数打包 (R: 金属度, G: 粗糙度)
    // 占位材质没有 metallic/roughness 贴图：默认白纹理会给出 metallic=1，
    // 让棋盘格变成金属反射，黑白对比会被环境光吃掉。退化为"粗糙电介质"。
    float metallic = 0.0;
    float roughness = 1.0;
    if (gMaterial.albedoFallback == 0u)
    {
        const vec2 metallicRoughnessFactor = materialFactors.factors[matId * 2u + 1u].xy;
        metallic = texture(metallicMaps[nonuniformEXT(matId)], fragUV).b * metallicRoughnessFactor.x;
        roughness = clamp(texture(roughnessMaps[nonuniformEXT(matId)], fragUV).g * metallicRoughnessFactor.y,
                          0.04, 1.0);
    }
    outPBR = vec4(metallic, roughness, 0.0, 1.0);
}