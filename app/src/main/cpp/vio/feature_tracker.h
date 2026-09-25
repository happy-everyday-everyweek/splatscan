#pragma once

#include <cstdint>
#include <vector>

namespace splatscan {

struct FeaturePoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct FeatureTrack {
    FeaturePoint previous;
    FeaturePoint current;
    bool valid = false;
};

/**
 * 轻量特征跟踪：FAST 角点检测 + 金字塔 Lucas-Kanade 光流。
 *
 * 自己写而不用现成库，是为了保持体积与依赖干净；精度只服务于高斯重建所需的位姿，
 * 不追求 SLAM 级别的一致性，位姿的旋转部分由 IMU 提供。
 */
class FeatureTracker {
public:
    void detect(const uint8_t* gray, int width, int height, int stride,
                std::vector<FeaturePoint>& out, int maxFeatures, int minDistance = 12) const;

    /** 跟踪上一帧的特征点，返回成功跟踪的数量。 */
    int track(const uint8_t* previousGray, const uint8_t* currentGray, int width, int height,
              int stride, std::vector<FeatureTrack>& tracks, int window = 8, int levels = 2) const;

private:
    static float sampleBilinear(const uint8_t* image, int width, int height, int stride, float x,
                                float y);
};

}  // namespace splatscan