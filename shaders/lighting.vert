#version 450

layout(location = 0) out vec2 outUV;

void main() {
    // 自动生成一个覆盖全屏的三角形 (顶点坐标范围 -1 到 3)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0f + -1.0f, 0.0f, 1.0f);
}