#pragma once

#include <vector>

#include "core/math_types.h"
#include "gs/raster_cpu.h"
#include "vio/feature_tracker.h"

namespace splatscan {

struct PoseSolution {
    Mat3 rotation = Mat3::identity();
    Vec3 translation{0.0f, 0.0f, 0.0f};
    float inlierRatio = 0.0f;
    int inliers = 0;
    bool valid = false;
};

/**
 * 位姿求解：旋转来自 IMU，平移由特征对应关系线性求解（对极约束的最小奇异向量）。
 *
 * 平移的绝对尺度在单目下不可观测，这里用 IMU 的加速度短时积分给出近似尺度，
 * 剩下的误差由高斯位置自身的优化吸收。
 */
class PoseSolver {
public:
    static PoseSolution solveTranslation(const std::vector<FeatureTrack>& tracks,
                                         const CameraIntrinsics& camera, const Mat3& rotation,
                                         float scaleHint);

    /** 以惯性测量推进旋转：陀螺积分 + 重力方向慢修正。 */
    static Mat3 integrateRotation(const Mat3& rotation, const Vec3& gyro, float dtSeconds,
                                  const Vec3& gravityDirection);
};

}  // namespace splatscan