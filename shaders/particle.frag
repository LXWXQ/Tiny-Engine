#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main()
{
    // 径向软圆：中心全亮，边缘平滑消失，避免方形 billboard 的硬边。
    const float radiusSquared = dot(fragUV, fragUV);
    const float mask = 1.0 - smoothstep(0.20, 1.0, radiusSquared);
    const float alpha = clamp(fragColor.a, 0.0, 1.0) * mask;
    if (alpha <= 0.001)
        discard;

    // 略微提亮，让火焰/火花进入 Bloom 的阈值以上；最终强度仍受颜色控制。
    outColor = vec4(fragColor.rgb * 1.4, alpha);
}
