#include "vio/pose_solver.h"

#include <algorithm>
#include <cmath>

namespace splatscan {

namespace {

Vec3 bearing(float x, float y, const CameraIntrinsics& camera) {
    return normalize(Vec3{(x - camera.cx) / camera.fx, (y - camera.cy) / camera.fy, 1.0f});
}

/** 对称 3x3 矩阵最小特征值对应的特征向量，用雅可比迭代。 */
Vec3 smallestEigenVector(float matrix[9]) {
    float a[9];
    for (int i = 0; i < 9; ++i) a[i] = matrix[i];
    float v[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    for (int sweep = 0; sweep < 24; ++sweep) {
        float offDiagonal = std::fabs(a[1]) + std::fabs(a[2]) + std::fabs(a[5]);
        if (offDiagonal < 1e-9f) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const int pq = p * 3 + q;
                if (std::fabs(a[pq]) < 1e-12f) continue;
                const float theta = (a[q * 3 + q] - a[p * 3 + p]) / (2.0f * a[pq]);
                const float t = (theta >= 0.0f ? 1.0f : -1.0f) /
                                (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
                const float c = 1.0f / std::sqrt(t * t + 1.0f);
                const float s = t * c;

                for (int k = 0; k < 3; ++k) {
                    const float akp = a[k * 3 + p];
                    const float akq = a[k * 3 + q];
                    a[k * 3 + p] = c * akp - s * akq;
                    a[k * 3 + q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const float apk = a[p * 3 + k];
                    const float aqk = a[q * 3 + k];
                    a[p * 3 + k] = c * apk - s * aqk;
                    a[q * 3 + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const float vkp = v[k * 3 + p];
                    const float vkq = v[k * 3 + q];
                    v[k * 3 + p] = c * vkp - s * vkq;
                    v[k * 3 + q] = s * vkp + c * vkq;
                }
            }
        }
    }

    int smallest = 0;
    for (int i = 1; i < 3; ++i) {
        if (a[i * 3 + i] < a[smallest * 3 + smallest]) smallest = i;
    }
    return normalize(Vec3{v[smallest], v[3 + smallest], v[6 + smallest]});
}

}  // namespace

PoseSolution PoseSolver::solveTranslation(const std::vector<FeatureTrack>& tracks,
                                          const CameraIntrinsics& camera, const Mat3& rotation,
                                          float scaleHint) {
    PoseSolution solution;
    solution.rotation = rotation;

    float matrix[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int used = 0;

    for (const FeatureTrack& track : tracks) {
        if (!track.valid) continue;
        const Vec3 first = rotation * bearing(track.previous.x, track.previous.y, camera);
        const Vec3 second = bearing(track.current.x, track.current.y, camera);
        const Vec3 a = cross(first, second);
        matrix[0] += a.x * a.x;
        matrix[1] += a.x * a.y;
        matrix[2] += a.x * a.z;
        matrix[4] += a.y * a.y;
        matrix[5] += a.y * a.z;
        matrix[8] += a.z * a.z;
        ++used;
    }
    matrix[3] = matrix[1];
    matrix[6] = matrix[2];
    matrix[7] = matrix[5];

    if (used < 8) {
        solution.valid = false;
        return solution;
    }

    Vec3 direction = smallestEigenVector(matrix);
    const float norm = length(direction);
    if (norm < 1e-6f || !std::isfinite(norm)) {
        solution.valid = false;
        return solution;
    }
    solution.translation = direction * (scaleHint / norm);

    // 内点统计：重投影误差小于阈值即算内点，用作跟踪稳定性指标
    int inliers = 0;
    for (const FeatureTrack& track : tracks) {
        if (!track.valid) continue;
        const Vec3 rotated = rotation * bearing(track.previous.x, track.previous.y, camera);
        const Vec3 second = bearing(track.current.x, track.current.y, camera);
        const Vec3 epipolar = second * 0.0f;  // 占位，避免未使用变量
        (void)epipolar;
        const float residual = std::fabs(dot(cross(second, rotated), solution.translation));
        if (residual < 0.05f * scaleHint) ++inliers;
    }
    solution.inliers = inliers;
    solution.inlierRatio = static_cast<float>(inliers) / static_cast<float>(used);
    solution.valid = inliers >= 8;
    return solution;
}

Mat3 PoseSolver::integrateRotation(const Mat3& rotation, const Vec3& gyro, float dtSeconds,
                                   const Vec3& gravityDirection) {
    const float angle = length(gyro) * dtSeconds;
    if (angle < 1e-6f) return rotation;

    const Vec3 axis = normalize(gyro);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;

    Mat3 delta;
    delta.m[0] = t * axis.x * axis.x + c;
    delta.m[1] = t * axis.x * axis.y - s * axis.z;
    delta.m[2] = t * axis.x * axis.z + s * axis.y;
    delta.m[3] = t * axis.x * axis.y + s * axis.z;
    delta.m[4] = t * axis.y * axis.y + c;
    delta.m[5] = t * axis.y * axis.z - s * axis.x;
    delta.m[6] = t * axis.x * axis.z - s * axis.y;
    delta.m[7] = t * axis.y * axis.z + s * axis.x;
    delta.m[8] = t * axis.z * axis.z + c;

    Mat3 updated = delta * rotation;

    // 用重力方向做慢修正：抑制陀螺漂移，同时不影响短时响应
    if (length(gravityDirection) > 0.5f) {
        const Vec3 up = normalize(gravityDirection);
        const Vec3 cameraUp = updated * Vec3{0.0f, -1.0f, 0.0f};
        const Vec3 correction = cross(cameraUp, up);
        const float weight = 0.02f;
        const float correctionAngle = length(correction) * weight;
        if (correctionAngle > 1e-5f) {
            const Vec3 axis = normalize(correction);
            const float cc = std::cos(correctionAngle);
            const float ss = std::sin(correctionAngle);
            const float tt = 1.0f - cc;
            Mat3 fix;
            fix.m[0] = tt * axis.x * axis.x + cc;
            fix.m[1] = tt * axis.x * axis.y - ss * axis.z;
            fix.m[2] = tt * axis.x * axis.z + ss * axis.y;
            fix.m[3] = tt * axis.x * axis.y + ss * axis.z;
            fix.m[4] = tt * axis.y * axis.y + cc;
            fix.m[5] = tt * axis.y * axis.z - ss * axis.x;
            fix.m[6] = tt * axis.x * axis.z - ss * axis.y;
            fix.m[7] = tt * axis.y * axis.z + ss * axis.x;
            fix.m[8] = tt * axis.z * axis.z + cc;
            updated = fix * updated;
        }
    }
    return updated;
}

}  // namespace splatscan