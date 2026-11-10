#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 2) flat in uint fragMaterialId;

layout(location = 0) out vec4 outColor;

// set 1 是 DescriptorManager 的 bindless 材质数组；玻璃/水面也走同一套。
layout(set = 1, binding = 0) uniform sampler2D albedoMaps[];

void main()
{
    // 0xFFFFFFFF = 没有贴图：保留早先的程序化圆形软遮罩。
    const bool useTexture = fragMaterialId != 0xFFFFFFFFu;
    vec4 texel = vec4(1.0);
    if (useTexture)
    {
        // 图像首行是顶部：billboard 的 +Y 对应 v=0，避免上下颠倒。
        const vec2 uv = vec2(fragUV.x * 0.5 + 0.5, 0.5 - fragUV.y * 0.5);
        texel = texture(albedoMaps[nonuniformEXT(fragMaterialId)], uv);
    }

    float alpha = 0.0;
    if (useTexture)
    {
        alpha = clamp(fragColor.a, 0.0, 1.0) * texel.a;
    }
    else
    {
        const float radiusSquared = dot(fragUV, fragUV);
        const float mask = 1.0 - smoothstep(0.20, 1.0, radiusSquared);
        alpha = clamp(fragColor.a, 0.0, 1.0) * mask;
    }
    if (alpha <= 0.001)
        discard;

    // 略微提亮，让火焰/火花进入 Bloom 的阈值以上；最终强度仍受颜色控制。
    outColor = vec4(fragColor.rgb * texel.rgb * 1.4, alpha);
}
