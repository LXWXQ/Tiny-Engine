#version 450

// Bloom 降采样：13 抽样（Jimenez / COD 的经典 dual-filter 降采样核）。
//
// 为什么不用「一个双线性采样」或简单的 4 抽样：
//   直接 2x2 盒式降采样在 2 倍频处几乎不衰减，亮像素会退化成跳动的小方块，
//   上采样之后就是一圈圈的"光栅化摩尔纹"。13 抽样核在 [1,2] 与 [2,3] 倍
//   奈奎斯特区间都有衰减，模糊质量高得多，而且只需要 5 个双线性采样点。
//
// 坐标换算：目标 UV -> 源 UV
//   dest 的半个纹素 == src 的 1 个纹素（因为 src 恰好是 dest 的 2 倍）
//   于是 srcUV = (inUV - 0.5) * 2.0 + 0.5 = inUV * 2.0 - 0.5
// 下式的 A/B 就构造在这个坐标系里：A = 1 个源纹素，B = 2 个源纹素。

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec2 srcTexelSize;   // 1.0 / srcExtent
    int  karisAverage;   // 1 = 首级降采样，做 Karis 加权抑制萤火虫
    float pad;
} pc;

vec3 T(vec2 uv) { return texture(srcTex, uv).rgb; }

// Karis 平均：按 1/(1+luma) 加权。极亮的孤立像素（高光萤火虫）权重大幅下降，
// 否则它在后续几级里会一直被平均进来，形成一闪一闪的白色斑点。
vec3 KarisWeighted(vec3 a, vec3 b, vec3 c, vec3 d)
{
    float wa = 1.0 / (1.0 + dot(a, vec3(0.2126, 0.7152, 0.0722)));
    float wb = 1.0 / (1.0 + dot(b, vec3(0.2126, 0.7152, 0.0722)));
    float wc = 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
    float wd = 1.0 / (1.0 + dot(d, vec3(0.2126, 0.7152, 0.0722)));
    return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-5);
}

void main()
{
    vec2 uv = inUV * 2.0 - 0.5;
    vec2 A  = pc.srcTexelSize;          // 1 个源纹素
    vec2 B  = pc.srcTexelSize * 2.0;    // 2 个源纹素

    // 13 个抽样的标准位置（相对源像素中心）
    vec3 a = T(uv - B + vec2(0.0, A.y));
    vec3 b = T(uv - vec2(0.0, A.y));
    vec3 c = T(uv + B - vec2(0.0, A.y));
    vec3 d = T(uv - A);
    vec3 e = T(uv + A);
    vec3 f = T(uv - vec2(B.x, 0.0));
    vec3 g = T(uv);
    vec3 h = T(uv + vec2(B.x, 0.0));
    vec3 i = T(uv - B + vec2(0.0, A.y));
    vec3 j = T(uv + B - vec2(0.0, A.y));
    vec3 k = T(uv - B);
    vec3 l = T(uv + vec2(0.0, B.y));
    vec3 m = T(uv + B);

    // 13 个抽样按到中心的距离分成三档：角 1/32、边 2/32、心 4/32（权重和为 1）。
    // Karis 分支把中心的 2x2 单独拿出来再做一次亮度加权，
    // 这是原始论文为了同时兼顾"抗锯齿"和"抗萤火虫"的折中。
    vec3 result;
    if (pc.karisAverage != 0)
    {
        // 中心 2x2 用亮度加权，外围 9 个抽样按角/边分组做盒式平均。
        // 权重仍然是 0.5 : 0.125 x 4，只是中心那一组的成员被换成了加权版本。
        vec3 center = KarisWeighted(d, e, i, j);
        vec3 g1 = (a + b + g + f) * 0.25;
        vec3 g2 = (b + c + h + g) * 0.25;
        vec3 g3 = (f + g + l + k) * 0.25;
        vec3 g4 = (g + h + m + l) * 0.25;

        result = center * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    }
    else
    {
        result  = g * 0.125;                        // 心
        result += (a + c + k + m) * 0.03125;        // 角
        result += (b + f + h + l) * 0.0625;         // 边
        result += (d + e + i + j) * 0.125;          // 中心 2x2 的四个双线性点
    }

    outColor = vec4(result, 1.0);
}
