#pragma once

#include <cstdint>
#include <vector>

#include "core/math_types.h"

namespace splatscan {

/**
 * 训练用的高斯参数。
 *
 * 与完整 3DGS 的差别是刻意为之：每个高斯保留位置、屏幕对齐的两轴尺度、颜色与不透明度，
 * 去掉三轴各向异性与四元数旋转。这样每个高斯的训练态从约 2KB 降到约 72 字节，
 * 也正是「小内存设备也能跑」的来源。导入导出仍走完整的 17 属性布局，互不影响。
 */
struct GaussianParams {
    std::vector<float> position;   // 3 per gaussian
    std::vector<float> logScale;   // 2 per gaussian（屏幕对齐的两轴）
    std::vector<float> color;      // 3 per gaussian（球谐 DC 等价量）
    std::vector<float> opacity;    // 1 per gaussian（logit）

    std::vector<float> gradPosition;
    std::vector<float> gradLogScale;
    std::vector<float> gradColor;
    std::vector<float> gradOpacity;

    // Adam 状态
    std::vector<float> momentPosition;
    std::vector<float> variancePosition;
    std::vector<float> momentLogScale;
    std::vector<float> varianceLogScale;
    std::vector<float> momentColor;
    std::vector<float> varianceColor;
    std::vector<float> momentOpacity;
    std::vector<float> varianceOpacity;

    // 每个高斯被观察到的次数与累计误差，用于稠密化与剪枝
    std::vector<int32_t> observationCount;
    std::vector<float> meanError;

    int32_t count = 0;
    int32_t capacity = 0;
    int32_t step = 0;

    void ensureCapacity(int32_t required);
    int32_t add(const Vec3& position, float logScaleX, float logScaleY, const Vec3& color,
                float opacityLogit);
    void remove(int32_t index);
    void zeroGradients();
    void adamStep(float learningRate, float beta1, float beta2, float epsilon);
    void densify(float splitErrorThreshold, int32_t observationFloor, int32_t maxGaussians);
    void prune(float minOpacity, float maxScale);
    void resetOpacity();
    double memoryBytes() const;
};

}  // namespace splatscan
