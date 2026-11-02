#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

// 只用到全局集合里的 G-Buffer 位置/法线（set = 0）
layout(set = 0, binding = 2) uniform sampler2D texPosition;
layout(set = 0, binding = 3) uniform sampler2D texNormal;

layout(set = 1, binding = 0) uniform SSAOUbo {
    mat4 projection;
    mat4 view;
    vec4 samples[64];
    int kernelSize;
    float radius;
    float bias;
    float enabled;
} ubo;

layout(set = 1, binding = 1) uniform sampler2D texNoise;

void main()
{
    // D-2 修正：enabled 的判断必须放在最前面，而且**不能用 pass.setEnabled()
    // 来关闭 SSAO**。
    //
    // 原因：ssao 这张图是渲染图 createTexture 出来的瞬时资源，每帧从
    // UNDEFINED 布局开始（内容不被保证）。如果关 SSAO 时把整个 Pass 跳过，
    // 这张图整帧就没人写 —— 而 lighting.frag 里的 `ambient *= ssao` 是无条件
    // 执行的，于是环境光会被乘以一片未定义内容（实测表现为画面忽明忽暗）。
    // 验证层抓不到这个：从 UNDEFINED 转出来本身是合法的。
    //
    // 正确做法：Pass 永远执行，关闭时在着色器里输出中性白（1.0 = 无遮蔽），
    // 这样"关闭 SSAO"和"SSAO 输出 1.0"是同一个语义。
    if (ubo.enabled < 0.5) {
        outFragColor = vec4(1.0);
        return;
    }

    // G-Buffer 里存的是「世界空间」法线。
    // 长度为 0 是本引擎约定的"非几何"标记（天空盒写的就是 0），用来跳过遮蔽计算。
    vec4 normalSample = texture(texNormal, inUV);
    vec3 normalWorld = normalSample.xyz;
    // .w is an SSAO policy mask: 0 = terrain / grid / skybox opt out.
    if (normalSample.w < 0.5 || length(normalWorld) < 0.1) {
        outFragColor = vec4(1.0);
        return;
    }

    vec2 screenSize = textureSize(texPosition, 0);
    vec2 noiseScale = screenSize / 4.0;

    vec3 posWorld = texture(texPosition, inUV).xyz;

    // =================================================================
    // 关键修正：遮挡判断必须在「视图空间」进行。
    //
    // 原实现在世界空间直接比较 z —— 那只有当相机恰好朝向 -Z 时才成立，
    // 相机一转，遮蔽就随朝向乱变（侧面失效、地面出现方向性条纹）。
    // 正确做法：把位置和法线都转到视图空间，再比较视图空间的深度。
    // =================================================================
    vec3 viewPos = (ubo.view * vec4(posWorld, 1.0)).xyz;
    vec3 N       = normalize((ubo.view * vec4(normalWorld, 0.0)).xyz);

    // 视图空间 TBN：用 4x4 噪声纹理绕法线随机旋转，打散规则采样花纹
    vec3 randomVec = texture(texNoise, inUV * noiseScale).xyz;
    vec3 tangent   = normalize(randomVec - N * dot(randomVec, N));
    vec3 bitangent = cross(N, tangent);
    mat3 TBN       = mat3(tangent, bitangent, N);

    float occlusion = 0.0;
    // 采样核容量 64，与 C++ 侧 SSAOPass::generateSampleKernel() 保持一致
    int actualKernelSize = clamp(ubo.kernelSize, 1, 64);

    for (int i = 0; i < actualKernelSize; ++i)
    {
        // 半球采样点（视图空间）
        vec3 samplePos = viewPos + (TBN * ubo.samples[i].xyz) * ubo.radius;

        // 用 projection（而不是 projectionView）投影回屏幕
        vec4 offset = ubo.projection * vec4(samplePos, 1.0);
        if (offset.w <= 0.0001) continue;            // 采样点落在相机后方或相机平面上
        vec3 ndc = offset.xyz / offset.w;
        vec2 sampleUV = ndc.xy * 0.5 + 0.5;          // Vulkan NDC -> UV（y 轴同为向下，无需翻转）

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) continue;

        // 该屏幕像素上"实际几何"的视图空间深度
        vec3 sampleWorld  = texture(texPosition, sampleUV).xyz;
        float sampleViewZ = (ubo.view * vec4(sampleWorld, 1.0)).z;

        // 视图空间相机朝 -Z：z 越大表示离相机越近。
        // 若该处几何比采样点更靠近相机，则采样点被遮挡。
        float rangeCheck = smoothstep(0.0, 1.0, ubo.radius / (abs(viewPos.z - sampleViewZ) + 0.0001));
        occlusion += (sampleViewZ >= samplePos.z + ubo.bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(actualKernelSize));
    outFragColor = vec4(occlusion, occlusion, occlusion, 1.0);
}
