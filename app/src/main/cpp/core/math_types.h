#pragma once

#include <cmath>

namespace splatscan {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& other) {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }

inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    if (len < 1e-8f) return {0.0f, 0.0f, 0.0f};
    return {v.x / len, v.y / len, v.z / len};
}

/** 行主序 3x3 矩阵，用于相机姿态。 */
struct Mat3 {
    float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    static Mat3 identity() { return {}; }

    Vec3 operator*(const Vec3& v) const {
        return {
            m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z,
        };
    }

    Mat3 transpose() const {
        Mat3 out;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out.m[r * 3 + c] = m[c * 3 + r];
            }
        }
        return out;
    }
};

inline Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a.m[r * 3 + k] * b.m[k * 3 + c];
            }
            out.m[r * 3 + c] = sum;
        }
    }
    return out;
}

/** 相机位姿：世界到相机。 */
struct Pose {
    Mat3 rotation = Mat3::identity();
    Vec3 translation{0.0f, 0.0f, 0.0f};

    Vec3 transform(const Vec3& world) const {
        const Vec3 rotated = rotation * world;
        return {rotated.x + translation.x, rotated.y + translation.y, rotated.z + translation.z};
    }
};

inline float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

}  // namespace splatscan
