#include "gs/gaussian_params.h"

#include <algorithm>
#include <cmath>

namespace splatscan {

namespace {
constexpr int32_t kPositionStride = 3;
constexpr int32_t kScaleStride = 2;
constexpr int32_t kColorStride = 3;
}  // namespace

void GaussianParams::ensureCapacity(int32_t required) {
    if (required <= capacity) return;
    int32_t next = capacity > 0 ? capacity : 256;
    while (next < required) next *= 2;
    position.resize(static_cast<size_t>(next) * kPositionStride, 0.0f);
    logScale.resize(static_cast<size_t>(next) * kScaleStride, 0.0f);
    color.resize(static_cast<size_t>(next) * kColorStride, 0.0f);
    opacity.resize(static_cast<size_t>(next), 0.0f);

    gradPosition.resize(position.size(), 0.0f);
    gradLogScale.resize(logScale.size(), 0.0f);
    gradColor.resize(color.size(), 0.0f);
    gradOpacity.resize(opacity.size(), 0.0f);

    momentPosition.resize(position.size(), 0.0f);
    variancePosition.resize(position.size(), 0.0f);
    momentLogScale.resize(logScale.size(), 0.0f);
    varianceLogScale.resize(logScale.size(), 0.0f);
    momentColor.resize(color.size(), 0.0f);
    varianceColor.resize(color.size(), 0.0f);
    momentOpacity.resize(opacity.size(), 0.0f);
    varianceOpacity.resize(opacity.size(), 0.0f);

    observationCount.resize(next, 0);
    meanError.resize(next, 0.0f);
    capacity = next;
}

int32_t GaussianParams::add(const Vec3& pos, float logScaleX, float logScaleY, const Vec3& col,
                            float opacityLogit) {
    ensureCapacity(count + 1);
    const int32_t index = count;
    position[index * kPositionStride + 0] = pos.x;
    position[index * kPositionStride + 1] = pos.y;
    position[index * kPositionStride + 2] = pos.z;
    logScale[index * kScaleStride + 0] = logScaleX;
    logScale[index * kScaleStride + 1] = logScaleY;
    color[index * kColorStride + 0] = col.x;
    color[index * kColorStride + 1] = col.y;
    color[index * kColorStride + 2] = col.z;
    opacity[index] = opacityLogit;
    observationCount[index] = 0;
    meanError[index] = 0.0f;
    count = index + 1;
    return index;
}

void GaussianParams::remove(int32_t index) {
    if (index < 0 || index >= count) return;
    const int32_t last = count - 1;
    if (index != last) {
        for (int32_t k = 0; k < kPositionStride; ++k) {
            position[index * kPositionStride + k] = position[last * kPositionStride + k];
            gradPosition[index * kPositionStride + k] = 0.0f;
            momentPosition[index * kPositionStride + k] = 0.0f;
            variancePosition[index * kPositionStride + k] = 0.0f;
        }
        for (int32_t k = 0; k < kScaleStride; ++k) {
            logScale[index * kScaleStride + k] = logScale[last * kScaleStride + k];
            gradLogScale[index * kScaleStride + k] = 0.0f;
            momentLogScale[index * kScaleStride + k] = 0.0f;
            varianceLogScale[index * kScaleStride + k] = 0.0f;
        }
        for (int32_t k = 0; k < kColorStride; ++k) {
            color[index * kColorStride + k] = color[last * kColorStride + k];
            gradColor[index * kColorStride + k] = 0.0f;
            momentColor[index * kColorStride + k] = 0.0f;
            varianceColor[index * kColorStride + k] = 0.0f;
        }
        opacity[index] = opacity[last];
        gradOpacity[index] = 0.0f;
        momentOpacity[index] = 0.0f;
        varianceOpacity[index] = 0.0f;
        observationCount[index] = observationCount[last];
        meanError[index] = meanError[last];
    }
    count = last;
}

void GaussianParams::zeroGradients() {
    std::fill(gradPosition.begin(), gradPosition.end(), 0.0f);
    std::fill(gradLogScale.begin(), gradLogScale.end(), 0.0f);
    std::fill(gradColor.begin(), gradColor.end(), 0.0f);
    std::fill(gradOpacity.begin(), gradOpacity.end(), 0.0f);
}

void GaussianParams::adamStep(float learningRate, float beta1, float beta2, float epsilon) {
    ++step;
    const float bias1 = 1.0f - std::pow(beta1, static_cast<float>(step));
    const float bias2 = 1.0f - std::pow(beta2, static_cast<float>(step));
    const float stepSize = learningRate * std::sqrt(bias2) / bias1;

    for (int32_t i = 0; i < count; ++i) {
        for (int32_t k = 0; k < kPositionStride; ++k) {
            const size_t index = static_cast<size_t>(i) * kPositionStride + k;
            float& grad = gradPosition[index];
            momentPosition[index] = beta1 * momentPosition[index] + (1.0f - beta1) * grad;
            variancePosition[index] =
                beta2 * variancePosition[index] + (1.0f - beta2) * grad * grad;
            position[index] -=
                stepSize * momentPosition[index] / (std::sqrt(variancePosition[index]) + epsilon);
        }
        for (int32_t k = 0; k < kScaleStride; ++k) {
            const size_t index = static_cast<size_t>(i) * kScaleStride + k;
            float& grad = gradLogScale[index];
            momentLogScale[index] = beta1 * momentLogScale[index] + (1.0f - beta1) * grad;
            varianceLogScale[index] =
                beta2 * varianceLogScale[index] + (1.0f - beta2) * grad * grad;
            logScale[index] -=
                stepSize * momentLogScale[index] / (std::sqrt(varianceLogScale[index]) + epsilon);
        }
        for (int32_t k = 0; k < kColorStride; ++k) {
            const size_t index = static_cast<size_t>(i) * kColorStride + k;
            float& grad = gradColor[index];
            momentColor[index] = beta1 * momentColor[index] + (1.0f - beta1) * grad;
            varianceColor[index] = beta2 * varianceColor[index] + (1.0f - beta2) * grad * grad;
            color[index] -= stepSize * momentColor[index] / (std::sqrt(varianceColor[index]) + epsilon);
        }
        const size_t index = static_cast<size_t>(i);
        float& grad = gradOpacity[index];
        momentOpacity[index] = beta1 * momentOpacity[index] + (1.0f - beta1) * grad;
        varianceOpacity[index] = beta2 * varianceOpacity[index] + (1.0f - beta2) * grad * grad;
        opacity[index] -= stepSize * momentOpacity[index] / (std::sqrt(varianceOpacity[index]) + epsilon);
    }

    // 尺度与不透明度保持在可解释范围内
    for (int32_t i = 0; i < count; ++i) {
        logScale[i * kScaleStride + 0] = clampf(logScale[i * kScaleStride + 0], -12.0f, 2.0f);
        logScale[i * kScaleStride + 1] = clampf(logScale[i * kScaleStride + 1], -12.0f, 2.0f);
        opacity[i] = clampf(opacity[i], -8.0f, 8.0f);
        for (int32_t k = 0; k < kColorStride; ++k) {
            color[i * kColorStride + k] = clampf(color[i * kColorStride + k], -3.0f, 3.0f);
        }
    }
}

void GaussianParams::densify(float splitErrorThreshold, int32_t observationFloor,
                             int32_t maxGaussians) {
    if (count >= maxGaussians) return;
    const int32_t original = count;
    for (int32_t i = 0; i < original && count < maxGaussians; ++i) {
        if (observationCount[i] < observationFloor) continue;
        if (meanError[i] < splitErrorThreshold) continue;
        const Vec3 pos{position[i * kPositionStride + 0], position[i * kPositionStride + 1],
                       position[i * kPositionStride + 2]};
        const float jitter = 0.5f * std::exp(logScale[i * kScaleStride + 0]);
        const float ox = (static_cast<float>((i % 3) - 1)) * jitter;
        const float oy = (static_cast<float>(((i / 3) % 3) - 1)) * jitter;
        const Vec3 gaussianColor{color[i * kColorStride + 0], color[i * kColorStride + 1],
                                 color[i * kColorStride + 2]};
        add({pos.x + ox, pos.y + oy, pos.z}, logScale[i * kScaleStride + 0] - 0.3f,
            logScale[i * kScaleStride + 1] - 0.3f, gaussianColor, opacity[i] - 0.5f);
        observationCount[i] = 0;
        meanError[i] = 0.0f;
    }
}

void GaussianParams::prune(float minOpacity, float maxScale) {
    for (int32_t i = count - 1; i >= 0; --i) {
        const float alpha = 1.0f / (1.0f + std::exp(-opacity[i]));
        const float scaleX = std::exp(logScale[i * kScaleStride + 0]);
        const float scaleY = std::exp(logScale[i * kScaleStride + 1]);
        if (alpha < minOpacity || scaleX > maxScale || scaleY > maxScale) {
            remove(i);
        }
    }
}

void GaussianParams::resetOpacity() {
    for (int32_t i = 0; i < count; ++i) {
        opacity[i] = std::min(opacity[i], -1.5f);
    }
}

double GaussianParams::memoryBytes() const {
    const double perGaussian = static_cast<double>(kPositionStride * 5 + kScaleStride * 5 +
                                                   kColorStride * 5 + 1 * 5) * sizeof(float) +
                               sizeof(int32_t) + sizeof(float);
    return perGaussian * static_cast<double>(capacity);
}

}  // namespace splatscan
