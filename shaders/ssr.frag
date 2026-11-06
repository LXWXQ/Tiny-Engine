#version 450

// F6-01：水面近场屏幕空间反射。
//
// 输入只有 HDR 颜色 + G-Buffer 世界坐标，没有法线/深度纹理：
//   * 水面是解析平面（y = waterLevel），不需要深度就能求出视线与水面的交点；
//   * 反射射线从水面交点沿 reflect(viewDir, up) 前进，逐步投影回屏幕，
//     用 G-Buffer 位置和射线做「点到射线距离」判定，命中就取 HDR 颜色。
//
// 输出 alpha 是置信度：0 = 没有命中/边缘，WaterPass 会回退到平面反射/IBL。
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUbo
{
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

layout(set = 0, binding = 2) uniform sampler2D inPosition;
layout(set = 1, binding = 0) uniform sampler2D hdrScene;

layout(push_constant) uniform SSRPush
{
    mat4 inverseProjectionView;
    vec4 params;   // x = maxDistance, y = thickness, z = stepCount, w = enabled
    vec4 water;    // x = waterLevel, y = 1/width, z = 1/height
} push;

float edgeFade(vec2 uv)
{
    // 屏幕边缘的命中点通常是「擦边」的，容易把屏幕外的信息拉伸成条纹。
    float ex = smoothstep(0.0, 0.05, uv.x) * smoothstep(0.0, 0.05, 1.0 - uv.x);
    float ey = smoothstep(0.0, 0.05, uv.y) * smoothstep(0.0, 0.05, 1.0 - uv.y);
    return ex * ey;
}

void main()
{
    vec3 camera = ubo.cameraPos.xyz;
    float level = push.water.x;
    float maxDistance = max(push.params.x, 1.0);

    // 相机在水下/贴水面时没有可反射的水面，直接输出空。
    if (camera.y <= level + 0.05)
    {
        outColor = vec4(0.0);
        return;
    }

    // 从近裁剪面上的像素反投影出视线方向。Vulkan 的 NDC y 向下，
    // inUV 也是左上为原点，两者方向一致，不需要翻转。
    vec2 ndc = inUV * 2.0 - 1.0;
    vec4 nearPoint = push.inverseProjectionView * vec4(ndc, 0.0, 1.0);
    vec3 viewDir = normalize(nearPoint.xyz / nearPoint.w - camera);

    // 视线必须朝下才能打到水面。
    if (viewDir.y >= -1e-4)
    {
        outColor = vec4(0.0);
        return;
    }

    float waterT = (level - camera.y) / viewDir.y;
    if (waterT <= 0.0)
    {
        outColor = vec4(0.0);
        return;
    }

    vec3 waterPos = camera + viewDir * waterT;

    // 这一像素如果已经是不透明几何（离相机比水面更近），就不需要算反射；
    // 水面像素的 G-Buffer 位置是水后面的地形/天空，因此不会误杀。
    vec3 scenePos = texture(inPosition, inUV).xyz;
    if (dot(scenePos, scenePos) > 1e-6)
    {
        float sceneDistance = length(scenePos - camera);
        if (sceneDistance < waterT - 0.5)
        {
            outColor = vec4(0.0);
            return;
        }
    }

    vec3 rayDir = reflect(viewDir, vec3(0.0, 1.0, 0.0));
    const int kSteps = 48;
    float prevT = 0.0;

    for (int i = 0; i < kSteps; ++i)
    {
        // 近处步长小、远处步长大：0.35m 起步，按 5.5% 指数增长，
        // 48 步足以覆盖 200m，同时近场的岸线细节不会被跳过。
        float t = max(0.35, prevT + max(0.5, prevT * 0.055));
        if (t > maxDistance)
            break;
        prevT = t;

        vec3 rayPos = waterPos + rayDir * t;
        vec4 clip = ubo.projectionView * vec4(rayPos, 1.0);
        if (clip.w <= 1e-5)
            break;

        vec2 hitUV = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(hitUV, vec2(0.0))) || any(greaterThan(hitUV, vec2(1.0))))
            break;

        vec3 hitPos = texture(inPosition, hitUV).xyz;
        if (dot(hitPos, hitPos) < 1e-6)
            continue;   // 天空/未写入：SSR 不反射天空，交给 IBL

        vec3 delta = hitPos - waterPos;
        float along = dot(delta, rayDir);
        float perp = length(delta - rayDir * along);
        float thickness = max(push.params.y, t * 0.02);

        // along 限制在 [0, t]：既不能是水面后面的东西（along < 0），
        // 也不能是这一步行进之后才出现的几何（along > t）。
        if (along > 0.0 && along <= t && perp < thickness * 2.5)
        {
            float confidence = edgeFade(inUV);
            confidence *= 1.0 - smoothstep(0.7 * maxDistance, maxDistance, t);
            outColor = vec4(texture(hdrScene, hitUV).rgb, confidence);
            return;
        }
    }

    outColor = vec4(0.0);
}
