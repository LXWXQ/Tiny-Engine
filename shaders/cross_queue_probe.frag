#version 450

// TD-02 跨队列验收 graphics 着色器。
// mode 0：读取 compute 生成的 SSBO，把校验和写入 host-visible buffer（C->G）。
// mode 1：把一帧常量写进 storage image，供紧接着的 compute pass 读取（G->C）。
// 输出颜色附件只是为了满足动态渲染对 graphics pipeline 的要求，不参与验证。

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std430) readonly buffer ProbeData
{
    uint values[];
} dataBuffer;

layout(set = 0, binding = 1, std430) writeonly buffer ProbeResult
{
    uint magic;
    uint frameNumber;
    uint direction;
    uint checksum;
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
} resultBuffer;

layout(set = 0, binding = 2, r32ui) uniform uimage2D probeImage;

layout(push_constant) uniform Push
{
    uint mode;
    uint frameNumber;
    uint count;
    uint pad;
} pc;

void main()
{
    if (pc.mode == 0u)
    {
        // 只让左上角像素做一次串行求和，避免同一结果被 64x64 个 fragment 覆盖。
        if (ivec2(gl_FragCoord.xy) == ivec2(0, 0))
        {
            uint sum = 0u;
            for (uint i = 0u; i < pc.count; ++i)
                sum += dataBuffer.values[i];

            resultBuffer.magic = 0x54D02000u;
            resultBuffer.frameNumber = pc.frameNumber;
            resultBuffer.direction = 0u;
            resultBuffer.checksum = sum;
            resultBuffer.count = pc.count;
            resultBuffer.pad0 = 0u;
            resultBuffer.pad1 = 0u;
            resultBuffer.pad2 = 0u;

            imageStore(probeImage, ivec2(0, 0), uvec4(sum, 0u, 0u, 1u));
        }
        outColor = vec4(0.0, 1.0, 0.0, 1.0);
        return;
    }

    // G->C 场景：每个像素都写同一个常量，compute 端会逐像素求和。
    const uint value = 0x10000000u + pc.frameNumber;
    imageStore(probeImage, ivec2(gl_FragCoord.xy), uvec4(value, 7u, 11u, 13u));
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
}