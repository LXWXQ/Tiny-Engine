#version 450

// F6 阴影：深度专用通道。只读位置，模型矩阵与光空间矩阵都走 push constant，
// 所以这条管线不绑任何描述符集 —— 阴影 pass 与材质/实例表完全解耦。

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform Push
{
    mat4 model;
    mat4 lightViewProjection;
} push;

void main()
{
    gl_Position = push.lightViewProjection * push.model * vec4(inPosition, 1.0);
}
