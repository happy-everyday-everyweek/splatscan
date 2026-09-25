#version 450

// 把计算着色器写出的模型画面搬上交换链。
// 模型图与窗口画幅很少一致（相机缓冲、窗口比例、系统栏都会影响），
// 所以这里按等比缩放采样，多出来的部分填背景色，不做任何拉伸。

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D modelImage;

// x、y 是采样范围缩放系数（都不超过 1），z、w 预留。
layout(push_constant) uniform BlitParams {
    vec2 uvScale;
    vec2 unused;
} params;

void main() {
    const vec2 centered = (uv - vec2(0.5)) / params.uvScale + vec2(0.5);
    if (centered.x < 0.0 || centered.x > 1.0 || centered.y < 0.0 || centered.y > 1.0) {
        outColor = vec4(0.06, 0.06, 0.08, 1.0);
        return;
    }
    outColor = vec4(texture(modelImage, centered).rgb, 1.0);
}
