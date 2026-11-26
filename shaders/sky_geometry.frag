#version 450

// 3D 天空盒的片元着色器：只往 G-Buffer 写值，光照完全交给 LightingPass。
//
// 与 2D 天空盒（skybox_mrt.frag 写 outNormal = 0 作为"天空"标记）不同，这里
// 写真实法线 —— 远处的山与建筑要被太阳照亮、要参与雾与反射，所以它必须
// 像普通几何一样进入延迟光照。
//
// 两条防御（都踩过）：
// 1. 法线判断写 !(dot(n,n) > eps) 而不是 dot(n,n) < eps：零法线 normalize 出
//    NaN，而 NaN 与任何数比较都是 false，几何会在光照阶段整块消失。
// 2. 基色不用顶点色、也不用"法线朝上就取天空色"的写法：程序化远景常常没有
//    法线，回退到 +Y 之后颜色正好等于天空色，几何画了却看不出来。这里按
//    世界高度做地面到高空的渐变，颜色与天空明显区分。

layout(location = 0) in vec3 fragPosWorld;
layout(location = 1) in vec3 fragNormalWorld;
layout(location = 2) in vec3 fragColor;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outPBR;
layout(location = 4) out vec2 outVelocity;

void main() {
    vec3 normal = fragNormalWorld;
    if (!(dot(normal, normal) > 1.0e-12))
        normal = vec3(0.0, 1.0, 0.0);
    normal = normalize(normal);

    const vec3 lowTint = vec3(0.20, 0.26, 0.22);
    const vec3 highTint = vec3(0.44, 0.47, 0.50);
    const float heightBlend = clamp(fragPosWorld.y * 0.02 + 0.5, 0.0, 1.0);
    vec3 tint = mix(lowTint, highTint, heightBlend);

    outPosition = vec4(fragPosWorld, 1.0);
    outNormal = vec4(normal, 1.0);
    outAlbedo = vec4(tint, 1.0);
    outPBR = vec4(0.0, 1.0, 0.0, 1.0);
    // 这一类几何不产生屏幕空间运动（顶点随相机一起动，或本身就是无限远）。
    outVelocity = vec2(0.0);
}