#include "vio/feature_tracker.h"

#include <algorithm>
#include <cmath>

namespace splatscan {

namespace {

inline float centerScore(const uint8_t* image, int width, int height, int stride, int x, int y) {
    const int offset = y * stride + x;
    const int center = image[offset];
    int brighter = 0;
    int darker = 0;
    const int threshold = 18;
    // FAST-9 的近似：只检查上下左右四个方向的八个点
    const int offsets[8][2] = {{0, -3}, {0, 3}, {-3, 0}, {3, 0}, {-2, -2}, {2, -2}, {-2, 2}, {2, 2}};
    for (const auto& item : offsets) {
        const int sx = x + item[0];
        const int sy = y + item[1];
        if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
        const int value = image[sy * stride + sx];
        if (value > center + threshold) ++brighter;
        if (value < center - threshold) ++darker;
    }
    const int score = std::max(brighter, darker);
    return score >= 6 ? center : -1;
}

}  // namespace

float FeatureTracker::sampleBilinear(const uint8_t* image, int width, int height, int stride,
                                     float x, float y) {
    if (x < 0.0f || y < 0.0f || x > static_cast<float>(width - 2) ||
        y > static_cast<float>(height - 2)) {
        return 0.0f;
    }
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float v00 = image[y0 * stride + x0];
    const float v10 = image[y0 * stride + x0 + 1];
    const float v01 = image[(y0 + 1) * stride + x0];
    const float v11 = image[(y0 + 1) * stride + x0 + 1];
    const float top = v00 * (1.0f - fx) + v10 * fx;
    const float bottom = v01 * (1.0f - fx) + v11 * fx;
    return top * (1.0f - fy) + bottom * fy;
}

void FeatureTracker::detect(const uint8_t* gray, int width, int height, int stride,
                            std::vector<FeaturePoint>& out, int maxFeatures, int minDistance) const {
    out.clear();
    const int margin = 4;
    // 均匀网格采样，避免特征全部挤在纹理最密的角落
    const int cell = std::max(minDistance * 2, 8);
    for (int y = margin; y < height - margin; y += cell) {
        for (int x = margin; x < width - margin; x += cell) {
            if (static_cast<int>(out.size()) >= maxFeatures) return;
            int bestScore = 0;
            int bestX = -1;
            int bestY = -1;
            for (int dy = 0; dy < cell && y + dy < height - margin; dy += 2) {
                for (int dx = 0; dx < cell && x + dx < width - margin; dx += 2) {
                    const float score = centerScore(gray, width, height, stride, x + dx, y + dy);
                    if (score > static_cast<float>(bestScore)) {
                        bestScore = static_cast<int>(score);
                        bestX = x + dx;
                        bestY = y + dy;
                    }
                }
            }
            if (bestX >= 0) {
                out.push_back({static_cast<float>(bestX), static_cast<float>(bestY)});
            }
        }
    }
}

int FeatureTracker::track(const uint8_t* previousGray, const uint8_t* currentGray, int width,
                          int height, int stride, std::vector<FeatureTrack>& tracks, int window,
                          int levels) const {
    int tracked = 0;
    const int half = window / 2;

    // 金字塔用「按块平均」现场采样得到，不额外分配图像。
    // 关键约束：上一帧与当前帧必须在同一层、同一尺度上采样，
    // 否则残差项里混进两个尺度的图像，解出来的位移没有意义（这正是之前跟踪全废的原因）。
    const auto sampleAt = [&](const uint8_t* image, float x, float y, int level) -> float {
        if (level <= 0) return sampleBilinear(image, width, height, stride, x, y);
        const int block = 1 << level;
        const int baseX = static_cast<int>(x * static_cast<float>(block));
        const int baseY = static_cast<int>(y * static_cast<float>(block));
        float sum = 0.0f;
        int count = 0;
        for (int dy = 0; dy < block; ++dy) {
            const int sy = baseY + dy;
            if (sy < 0 || sy >= height) continue;
            for (int dx = 0; dx < block; ++dx) {
                const int sx = baseX + dx;
                if (sx < 0 || sx >= width) continue;
                sum += static_cast<float>(image[sy * stride + sx]);
                ++count;
            }
        }
        return count > 0 ? sum / static_cast<float>(count) : 0.0f;
    };

    for (FeatureTrack& track : tracks) {
        track.valid = false;
        float px = track.previous.x;
        float py = track.previous.y;
        bool ok = true;

        for (int level = levels - 1; level >= 0 && ok; --level) {
            const float scale = static_cast<float>(1 << level);
            float lx = px / scale;
            float ly = py / scale;

            for (int iteration = 0; iteration < 8; ++iteration) {
                float sumIxIx = 0.0f;
                float sumIyIy = 0.0f;
                float sumIxIy = 0.0f;
                float sumIxIt = 0.0f;
                float sumIyIt = 0.0f;

                for (int dy = -half; dy <= half; ++dy) {
                    for (int dx = -half; dx <= half; ++dx) {
                        const float gx = lx + static_cast<float>(dx);
                        const float gy = ly + static_cast<float>(dy);
                        const float ix =
                            sampleAt(previousGray, gx + 0.5f, gy, level) -
                            sampleAt(previousGray, gx - 0.5f, gy, level);
                        const float iy =
                            sampleAt(previousGray, gx, gy + 0.5f, level) -
                            sampleAt(previousGray, gx, gy - 0.5f, level);
                        const float it = sampleAt(currentGray, gx, gy, level) -
                                         sampleAt(previousGray, gx, gy, level);
                        sumIxIx += ix * ix;
                        sumIyIy += iy * iy;
                        sumIxIy += ix * iy;
                        sumIxIt += ix * it;
                        sumIyIt += iy * it;
                    }
                }

                const float determinant = sumIxIx * sumIyIy - sumIxIy * sumIxIy;
                if (std::fabs(determinant) < 1e-6f) {
                    ok = false;
                    break;
                }
                const float dxShift = (-sumIyIy * sumIxIt + sumIxIy * sumIyIt) / determinant;
                const float dyShift = (-sumIxIx * sumIyIt + sumIxIy * sumIxIt) / determinant;
                lx += dxShift;
                ly += dyShift;
                if (std::fabs(dxShift) + std::fabs(dyShift) < 0.05f) break;
            }

            px = lx * scale;
            py = ly * scale;
            if (px < 2.0f || py < 2.0f || px > static_cast<float>(width - 3) ||
                py > static_cast<float>(height - 3)) {
                ok = false;
            }
        }

        if (ok) {
            track.current = {px, py};
            track.valid = true;
            ++tracked;
        }
    }
    return tracked;
}

}  // namespace splatscan