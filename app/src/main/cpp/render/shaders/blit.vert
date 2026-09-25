#version 450

// 全屏三角形：不需要顶点缓冲，靠顶点序号直接生成覆盖屏幕的三角形。

layout(location = 0) out vec2 uv;

void main() {
    const vec2 position = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
