#version 450

// P2 第十四步：阴影的 GPU 批量路径。
// 与 shadow.vert 的区别：模型矩阵不再走 push constant，而是按「级联紧凑 remap」
// 从 set 3 的实例表取 —— 一条 indirect 覆盖合并网格里的所有静态子网格。
// 光空间矩阵仍每级联推一次。
layout(location = 0) in vec3 inPosition;

struct InstanceRecord
{
    mat4 model;
    mat4 normal;
    uvec4 flags;
    vec4 misc;
    vec4 uvTransform;
    vec4 boundsMin;
    vec4 boundsMax;
    uvec4 draw;
    mat4 previousModel;   // 本步不读，只为与顶点缓冲的 288 字节行距一致
};

layout(set = 3, binding = 0) readonly buffer InstanceBuffer { InstanceRecord instances[]; };
layout(set = 3, binding = 1) readonly buffer InstanceRemapBuffer { uint instanceRemap[]; };

layout(push_constant) uniform Push
{
    mat4 lightViewProjection;
} push;

void main()
{
    const uint instanceIndex = instanceRemap[gl_InstanceIndex];
    gl_Position = push.lightViewProjection * instances[instanceIndex].model * vec4(inPosition, 1.0);
}
