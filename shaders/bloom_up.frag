#version 450

// Bloom 上采样：3x3 帐篷滤波（tent filter），配合管线的「加法混合」叠加到细一级的图上。
//
// 为什么用帐篷而不是盒式：盒式的频率响应有旁瓣，叠加若干级之后会出现
// 明显的方块状拖影；帐篷核在各级之间能形成近似高斯的总响应，辉光过渡自然。
//
// 混合状态由 C++ 侧 BaloomPass 设置成 ONE / ONE（纯加），
// 因此本 shader 只输出"这一级要贡献的量"，不需要自己读回目标图。

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec2 srcTexelSize;   // 1.0 / srcExtent（src = 粗一级、分辨率是一半）
    float radius;        // 采样半径缩放，1.0 为标准帐篷
    float pad;
} pc;

vec3 T(vec2 uv) { return texture(srcTex, uv).rgb; }

void main()
{
    // 与 bloom_down 相同的坐标系换算：dest 半纹素 == src 一纹素，
    // 用 -0.5 .. +0.5 的偏移表示"源纹素中心到目标像素中心"的距离
    vec2 uv = inUV * 2.0 - 0.5;
    vec2 t  = pc.srcTexelSize * pc.radius;

    // 3x3 帐篷权重：角 1/16，边 1/8，心 1/4（和为 1）
    vec3 result = T( uv) * 4.0;
    result += (T(uv + vec2(-t.x,  0.0)) + T(uv + vec2(t.x,  0.0))
             + T(uv + vec2( 0.0, -t.y)) + T(uv + vec2(0.0,  t.y))) * 2.0;
    result += (T(uv + vec2(-t.x, -t.y)) + T(uv + vec2(t.x, -t.y))
             + T(uv + vec2(-t.x,  t.y)) + T(uv + vec2(t.x,  t.y)));

    outColor = vec4(result / 16.0, 1.0);
}
