#version 450

// Bloom 第一阶段：从 HDR 场景图里「保留颜色、只压暗」地提取亮部。
//
// 为什么不能简单写 step(threshold, luma) * color：
//   硬阈值会让亮度刚好在阈值附近的像素在帧间/像素间突然从 0 跳到 1，
//   在辉光的边界上留下一圈明显的"硬边"。这里用软膝盖（soft knee）
//   在 [threshold - knee, threshold + knee] 上做二次过渡，边界是连续的。

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrScene;

// threshold / knee / intensity 的顺序与 C++ 侧 BloomBrightPush 严格一致
layout(push_constant) uniform Push {
    float threshold;
    float knee;
    float intensity;
} pc;

void main()
{
    vec3 c = texture(hdrScene, inUV).rgb;

    // Rec.709 亮度 —— 不能直接用 RGB 最大值，否则纯蓝的高光会被判定成暗部
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));

    float knee = max(pc.knee, 1e-4);
    // 二次曲线在 [t-k, t+k] 上从 0 平滑升到 1（COD 系的经典 soft-knee 近似）
    float soft = clamp(luma - pc.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    // luma > t+k 时 soft 恰好等于 luma - t，于是 weight = 1（完全保留）
    float weight = max(soft, luma - pc.threshold) / max(luma, 1e-5);

    outColor = vec4(c * weight * pc.intensity, 1.0);
}
