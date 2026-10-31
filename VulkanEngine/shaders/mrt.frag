#version 450
#extension GL_EXT_nonuniform_qualifier : require   // D-3 bindless 的数组索引

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragPosWorld;
layout(location = 2) in vec3 fragNormalWorld;
layout(location = 3) in vec2 fragUV;
layout(location = 4) in vec3 fragPosLocal;
layout(location = 5) in vec3 fragNormalLocal;

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
} push;

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
    vec2 normalized = (uv - push.checkerUvTransform.zw) * push.checkerUvTransform.xy;
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
    if (push.checkerUvTransform.x > 0.0f)
        return mix(vec3(0.03), vec3(0.85), checkerUv(fragUV));

    const vec3 p = localPos * push.checkerScale;
    return mix(vec3(0.03), vec3(0.85), checker3D(p));
}

// 你的偏导数法线计算魔法保留在这里！
vec3 getNormalFromMap(uint matId) {
    vec3 tangentNormal = texture(normalMaps[nonuniformEXT(matId)], fragUV).xyz * 2.0 - 1.0;
    
    vec3 Q1  = dFdx(fragPosWorld);
    vec3 Q2  = dFdy(fragPosWorld);
    vec2 st1 = dFdx(fragUV);
    vec2 st2 = dFdy(fragUV);
    
    vec3 N   = normalize(fragNormalWorld);
    
    // ✨ 核心修复：UV 接缝拦截器
    // 正常像素间 UV 变化极小。如果跨度超过 0.5，说明跨越了贴图的 0.0~1.0 边界
    if (abs(st1.s) > 0.5 || abs(st1.t) > 0.5 || abs(st2.s) > 0.5 || abs(st2.t) > 0.5) {
        return N; // 遇到接缝，直接返回顶点自带的光滑法线，保平安！
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

void main() {
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

    // 材质下标。y == 0 表示这个物体没有可用的材质槽位（天空盒 / 程序化网格 /
    // 编辑器里还没分配材质的网格），直接画棋盘格占位 —— 因为此时数组里那个
    // 下标可能压根没绑定内容（descriptorBindingPartiallyBound 允许数组尾部未绑定，
    // 采它就是未定义行为）。
    const uint matId = push.material.x;
    if (push.material.y == 0u) {
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
    vec4 albedoColor = (push.albedoFallback != 0u)
        ? vec4(checkerboardFallback(fragPosLocal), 1.0)
        : texture(albedoMaps[nonuniformEXT(matId)], fragUV);
    if (push.alphaMasked != 0u && albedoColor.a < push.alphaCutoff)
        discard;

    // 占位棋盘格不乘顶点色（理由同上：要保证任何模型都能看出黑白格）；
    // 真正有 albedo 贴图的走原来的 tint 行为。
    vec3 albedo = albedoColor.rgb;
    if (push.albedoFallback == 0u)
        albedo *= fragColor;
    outAlbedo = vec4(albedo, albedoColor.a);

    // 4. PBR 参数打包 (R: 金属度, G: 粗糙度)
    // 占位材质没有 metallic/roughness 贴图：默认白纹理会给出 metallic=1，
    // 让棋盘格变成金属反射，黑白对比会被环境光吃掉。退化为"粗糙电介质"。
    float metallic = 0.0;
    float roughness = 1.0;
    if (push.albedoFallback == 0u)
    {
        metallic = texture(metallicMaps[nonuniformEXT(matId)], fragUV).b;
        roughness = texture(roughnessMaps[nonuniformEXT(matId)], fragUV).g;
    }
    outPBR = vec4(metallic, roughness, 0.0, 1.0);
}