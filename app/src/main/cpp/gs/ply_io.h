#pragma once

#include <string>

#include "core/math_types.h"
#include "gs/gaussian_params.h"

namespace splatscan {

/**
 * 二进制 PLY 的属性顺序（17 个 float）。写出与读回都只认这一份定义，
 * 任何一方改动都会同时作用到另一方，避免两处各自维护顺序。
 */
enum PlyProperty : int32_t {
    kPlyX = 0,
    kPlyY = 1,
    kPlyZ = 2,
    kPlyNormalX = 3,
    kPlyNormalY = 4,
    kPlyNormalZ = 5,
    kPlyRed = 6,
    kPlyGreen = 7,
    kPlyBlue = 8,
    kPlyOpacity = 9,
    kPlyScale0 = 10,
    kPlyScale1 = 11,
    kPlyScale2 = 12,
    kPlyRot0 = 13,
    kPlyRot1 = 14,
    kPlyRot2 = 15,
    kPlyRot3 = 16,
    kPlyPropertyCount = 17,
};

/**
 * 读取 3D 高斯泼溅的二进制 PLY（17 个 float 属性）。
 *
 * 只认我们自己写出的布局与最常见的社区布局：位置、法线占位、球谐 DC 三通道、
 * 不透明度 logit、三个对数尺度、四元数。四元数在这里被丢弃（训练态不带旋转），
 * 三个尺度里取前两个作为屏幕对齐的两轴尺度。
 */
bool readPlyModel(const std::string& path, GaussianParams& out, Vec3& boundsCenter,
                  float& boundsRadius);

/** 写出与 readPlyModel 同一套布局的二进制 PLY。四元数写单位值，第三轴尺度复用第二轴。 */
bool writePlyModel(const std::string& path, const GaussianParams& model);

}  // namespace splatscan