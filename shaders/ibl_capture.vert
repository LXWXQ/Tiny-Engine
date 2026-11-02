#version 450

// IBL 烘焙专用的顶点着色器：全屏三角形 + 输出 [0,1] 的 UV。
// 立方体面方向在片元着色器里由 push constant 的 faceIndex 推出，
// 因此这里不需要顶点缓冲，也不需要立方体网格。
layout(location = 0) out vec2 outUV;

void main()
{
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}