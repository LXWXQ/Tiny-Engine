#version 450

// 🌟 核心修复 1：恢复你原本的 3D 向量输入！(请确保你的 vert 着色器传过来的是 vec3 局部/世界坐标)
layout(location = 0) in vec3 inUVW;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outPBR;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D environmentMap;
const vec2 invAtan = vec2(0.1591, 0.3183);

// 等距柱状投影。
//
// 注意 V 的方向：这张 HDR 的**图像顶部是天顶**，所以 +Y 方向必须落在 uv.y = 0。
// 原来的写法 (asin(y)/PI + 0.5) 得到的是 uv.y = 1（图像底行），整个天空上下颠倒：
// 屏幕上方采样到了贴图的雾/地面区域，于是看起来又灰又平、云都在下半部分。
// 以 ibl_equirect_to_cube.frag 为准（它一直是 0.5 - asin/PI，也是 IBL 光照用的方向），
// 两边必须一致，否则"看到的天空"和"照亮场景的环境光"来自相反方向。
vec2 sampleSphericalMap(vec3 v) {
    return vec2(atan(v.z, v.x) * invAtan.x + 0.5,
                0.5 - asin(clamp(v.y, -1.0, 1.0)) * invAtan.y);
}

void main() {
    // 1. 利用你原本的逻辑，进行正确的 3D 向量球面采样！
    vec3 dir = normalize(inUVW);
    vec2 envUV = sampleSphericalMap(dir);
    // ---------------------------------------------------------------------
    // 等距柱状贴图的接缝修复（这就是那条虚线亮纹的来源）
    //
    // sampleSphericalMap() 用 atan(v.z, v.x) 求经度：在 ±PI 处它会从 +PI 跳回
    // -PI，于是 envUV.x 在接缝两侧相差约 1.0。硬件用 dFdx/dFdy 自动选 mip，
    // 在接缝那一列算出的导数 ≈ 1.0（整整绕一圈），判定"这里被极度缩小"，
    // 直接退到最低 mip（2048x1024 退到 1x1），结果是一条亮线。
    // 实测：把 textureQueryLod 可视化出来，整片天空 LOD≈0，只有那条线是 LOD=8。
    //
    // 修法：不用隐式导数，改成手工算 dFdx/dFdy 后把跨接缝的那一份折回来
    // （差值落在 [-0.5, 0.5]），再交给 textureGrad。这样接缝两侧的 LOD 连续，
    // 纹理本身又是 REPEAT 寻址，采样值天然连续，线就消失了。
    // ---------------------------------------------------------------------
    vec2 dx = dFdx(envUV);
    vec2 dy = dFdy(envUV);
    dx.x -= round(dx.x);
    dy.x -= round(dy.x);
    vec3 envColor = textureGrad(environmentMap, envUV, dx, dy).rgb;
    
    // 2. 直接透传线性 HDR 辐照度，不在这里做任何色调映射。
    //
    // D-2 修正：原来这里做了 ACES + gamma，理由是"天空盒会被 lighting.frag
    // 通过法线为 0 的标记原样透传为最终像素"。但 D-2 之后光照通道输出的是
    // 线性 HDR、由 PresentPass 统一做 tonemap，天空盒再提前 tonemap 一次
    // 就成了双重色调映射 —— 实测表现为天空被压暗发灰、边缘出现一圈假光晕
    // （因为被 clamp 到 1.0 的天空仍然超过 Bloom 阈值，整块天空都在起辉光）。
    //
    // 正确做法：天空盒和普通几何一样，只往 HDR 图里写线性值。
    // 为了让 envColor > 1 的高光能活到 Bloom，albedo 附件必须是浮点格式
    // （见 GBufferFormat::Albedo）—— 8bit UNORM 会在这里直接截断。
    
    // 3. 将处理好的终极颜色存入反照率通道
    outAlbedo = vec4(envColor, 1.0);
    
    // 4. 给 Lighting Pass 留下接头暗号：法线为 0
    outNormal = vec4(0.0, 0.0, 0.0, 1.0);
    
    // 5. 其余通道填 0 确保安全
    // 5. Store a far point along the sky direction so the motion-blur
    // reprojection pass gets a valid camera-relative world position.
    outPosition = vec4(ubo.cameraPos.xyz + normalize(inUVW) * 10000.0, 1.0);
    outPBR = vec4(0.0);
}
