#version 450

// 把计算着色器写出的模型画面搬上交换链。

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D modelImage;

void main() {
    outColor = vec4(texture(modelImage, uv).rgb, 1.0);
}
