#pragma once

#include <string>

#include "core/math_types.h"
#include "gs/gaussian_params.h"

namespace splatscan {

/**
 * 读取 3D 高斯泼溅的二进制 PLY（17 个 float 属性）。
 *
 * 只认我们自己写出的布局与最常见的社区布局：位置、法线占位、球谐 DC 三通道、
 * 不透明度 logit、三个对数尺度、四元数。四元数在这里被丢弃（训练态不带旋转），
 * 三个尺度里取前两个作为屏幕对齐的两轴尺度。
 */
bool readPlyModel(const std::string& path, GaussianParams& out, Vec3& boundsCenter,
                  float& boundsRadius);

}  // namespace splatscan