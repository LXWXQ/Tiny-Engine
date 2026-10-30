#version 450
#extension GL_EXT_nonuniform_qualifier : require   // D-3 bindless 的数组索引

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragPosWorld;
layout(location = 2) in vec3 fragNormalWorld;
layout(location = 3) in vec2 fragUV;
layout(location = 4) in vec3 fragPosLocal;

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
} push;

// 程序化棋盘格（8x8 格）。
//
// 用途：没有有效材质 / 材质没有 albedo 贴图时，用它代替"默认白纹理"。
// 纯白渲染会让物体变成一团没有细节的白块 —— 看不出形状、朝向、有没有在动。
// 棋盘格能同时暴露：轮廓、UV 方向、物体的旋转。
//
// 用 fragUV 而不是屏幕坐标：贴图跟着物体走，转动物体时格子会跟着转。
// 占位材质的棋盘格（三平面 + 盒式滤波抗锯齿）
//
// 【为什么不用 UV】
//   1. 不同模型/不同面的 UV 尺度可能完全不一致 —— 同一个立方体六个面会显示
//      出密度不同的格子，看起来像"贴图错了"；
//   2. UV 频率一旦过高就会产生**摩尔纹**（远处的面糊成一片条纹）。
//   改用**局部空间坐标**按法线主轴投影（三平面映射的单轴版）：
//   六个面共用同一套尺度，且格子跟着物体旋转/移动。
//
// 【为什么必须做滤波】
//   程序化棋盘格没有 mipmap，高频处必然走样。这里用 fwidth 估计
//   一个像素覆盖多少格，做盒式滤波：
//     - 格子足够大时 -> 接近清晰的黑白格
//     - 格子小于一个像素时 -> 平滑收敛到平均灰度，不产生摩尔纹
vec3 checkerboardTriplanar(vec3 localPos, vec3 worldNormal)
{
    // 按法线主轴选两个分量（与三平面映射同样的思路）
    vec3 an = abs(normalize(worldNormal));
    vec2 coord;
    if (an.x >= an.y && an.x >= an.z)      coord = localPos.zy;
    else if (an.y >= an.z)                 coord = localPos.xz;
    else                                   coord = localPos.xy;

    // 密度来自 CPU：保证"物体最大尺寸上大约 6 格"，
    // 与模型自身的局部尺寸无关（debug_cube.obj 与 Quad-Sphere.obj 差上百倍）。
    coord *= push.checkerScale;

    // 盒式滤波：w = 一个像素在 coord 空间覆盖的宽度
    vec2 w = fwidth(coord) * 0.5 + 1e-5;
    vec2 i = 2.0 * (abs(fract((coord - w) * 0.5) - 0.5)
                  - abs(fract((coord + w) * 0.5) - 0.5)) / w;
    float pattern = 0.5 - 0.5 * i.x * i.y;   // 0/1 = 两色，0.5 = 平均

    return mix(vec3(0.05), vec3(0.62), pattern);
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
    outNormal = vec4(normalize(fragNormalWorld), 1.0);

    // 材质下标。y == 0 表示这个物体不用材质（天空盒 / 程序化网格），
    // 直接把顶点色当 albedo 写出去 —— 因为此时数组里那个下标可能压根没绑定内容
    //（descriptorBindingPartiallyBound 允许数组尾部未绑定，采它就是未定义行为）。
    const uint matId = push.material.x;
    if (push.material.y == 0u) {
        // 没有材质：画棋盘格而不是纯顶点色（顶点色通常是白的，看不出形状）
        outAlbedo = vec4(checkerboardTriplanar(fragPosLocal, fragNormalWorld) * fragColor, 1.0);
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
        ? vec4(checkerboardTriplanar(fragPosLocal, fragNormalWorld), 1.0)
        : texture(albedoMaps[nonuniformEXT(matId)], fragUV);
    if (push.alphaMasked != 0u && albedoColor.a < push.alphaCutoff)
        discard;
    outAlbedo = vec4(albedoColor.rgb * fragColor, albedoColor.a);

    // 4. PBR 参数打包 (R: 金属度, G: 粗糙度)
    float metallic = texture(metallicMaps[nonuniformEXT(matId)], fragUV).b;
    float roughness = texture(roughnessMaps[nonuniformEXT(matId)], fragUV).g;
    outPBR = vec4(metallic, roughness, 0.0, 1.0);
}