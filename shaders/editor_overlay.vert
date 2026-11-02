#version 450

// 编辑器线框（包围盒）顶点着色器。
// 顶点已经是**世界空间**坐标 —— 包围盒本来就在世界空间算好的，
// 不需要 model 矩阵，少一次乘法也少一个出错的地方。
layout(location = 0) in vec3 position;

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 color;
} push;

void main()
{
    gl_Position = push.viewProjection * vec4(position, 1.0);
}
